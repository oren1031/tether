#include "backend.h"

#include "serialports.h"
#include "credentials.h"

#ifndef TETHER_MIRROR
#define TETHER_MIRROR ""
#endif
#ifdef TETHER_LIBSSH
#include "sshsession.h"
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <functional>

namespace {

// Hosts, users, and jump hosts never start with '-', so nothing typed into a
// field can be read by ssh as an option.
const QRegularExpression hostPattern(R"(^[A-Za-z0-9_.:%\[\]][A-Za-z0-9_.:%\[\]-]*$)");
const QRegularExpression userPattern(R"(^[A-Za-z0-9_.][A-Za-z0-9_.@-]*$)");
const QRegularExpression jumpPattern(R"(^[A-Za-z0-9_.@:%\[\]][A-Za-z0-9_.@:%\[\],-]*$)");
const QRegularExpression optionPattern(R"(^([A-Za-z][A-Za-z0-9]*)\s*(?:=|\s)\s*(\S.*)$)");
const QString endpoint = R"((?:\[[0-9A-Fa-f:.]+\]|[A-Za-z0-9_.*][A-Za-z0-9_.-]*))";
const QRegularExpression forwardPattern("^(?:" + endpoint + ":)?\\d{1,5}:" + endpoint + ":\\d{1,5}$");
const QRegularExpression socksPattern("^(?:" + endpoint + ":)?\\d{1,5}$");
#ifdef Q_OS_WIN
const QRegularExpression devicePattern(R"(^(COM\d{1,3}|\\\\\.\\COM\d{1,3})$)", QRegularExpression::CaseInsensitiveOption);
#else
const QRegularExpression devicePattern(R"(^/dev/[A-Za-z0-9_./:+-]+$)");
#endif

QString expandHome(const QString &path) {
    if (path == "~")
        return QDir::homePath();
    return path.startsWith("~/") ? QDir::homePath() + path.mid(1) : path;
}

QStringList optionLines(const QVariant &value) {
    QStringList lines;
    for (const QString &line : value.toString().split('\n'))
        if (!line.trimmed().isEmpty())
            lines << line.trimmed();
    return lines;
}

// Sync: items carry when they were last saved, so of two computers' edits
// the newer one is kept. A stamp from the future (a wrong clock, or a file
// written to always win) counts as now.
qint64 modifiedOf(const QVariantMap &item) {
    return std::min<qint64>(item.value("modified").toLongLong(), QDateTime::currentMSecsSinceEpoch() + 300000);
}

// Items are compared by what they say, not by when they were saved.
QVariantMap withoutStamp(QVariantMap item) {
    item.remove("modified");
    return item;
}

QVariantMap withStamp(QVariantMap item) {
    item.insert("modified", QDateTime::currentMSecsSinceEpoch());
    return item;
}

// ssh options that run a program on this computer, load code into ssh, or
// turn off the checks of who the server is. Lower case; ssh ignores case.
const QStringList riskyOptionNames{
    "proxycommand", "localcommand", "permitlocalcommand", "knownhostscommand", "stricthostkeychecking",
    "userknownhostsfile", "globalknownhostsfile", "pkcs11provider", "securitykeyprovider", "xauthlocation",
    "forwardagent", "nohostauthenticationforlocalhost", "hostkeyalias", "verifyhostkeydns"};

// What a synced copy may not change on its own: where a host connects, as
// whom, with what options, and what is typed into it.
const QHash<QString, QStringList> guardedFields{
    {"host", {"kind", "host", "port", "user", "options", "jumpHost", "identityFile", "identityId", "forwards",
              "startupSnippets", "device"}},
    {"snippet", {"text", "run"}},
    {"login", {"user", "identityFile"}}};

bool guardedDiffers(const QString &type, const QVariantMap &a, const QVariantMap &b) {
    for (const QString &field : guardedFields.value(type))
        if (a.value(field) != b.value(field))
            return true;
    return false;
}

int indexOfId(const QVariantList &list, const QString &id) {
    for (int i = 0; i < list.size(); ++i)
        if (list[i].toMap().value("id").toString() == id)
            return i;
    return -1;
}

// Only the fields a snippet or a login has, whatever file they came from.
QVariantMap cleanSnippet(const QVariantMap &s) {
    QVariantMap out{{"id", s.value("id").toString()}, {"name", s.value("name").toString().trimmed()},
                    {"text", s.value("text").toString()}, {"run", s.value("run").toBool()}};
    if (s.contains("modified"))
        out.insert("modified", s.value("modified").toLongLong());
    return out;
}

QVariantMap cleanLogin(const QVariantMap &l) {
    QVariantMap out{{"id", l.value("id").toString()}, {"name", l.value("name").toString().trimmed()},
                    {"user", l.value("user").toString().trimmed()},
                    {"identityFile", l.value("identityFile").toString().trimmed()},
                    {"hasPassword", l.value("hasPassword").toBool()}};
    if (l.contains("modified"))
        out.insert("modified", l.value("modified").toLongLong());
    return out;
}

// What's wrong with a vault login, or nothing: for the dialog and for sync.
QString loginProblem(const QVariantMap &login) {
    if (login.value("name").toString().isEmpty())
        return Backend::tr("Give the login a name.");
    const QString user = login.value("user").toString();
    if (!user.isEmpty() && !userPattern.match(user).hasMatch())
        return Backend::tr("“%1” isn't a valid user name.").arg(user);
    if (login.value("identityFile").toString().contains('\n'))
        return Backend::tr("The key file path can't span lines.");
    return {};
}

QString safeFileName(const QString &name) {
    QString safe = name;
    safe.replace(QRegularExpression("[^A-Za-z0-9._-]+"), "_");
    safe = safe.left(40);
    return safe.isEmpty() || safe.startsWith('.') ? "session" : safe;
}

} // namespace

Backend::Backend(const QString &configFile, const QString &sshConfig, const QString &logDirectory, QObject *parent)
    : QObject(parent),
      m_configFile(!configFile.isEmpty() ? configFile
                   : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/tether/tether.json"),
      m_sshConfig(!sshConfig.isEmpty() ? sshConfig : QDir::homePath() + "/.ssh/config"),
      m_logDirectory(!logDirectory.isEmpty() ? logDirectory
                     : QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/tether/logs") {
    load();
    connect(&m_sshWatcher, &QFileSystemWatcher::fileChanged, this, &Backend::reloadSshConfig);
    connect(&m_sshWatcher, &QFileSystemWatcher::directoryChanged, this, &Backend::reloadSshConfig);
    reloadSshConfig();
    connect(&m_syncWatcher, &QFileSystemWatcher::fileChanged, this, [this] { QTimer::singleShot(300, this, [this] { readSyncFile(); watchSyncFile(); }); });
    connect(&m_syncWatcher, &QFileSystemWatcher::directoryChanged, this, [this] { QTimer::singleShot(300, this, [this] { readSyncFile(); watchSyncFile(); }); });
    readSyncFile();
    watchSyncFile();
}

QString Backend::tool(const QString &name) const {
    return m_tools.contains(name) ? m_tools.value(name) : QStandardPaths::findExecutable(name);
}

void Backend::setFontSize(int size) {
    size = std::clamp(size, 7, 40);
    if (size == m_fontSize)
        return;
    m_fontSize = size;
    save();
    emit fontSizeChanged();
}

QVariantMap Backend::defaultSettings() {
    return {{"accent", "blue"},           // a name from Style.accents, or "omarchy"
            {"interfaceFont", ""},        // empty picks Inter, then Noto Sans
            {"interfaceFontSize", 13},
            {"language", ""},             // empty follows the system; else a catalog code ("he")
            {"terminalScheme", "omarchy"}, // "omarchy" or a scheme the terminal ships
            {"terminalFont", ""},         // empty is the system monospace font
            {"lineSpacing", 0},
            {"blinkingCursor", false},
            {"boldText", true},
            {"paneDimming", true},
            {"shell", ""},                // empty uses $SHELL
            {"confirmQuit", true},
            {"confirmPaste", true},       // ask before a paste that would run several lines at once
            {"localTmux", false},         // local shells run inside tmux, so they outlive Tether
            {"shareConnections", true},   // ssh ControlMaster: one login per host for panes and files
            {"syncFolder", ""},
            {"checkUpdates", true},
            {"aiProvider", "auto"},
            {"updateMirror", TETHER_MIRROR}};      // auto, api, or claude-code
}

QVariantMap Backend::settings() const {
    QVariantMap all = defaultSettings();
    for (auto it = m_settings.cbegin(); it != m_settings.cend(); ++it)
        all.insert(it.key(), it.value());
    return all;
}

QString Backend::setSetting(const QString &key, const QVariant &value) {
    const QVariantMap defaults = defaultSettings();
    if (!defaults.contains(key))
        return tr("There's no setting called %1.").arg(key);
    const QVariant fallback = defaults.value(key);
    QVariant v = value;
    if (fallback.typeId() == QMetaType::Int) {
        bool ok = false;
        const int n = v.toInt(&ok);
        if (!ok)
            return tr("%1 needs a number.").arg(key);
        v = key == "interfaceFontSize" ? std::clamp(n, 10, 18) : key == "lineSpacing" ? std::clamp(n, 0, 12) : n;
    } else if (fallback.typeId() == QMetaType::Bool) {
        v = v.toBool();
    } else {
        v = v.toString().trimmed();
    }
    if (key == "syncFolder" && !v.toString().isEmpty()) {
        const QFileInfo folder(expandHome(v.toString()));
        if (!folder.isDir() || !folder.isWritable())
            return tr("%1 isn't a folder Tether can write to.").arg(v.toString());
        v = folder.absoluteFilePath();
    }
    if (key == "shell" && !v.toString().isEmpty()) {
        const QFileInfo shell(expandHome(v.toString()));
        if (!shell.isFile() || !shell.isExecutable())
            return tr("%1 isn't a program that can be run.").arg(v.toString());
    }
    if (settings().value(key) == v)
        return {};
    if (v == fallback)
        m_settings.remove(key);
    else
        m_settings.insert(key, v);
    emit settingsChanged();
    if (key == "syncFolder") {
        // Joining a folder that already has hosts takes them in; then both
        // sides hold the union.
        readSyncFile();
        watchSyncFile();
    }
    return save();
}

QString Backend::resetSettings() {
    if (m_settings.isEmpty())
        return {};
    m_settings.clear();
    emit settingsChanged();
    return save();
}

QString Backend::validate(const QVariantMap &p) {
    const QString kind = p.value("kind").toString();
    if (p.value("name").toString().trimmed().isEmpty())
        return tr("Give it a name.");

    if (kind == "ssh") {
        const QString host = p.value("host").toString().trimmed();
        if (host.isEmpty())
            return tr("Enter a host name or address.");
        if (!hostPattern.match(host).hasMatch())
            return tr("“%1” isn't a valid host name or address.").arg(host);
        const QString user = p.value("user").toString().trimmed();
        if (!user.isEmpty() && !userPattern.match(user).hasMatch())
            return tr("“%1” isn't a valid user name.").arg(user);
        const int port = p.value("port", 22).toInt();
        if (port < 1 || port > 65535)
            return tr("The port must be between 1 and 65535.");
        const QString jump = p.value("jumpHost").toString().trimmed();
        if (!jump.isEmpty() && !jumpPattern.match(jump).hasMatch())
            return tr("“%1” isn't a valid jump host.").arg(jump);
        if (p.value("identityFile").toString().contains('\n'))
            return tr("The key file path can't span lines.");
        for (const QString &line : optionLines(p.value("options")))
            if (!optionPattern.match(line).hasMatch())
                return tr("“%1” isn't an ssh option. Write one per line, like ServerAliveInterval=30.").arg(line);
        for (const QVariant &value : p.value("forwards").toList()) {
            const QVariantMap forward = value.toMap();
            const QString type = forward.value("type").toString(), spec = forward.value("spec").toString().trimmed();
            const bool valid = (type == "L" || type == "R") ? forwardPattern.match(spec).hasMatch()
                             : type == "D" ? socksPattern.match(spec).hasMatch() : false;
            if (!valid)
                return type == "D" ? tr("“%1” isn't a SOCKS proxy port. Use port or address:port, like 1080.").arg(spec)
                                   : tr("“%1” isn't a forward. Use port:host:hostport, like 8080:localhost:80.").arg(spec);
        }
        const QString transport = p.value("transport", "ssh").toString();
        if (transport != "ssh" && transport != "mosh")
            return tr("Connect with ssh or mosh.");
        if (transport == "mosh" && !p.value("forwards").toList().isEmpty())
            return tr("mosh can't carry port forwards. Use ssh for this host, or remove the forwards.");
        return {};
    }

    if (kind == "serial") {
        const QString device = p.value("device").toString().trimmed();
        if (!devicePattern.match(device).hasMatch() || device.contains(".."))
            return tr("Choose a serial device, like %1.").arg(exampleDevice());
        const int baud = p.value("baud", 115200).toInt();
        if (baud < 50 || baud > 16000000)
            return tr("The baud rate must be between 50 and 16000000.");
        const int dataBits = p.value("dataBits", 8).toInt();
        if (dataBits < 5 || dataBits > 8)
            return tr("Data bits must be 5, 6, 7, or 8.");
        const int stopBits = p.value("stopBits", 1).toInt();
        if (stopBits != 1 && stopBits != 2)
            return tr("Stop bits must be 1 or 2.");
        if (!QStringList{"none", "even", "odd"}.contains(p.value("parity", "none").toString()))
            return tr("Parity must be none, even, or odd.");
        if (!QStringList{"none", "hardware", "software"}.contains(p.value("flow", "none").toString()))
            return tr("Flow control must be none, hardware, or software.");
        if (!QStringList{"cr", "lf", "crlf"}.contains(p.value("enter", "cr").toString()))
            return tr("Enter must send CR, LF, or CR LF.");
        return {};
    }

    return tr("Unknown connection type.");
}

// Only the fields a profile of its kind uses are kept, with defaults filled in.
QVariantMap Backend::clean(const QVariantMap &p) {
    const QString kind = p.value("kind").toString();
    QVariantMap out{{"id", p.value("id").toString()}, {"kind", kind},
                    {"name", p.value("name").toString().trimmed()}, {"log", p.value("log").toBool()}};
    if (kind == "ssh") {
        QVariantList forwards;
        for (const QVariant &value : p.value("forwards").toList()) {
            const QVariantMap forward = value.toMap();
            forwards << QVariantMap{{"type", forward.value("type").toString()},
                                    {"spec", forward.value("spec").toString().trimmed()}};
        }
        out.insert({{"host", p.value("host").toString().trimmed()},
                    {"user", p.value("user").toString().trimmed()},
                    {"port", p.value("port", 22).toInt()},
                    {"identityFile", p.value("identityFile").toString().trimmed()},
                    {"jumpHost", p.value("jumpHost").toString().trimmed()},
                    {"options", optionLines(p.value("options")).join('\n')},
                    {"forwards", forwards},
                    {"identityId", p.value("identityId").toString()},
                    {"transport", p.value("transport", "ssh").toString()},
                    {"persistent", p.value("persistent").toBool()},
                    {"reconnect", p.value("reconnect", true).toBool()}});
    } else if (kind == "serial") {
        out.insert({{"device", p.value("device").toString().trimmed()},
                    {"baud", p.value("baud", 115200).toInt()},
                    {"dataBits", p.value("dataBits", 8).toInt()},
                    {"parity", p.value("parity", "none").toString()},
                    {"stopBits", p.value("stopBits", 1).toInt()},
                    {"flow", p.value("flow", "none").toString()},
                    {"enter", p.value("enter", "cr").toString()},
                    {"echo", p.value("echo").toBool()}});
    }
    QStringList startup;
    for (const QVariant &id : p.value("startupSnippets").toList())
        if (!id.toString().isEmpty())
            startup << id.toString();
    if (kind == "ssh" || kind == "serial")
        out.insert("startupSnippets", startup);
    // A pane split from a kept-alive session gets its own tmux session.
    if (!p.value("tmuxSession").toString().isEmpty())
        out.insert("tmuxSession", p.value("tmuxSession").toString());
    // When it was last saved, for sync to keep the newer of two edits.
    if (p.contains("modified"))
        out.insert("modified", p.value("modified").toLongLong());
    return out;
}

QString Backend::shellQuote(const QString &argument) {
    static const QRegularExpression plain(R"(^[A-Za-z0-9_./=:@%+,-]+$)");
    if (plain.match(argument).hasMatch())
        return argument;
    QString quoted = argument;
    quoted.replace('\'', "'\\''");
    return "'" + quoted + "'";
}

QVariantMap Backend::launch(const QVariantMap &profile) const {
    const QString kind = profile.value("kind").toString();
    QString program, title = profile.value("name").toString().trimmed();
    QStringList args;
    bool splittable = true;

    QStringList environment;
    if (kind == "local") {
        program = expandHome(settings().value("shell").toString());
#ifdef Q_OS_WIN
        // PowerShell 7 if it's installed, then Windows PowerShell, then cmd.
        if (program.isEmpty())
            program = QStandardPaths::findExecutable("pwsh");
        if (program.isEmpty())
            program = QStandardPaths::findExecutable("powershell");
        if (program.isEmpty())
            program = qEnvironmentVariable("COMSPEC", "cmd.exe");
#else
        if (program.isEmpty())
            program = qEnvironmentVariable("SHELL");
#ifdef Q_OS_ANDROID
        // Android's shell lives in /system, and apps can run it.
        if (program.isEmpty() || !QFileInfo(program).isExecutable())
            program = "/system/bin/sh";
#endif
        if (program.isEmpty() || !QFileInfo(program).isExecutable())
            program = "/bin/sh";
#endif
        title = tr("Local");
        // Inside tmux, the shell keeps running if Tether closes or crashes,
        // and shows up under Detached sessions to pick up again.
        if (settings().value("localTmux").toBool() && !tool("tmux").isEmpty()) {
            const QString name = "tether-" + QDateTime::currentDateTime().toString("MMdd-HHmmss-zzz");
            args = {"new-session", "-s", name, program};
            program = tool("tmux");
        }
    } else if (kind == "tmux") {
        const QString name = profile.value("session").toString();
        if (!name.startsWith("tether-") || name.contains(QRegularExpression("[^A-Za-z0-9_.:-]")))
            return {{"error", tr("That isn't one of Tether's tmux sessions.")}};
        if (tool("tmux").isEmpty())
            return {{"error", tr("tmux isn't installed.")}};
        program = tool("tmux");
        args = {"attach-session", "-t", name};
        title = tr("Local");
    } else {
        const QString error = validate(profile);
        if (!error.isEmpty())
            return {{"error", error}};
        const QVariantMap p = clean(profile);

        if (kind == "ssh") {
            program = tool("ssh");
#ifdef TETHER_LIBSSH
            // No ssh program, as on Android: Tether connects itself.
            if (program.isEmpty())
                return builtInSsh(p, title, profile);
#endif
            if (program.isEmpty())
#ifdef Q_OS_WIN
                return {{"error", tr("ssh isn't installed. Turn on OpenSSH Client in Windows Settings → System → Optional features.")}};
#else
                return {{"error", tr("ssh isn't installed. Install the openssh package (openssh-client on Debian and Ubuntu).")}};
#endif
            const QVariantMap r = resolved(p);
            const bool mosh = p["transport"] == "mosh";
            if (mosh && tool("mosh").isEmpty())
                return {{"error", tr("mosh isn't installed. Install the mosh package, or connect this host with ssh.")}};
            environment = askpassEnvironment(p, true);
            if (p["port"].toInt() != 22)
                args << "-p" << QString::number(p["port"].toInt());
            if (!r["user"].toString().isEmpty())
                args << "-l" << r["user"].toString();
            if (!r["identityFile"].toString().isEmpty())
                args << "-i" << expandHome(r["identityFile"].toString());
            if (!p["jumpHost"].toString().isEmpty())
                args << "-J" << p["jumpHost"].toString();
            for (const QVariant &value : p["forwards"].toList())
                args << "-" + value.toMap()["type"].toString() << value.toMap()["spec"].toString();
            for (const QString &line : optionLines(p["options"])) {
                const auto match = optionPattern.match(line);
                args << "-o" << match.captured(1) + "=" + match.captured(2).trimmed();
            }
            // On Windows, where tether.exe answers ssh's questions for a host
            // with a saved password, "trust this new host?" is shown as a
            // dialog with the fingerprint (Credentials::askpass); nothing is
            // accepted without being asked.
            // After the user's options, since ssh keeps the first value it
            // reads for each. Keepalives notice a dead link in under a minute.
            if (p["persistent"].toBool() || p["reconnect"].toBool())
                args << "-o" << "ServerAliveInterval=15" << "-o" << "ServerAliveCountMax=3";
            // One login per host: panes, the file browser, and reconnects
            // reuse it, so a password is typed once.
            if (sharesConnections() && !mosh)
                args << "-o" << "ControlMaster=auto" << "-o" << "ControlPath=" + controlPath()
                     << "-o" << "ControlPersist=10m";

            // tmux on the server keeps the shell and everything in it running
            // through a dropped connection; reconnecting reattaches.
            QString remote;
            if (p["persistent"].toBool()) {
                const QString session = tmuxSessionName(p.value("tmuxSession", p["name"]).toString());
                remote = QString("tmux new-session -A -s %1 2>/dev/null || exec \"$SHELL\" -l").arg(session);
            }

            if (mosh) {
                // mosh logs in over ssh with these options, then keeps the
                // session alive across sleep, roaming, and changing networks.
                QStringList sshCommand{shellQuote(program)};
                for (const QString &arg : args)
                    sshCommand << shellQuote(arg);
                const QString user = r["user"].toString();
                args = {"--ssh=" + sshCommand.join(' '), "--",
                        (user.isEmpty() ? QString() : user + "@") + p["host"].toString()};
                if (!remote.isEmpty())
                    args << "sh" << "-c" << remote;
                program = tool("mosh");
            } else {
                // "--" ends ssh's options, so the destination is only ever a host.
                args << "--" << p["host"].toString();
                if (!remote.isEmpty()) {
                    args.insert(0, "-t");
                    args << remote;
                }
            }
        } else {
            const QString device = p["device"].toString();
            const QString problem = SerialPorts::problem(device);
            if (!problem.isEmpty())
                return {{"error", problem}};
            program = tool("picocom");
            // Where there's no picocom, as on Windows and Android, Tether
            // opens the port itself.
            if (program.isEmpty() && SerialPorts::builtIn())
                return {{"serial", p}, {"title", title}, {"splittable", false}, {"profile", profile},
                        {"logPath", profile.value("log").toBool() ? newLogPath(title) : QString()}};
            if (program.isEmpty())
                return {{"error", tr("picocom isn't installed. Tether uses it to talk to serial ports; install the picocom package.")}};
            args << "--baud" << QString::number(p["baud"].toInt())
                 << "--databits" << QString::number(p["dataBits"].toInt())
                 << "--parity" << p["parity"].toString().left(1)
                 << "--stopbits" << QString::number(p["stopBits"].toInt())
                 << "--flow" << p["flow"].toString().left(1);
            // picocom sends CR for Enter; these maps make it LF or CR LF.
            if (p["enter"] == "lf")
                args << "--omap" << "crlf";
            else if (p["enter"] == "crlf")
                args << "--omap" << "crcrlf";
            if (p["echo"].toBool())
                args << "--echo";
            args << device;
            // One process owns a serial port, so a split beside it opens a local shell.
            splittable = false;
        }
    }

    // The terminal writes the log itself, so it works the same everywhere.
    const QString logPath = profile.value("log").toBool() && kind != "local" ? newLogPath(title) : QString();

    // ssh exits with 255 when the connection fails or drops; the pane then
    // reconnects on its own if the host asks for that.
    const bool reconnect = kind == "ssh" && profile.value("reconnect", true).toBool()
                           && profile.value("transport", "ssh").toString() == "ssh";
    return {{"program", program}, {"args", args}, {"environment", environment}, {"title", title},
            {"workingDirectory", QDir::homePath()}, {"logPath", logPath}, {"splittable", splittable},
            {"reconnect", reconnect}, {"profile", profile}};
}

#ifdef TETHER_LIBSSH
QVariantMap Backend::builtInSsh(const QVariantMap &p, const QString &title, const QVariantMap &profile) const {
    if (p["transport"] == "mosh")
        return {{"error", tr("mosh isn't available here; connect this host with ssh.")}};
    if (!p["jumpHost"].toString().isEmpty() || !p["forwards"].toList().isEmpty())
        return {{"error", tr("Jump hosts and port forwarding need the ssh program, which this system doesn't have.")}};
    const QVariantMap r = resolved(p);
    QVariantMap ssh{{"host", p["host"]}, {"port", p["port"]}, {"user", r["user"]},
                    {"identityFile", expandHome(r["identityFile"].toString())},
                    {"knownHosts", QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/known_hosts"}};
    if (p["persistent"].toBool())
        ssh.insert("command", QString("tmux new-session -A -s %1 2>/dev/null || exec \"$SHELL\" -l")
                                  .arg(tmuxSessionName(p.value("tmuxSession", p["name"]).toString())));
    const QString logPath = profile.value("log").toBool() ? newLogPath(title) : QString();
    return {{"ssh", ssh}, {"title", title}, {"logPath", logPath}, {"splittable", true},
            {"reconnect", profile.value("reconnect", true).toBool()}, {"profile", profile}};
}
#endif

QVariantMap Backend::importKey(const QUrl &url) {
    // On a desktop the key stays where it is; on Android, where apps can't
    // read other apps' files by path, it's copied into Tether's own storage.
#ifdef Q_OS_ANDROID
    QFile source(url.isLocalFile() ? url.toLocalFile() : url.toString());
    if (!source.open(QIODevice::ReadOnly))
        return {{"error", tr("Couldn't read that file: %1").arg(source.errorString())}};
    const QByteArray key = source.readAll();
    if (!key.contains("PRIVATE KEY"))
        return {{"error", tr("That doesn't look like a private key.")}};
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/keys";
    QDir().mkpath(directory);
    QString name = QFileInfo(url.path()).fileName();
    if (name.isEmpty() || name.contains('%') || name.contains(':'))
        name = "key-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    QSaveFile file(directory + "/" + name);
    if (!file.open(QIODevice::WriteOnly) || file.write(key) != key.size() || !file.commit())
        return {{"error", tr("Couldn't save the key: %1").arg(file.errorString())}};
    QFile::setPermissions(file.fileName(), QFile::ReadOwner | QFile::WriteOwner);
    return {{"path", file.fileName()}};
#else
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    const QString home = QDir::homePath();
    return {{"path", path.startsWith(home + "/") ? "~" + path.mid(home.size()) : path}};
#endif
}

void Backend::runTool(const QString &program, const QStringList &args, const QByteArray &input, int timeoutMs,
                      const std::function<void(bool, const QString &)> &done) {
    auto *process = new QProcess(this);
    auto *limit = new QTimer(process);
    limit->setSingleShot(true);
    connect(limit, &QTimer::timeout, process, &QProcess::kill);
    // A program that isn't there reports an error and never finishes; one
    // that is killed for taking too long finishes as crashed.
    auto report = [process, done](bool ok) {
        if (process->property("reported").toBool())
            return;
        process->setProperty("reported", true);
        const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
        process->deleteLater();
        if (done)
            done(ok, error);
    };
    connect(process, &QProcess::finished, this, [report](int code, QProcess::ExitStatus status) {
        report(status == QProcess::NormalExit && code == 0);
    });
    connect(process, &QProcess::errorOccurred, this, [report](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            report(false);
    });
    process->start(program, args);
    if (!input.isEmpty())
        process->write(input);
    process->closeWriteChannel();
    limit->start(timeoutMs);
}

void Backend::forgetHostKey(const QVariantMap &profile, const QString &token) {
    // Always answered after this call returns, so callers have one path.
    auto answer = [this, token](const QString &error) {
        QMetaObject::invokeMethod(this, [this, token, error] { emit hostKeyForgotten(token, error); }, Qt::QueuedConnection);
    };
    const QVariantMap p = clean(profile);
    const QString host = p["host"].toString();
    const int port = p["port"].toInt();
    if (host.isEmpty() || !hostPattern.match(host).hasMatch())
        return answer(tr("No host to forget."));
#ifdef TETHER_LIBSSH
    if (tool("ssh").isEmpty()) {
        const QString file = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/known_hosts";
        return answer(SshSession::forgetHost(file, host, port) ? QString() : tr("There was no saved key for %1.").arg(host));
    }
#endif
    // ssh-keygen knows hashed known_hosts lines too.
    const QString keygen = tool("ssh-keygen");
    if (keygen.isEmpty())
        return answer(tr("ssh-keygen isn't installed."));
    runTool(keygen, {"-R", port == 22 ? host : QString("[%1]:%2").arg(host).arg(port)}, {}, 10000,
            [this, token](bool ok, const QString &error) {
                emit hostKeyForgotten(token, ok ? QString() : tr("Couldn't remove the old key: %1").arg(error));
            });
}

QString Backend::newLogPath(const QString &title) const {
    QDir().mkpath(m_logDirectory);
    QFile::setPermissions(m_logDirectory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QString stem = m_logDirectory + "/" + safeFileName(title) + "-"
                       + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    QString path = stem + ".log";
    for (int n = 2; QFile::exists(path); ++n)
        path = QString("%1-%2.log").arg(stem).arg(n);
    return path;
}

// Windows' OpenSSH can't share connections (no ControlMaster there).
bool Backend::sharesConnections() const {
#ifdef Q_OS_WIN
    return false;
#else
    return settings().value("shareConnections").toBool();
#endif
}

QVariantMap Backend::quickConnect(const QString &text) const {
    const QString input = text.trimmed();
    QVariantMap p;
    static const QRegularExpression comPort(R"(^COM\d{1,3}(\s|$))", QRegularExpression::CaseInsensitiveOption);
    if (input.startsWith("/dev/") || comPort.match(input).hasMatch()) {
        const QStringList parts = input.split(QRegularExpression("\\s+"));
        p = {{"kind", "serial"}, {"name", parts[0]}, {"device", parts[0]},
             {"baud", parts.size() > 1 ? parts[1].toInt() : 115200}};
        if (parts.size() > 2)
            return {{"error", tr("Type a device and an optional baud rate, like %1 9600.").arg(exampleDevice())}};
    } else {
        QString host = input, user;
        int port = 22;
        if (const int at = host.lastIndexOf('@'); at > 0) {
            user = host.left(at);
            host = host.mid(at + 1);
        }
        // host:port, but not a bare IPv6 address, which has several colons.
        if (host.count(':') == 1) {
            bool ok = false;
            port = host.section(':', 1).toInt(&ok);
            if (!ok)
                return {{"error", tr("“%1” isn't a valid port.").arg(host.section(':', 1))}};
            host = host.section(':', 0, 0);
        }
        p = {{"kind", "ssh"}, {"name", input}, {"host", host}, {"user", user}, {"port", port}};
    }
    const QString error = validate(p);
    return error.isEmpty() ? clean(p) : QVariantMap{{"error", error}};
}


// ── Sessions ───────────────────────────────────────────────────────────────

QString Backend::tmuxSessionName(const QString &name) {
    QString slug = name.toLower();
    slug.replace(QRegularExpression("[^a-z0-9_-]+"), "-");
    slug.replace(QRegularExpression("^-+|-+$"), "");
    return "tether-" + (slug.isEmpty() ? QString("session") : slug.left(40));
}

QString Backend::controlPath() const {
    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty())
        runtime = QDir::tempPath();
    const QString directory = runtime + "/tether";
    QDir().mkpath(directory);
    QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    // %C is a hash of the host, port, and user, short enough for a socket path.
    return directory + "/cm-%C";
}

void Backend::refreshTmux() {
    const QString tmux = tool("tmux");
    if (tmux.isEmpty() || m_tmuxProcess.state() != QProcess::NotRunning)
        return;
    if (!m_tmuxProcess.property("connected").toBool()) {
        m_tmuxProcess.setProperty("connected", true);
        connect(&m_tmuxProcess, &QProcess::finished, this, [this](int code) {
            QVariantList sessions;
            if (code == 0) {
                for (const QString &line : QString::fromUtf8(m_tmuxProcess.readAllStandardOutput()).split('\n')) {
                    const QStringList f = line.split('\t');
                    if (f.size() < 4 || !f[0].startsWith("tether-") || f[1] != "0")
                        continue;
                    sessions << QVariantMap{{"name", f[0]},
                                            {"created", QDateTime::fromSecsSinceEpoch(f[2].toLongLong()).toString("d MMM, HH:mm")},
                                            {"path", f[3].startsWith(QDir::homePath()) ? "~" + f[3].mid(QDir::homePath().size()) : f[3]}};
                }
            }
            m_tmuxProcess.readAllStandardError();
            if (sessions != m_tmuxSessions) {
                m_tmuxSessions = sessions;
                emit tmuxSessionsChanged();
            }
        });
    }
    m_tmuxProcess.start(tmux, {"list-sessions", "-F",
                               "#{session_name}\t#{session_attached}\t#{session_created}\t#{pane_current_path}"});
}

void Backend::checkConnection(const QVariantMap &profile, const QString &token) {
    const QVariantMap p = resolved(clean(profile));
    if (p["kind"] != "ssh" || p["transport"] == "mosh" || !sharesConnections()) {
        QMetaObject::invokeMethod(this, [this, token] { emit connectionChecked(token, true); }, Qt::QueuedConnection);
        return;
    }
    // One question per pane at a time; a slow answer isn't asked again.
    if (m_checking.contains(token))
        return;
    QStringList args{"-O", "check", "-o", "ControlPath=" + controlPath(), "-p", QString::number(p["port"].toInt())};
    if (!p["user"].toString().isEmpty())
        args << "-l" << p["user"].toString();
    args << "--" << p["host"].toString();
    m_checking << token;
    runTool(tool("ssh"), args, {}, 1500, [this, token](bool ok, const QString &) {
        m_checking.removeAll(token);
        emit connectionChecked(token, ok);
    });
}

QVariantMap Backend::sftpCommand(const QVariantMap &profile) const {
    if (profile.value("kind") != "ssh")
        return {{"error", tr("Files are only for ssh hosts.")}};
    if (const QString error = validate(profile); !error.isEmpty())
        return {{"error", error}};
    const QString sftp = tool("sftp");
    if (sftp.isEmpty())
        return {{"error", tr("sftp isn't installed; it comes with openssh.")}};
    const QVariantMap p = resolved(clean(profile));
    QStringList args{"-q", "-b", "-", "-o", "Port=" + QString::number(p["port"].toInt()),
                     "-o", "ConnectTimeout=15"};
    if (!p["user"].toString().isEmpty())
        args << "-o" << "User=" + p["user"].toString();
    if (!p["identityFile"].toString().isEmpty())
        args << "-o" << "IdentityFile=" + expandHome(p["identityFile"].toString());
    if (!p["jumpHost"].toString().isEmpty())
        args << "-o" << "ProxyJump=" + p["jumpHost"].toString();
    for (const QString &line : optionLines(p["options"])) {
        const auto match = optionPattern.match(line);
        if (match.captured(1).compare("RemoteCommand", Qt::CaseInsensitive) != 0
            && match.captured(1).compare("RequestTTY", Qt::CaseInsensitive) != 0)
            args << "-o" << match.captured(1) + "=" + match.captured(2).trimmed();
    }
    if (sharesConnections())
        args << "-o" << "ControlMaster=auto" << "-o" << "ControlPath=" + controlPath() << "-o" << "ControlPersist=10m";
    args << "--" << p["host"].toString();
    return {{"program", sftp}, {"args", args}, {"environment", askpassEnvironment(p, false)},
            {"title", p["name"].toString()}};
}

// ── Vault ──────────────────────────────────────────────────────────────────

QVariantMap Backend::identity(const QString &id) const {
    if (id.isEmpty())
        return {};
    for (const QVariant &value : m_identities)
        if (value.toMap().value("id") == id)
            return value.toMap();
    return {};
}

// A profile with its vault login filled in where its own fields are blank.
QVariantMap Backend::resolved(const QVariantMap &profile) const {
    QVariantMap p = profile;
    const QVariantMap login = identity(p.value("identityId").toString());
    if (login.isEmpty())
        return p;
    if (p.value("user").toString().isEmpty())
        p["user"] = login.value("user");
    if (p.value("identityFile").toString().isEmpty())
        p["identityFile"] = login.value("identityFile");
    return p;
}

QStringList Backend::askpassEnvironment(const QVariantMap &profile, bool terminal) const {
    const QVariantMap login = identity(profile.value("identityId").toString());
    const bool password = login.value("hasPassword").toBool() && keyringAvailable();
    if (!password && terminal)
        return {};
    const QString helper = askpass();
    if (helper.isEmpty())
        return {};
    QStringList env{"SSH_ASKPASS=" + QDir::toNativeSeparators(helper), "SSH_ASKPASS_REQUIRE=force"};
#ifdef Q_OS_WIN
    env << "TETHER_ASKPASS=1";
#endif
    if (password)
        env << "TETHER_IDENTITY=" + login.value("id").toString();
    // The file browser has no terminal to ask in; it fails instead of hanging.
    if (!terminal)
        env << "TETHER_ASKPASS_NOTTY=1";
    return env;
}

// ssh runs this for every question it would ask. Password and passphrase
// prompts get the vault's secret, once per ssh process so a wrong one
// doesn't lock the account; everything else, like confirming a new host
// key, is asked in the terminal as ssh would.
static const char askpassScript[] = R"sh(#!/bin/sh
prompt="$1"
dir=@DIR@
mark="$dir/askpass-$PPID"
find "$dir" -maxdepth 1 -name 'askpass-*' -mmin +120 -delete 2>/dev/null
case "$prompt" in
*assword*|*assphrase*|*PIN*)
    if [ -n "$TETHER_IDENTITY" ] && [ ! -e "$mark" ]; then
        : > "$mark"
        secret=$(secret-tool lookup service tether identity "$TETHER_IDENTITY" 2>/dev/null)
        if [ -n "$secret" ]; then
            printf '%s\n' "$secret"
            exit 0
        fi
    fi ;;
esac
[ -n "$TETHER_ASKPASS_NOTTY" ] && exit 1
{ : < /dev/tty; } 2>/dev/null || exit 1
printf '%s' "$prompt" > /dev/tty
case "$prompt" in
*assword*|*assphrase*|*PIN*)
    stty -echo < /dev/tty
    IFS= read -r answer < /dev/tty
    stty echo < /dev/tty
    printf '\r\n' > /dev/tty ;;
*) IFS= read -r answer < /dev/tty ;;
esac
printf '%s\n' "$answer"
)sh";

QString Backend::askpass() const {
#ifdef Q_OS_WIN
    // tether.exe answers itself when started with TETHER_ASKPASS=1.
    return QCoreApplication::applicationFilePath();
#endif
    // The script is told the folder Tether itself uses. Left to work it out
    // from XDG_RUNTIME_DIR, it disagrees with Qt wherever that isn't set
    // (su and sudo sessions, some containers): its "already tried the saved
    // password" mark then can't be written, and a wrong saved password is
    // offered at every prompt until the server locks the account.
    const QString directory = QFileInfo(controlPath()).absolutePath();
    const QString path = directory + "/askpass";
    const QByteArray script = QByteArray(askpassScript).replace("@DIR@", shellQuote(directory).toUtf8());
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == script)
        return path;
    existing.close();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(script) < 0 || !file.commit())
        return {};
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

QString Backend::saveIdentity(const QVariantMap &input, const QString &password, bool clearPassword) {
    QVariantMap login = cleanLogin(input);
    if (const QString problem = loginProblem(login); !problem.isEmpty())
        return problem;
    if (m_vaultBusy)
        return tr("The last login is still being saved.");
    if (login["id"].toString().isEmpty())
        login["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString id = login["id"].toString();
    login["hasPassword"] = identity(id).value("hasPassword").toBool();
    // The answer always comes after this call returns, so callers have one path.
    auto finish = [this](const QVariantMap &login) {
        QMetaObject::invokeMethod(this, [this, login] { emit identitySaved(storeIdentity(login)); }, Qt::QueuedConnection);
    };

#ifdef Q_OS_WIN
    if (!password.isEmpty()) {
        if (const QString error = Credentials::store(id, "Tether: " + login["name"].toString(), password); !error.isEmpty())
            return error;
        login["hasPassword"] = true;
    } else if (clearPassword && login["hasPassword"].toBool()) {
        Credentials::remove(id);
        login["hasPassword"] = false;
    }
#else
    const QString secretTool = tool("secret-tool");
    if (!password.isEmpty()) {
        if (secretTool.isEmpty())
            return tr("Saving passwords needs secret-tool (the libsecret package) and a keyring like GNOME Keyring or KeePassXC.");
        // A locked keyring shows its own unlock prompt and answers when the
        // user is done with it; the window stays usable meanwhile.
        m_vaultBusy = true;
        runTool(secretTool, {"store", "--label=Tether: " + login["name"].toString(), "service", "tether", "identity", id},
                password.toUtf8(), 30000, [this, login](bool ok, const QString &error) mutable {
                    m_vaultBusy = false;
                    if (!ok) {
                        emit identitySaved(tr("The keyring didn't take the password: %1")
                                               .arg(error.isEmpty() ? tr("it didn't answer in time") : error));
                        return;
                    }
                    login["hasPassword"] = true;
                    emit identitySaved(storeIdentity(login));
                });
        return {};
    } else if (clearPassword && login["hasPassword"].toBool()) {
        if (!secretTool.isEmpty())
            runTool(secretTool, {"clear", "service", "tether", "identity", id}, {}, 15000, {});
        login["hasPassword"] = false;
    }
#endif
    finish(login);
    return {};
}

// Puts a checked login in the vault's list and saves it.
QString Backend::storeIdentity(const QVariantMap &input) {
    const QVariantMap login = withStamp(input);
    const QString id = login["id"].toString();
    const int at = indexOfId(m_identities, id);
    if (at >= 0)
        m_identities[at] = login;
    else
        m_identities << login;
    // What was just saved here is newer than a synced copy still waiting.
    if (release("login", id))
        emit syncReviewChanged();
    emit identitiesChanged();
    return save();
}

QString Backend::removeIdentity(const QString &id) {
    const QVariantMap login = identity(id);
    if (login.isEmpty())
        return {};
#ifdef Q_OS_WIN
    if (login.value("hasPassword").toBool())
        Credentials::remove(id);
#else
    if (login.value("hasPassword").toBool() && !tool("secret-tool").isEmpty())
        runTool(tool("secret-tool"), {"clear", "service", "tether", "identity", id}, {}, 15000, {});
#endif
    for (int i = 0; i < m_identities.size(); ++i)
        if (m_identities[i].toMap().value("id") == id)
            m_identities.removeAt(i--);
    if (!m_removed.contains(id))
        m_removed << id;
    if (release("login", id, true))
        emit syncReviewChanged();
    // Hosts that used it go back to their own fields.
    bool profilesTouched = false;
    for (QVariant &value : m_profiles) {
        QVariantMap p = value.toMap();
        if (p.value("identityId") == id) {
            p["identityId"] = QString();
            value = withStamp(p);
            profilesTouched = true;
        }
    }
    emit identitiesChanged();
    if (profilesTouched)
        emit profilesChanged();
    return save();
}

// ── Sync ───────────────────────────────────────────────────────────────────

QString Backend::syncFile() const {
    const QString folder = settings().value("syncFolder").toString();
    return folder.isEmpty() ? QString() : folder + "/tether-hosts.json";
}

void Backend::watchSyncFile() {
    const auto paths = m_syncWatcher.files() + m_syncWatcher.directories();
    if (!paths.isEmpty())
        m_syncWatcher.removePaths(paths);
    const QString file = syncFile();
    if (file.isEmpty()) {
        m_syncStatus.clear();
        emit syncStatusChanged();
        return;
    }
    // Sync tools replace the file, so watch its folder as well.
    m_syncWatcher.addPath(QFileInfo(file).absolutePath());
    if (QFileInfo::exists(file))
        m_syncWatcher.addPath(file);
}

QStringList Backend::riskyOptions(const QVariantMap &profile) {
    QStringList found;
    for (const QString &line : optionLines(profile.value("options"))) {
        const QString name = optionPattern.match(line).captured(1);
        if (riskyOptionNames.contains(name.toLower()) && !found.contains(name, Qt::CaseInsensitive))
            found << name;
    }
    return found;
}

// Keeps a synced copy aside instead of applying it. The same copy held
// before, waiting or turned down, isn't asked about again.
void Backend::hold(const QString &type, const QVariantMap &item, const QVariantMap &before) {
    const QString id = item.value("id").toString();
    const QVariantMap entry{{"type", type}, {"id", id}, {"item", item}, {"before", before}, {"state", "pending"}};
    for (QVariant &value : m_held) {
        const QVariantMap held = value.toMap();
        if (held.value("type") != type || held.value("id") != id)
            continue;
        if (withoutStamp(held.value("item").toMap()) != withoutStamp(item)) {
            value = entry;
            m_heldChanged = true;
        }
        return;
    }
    m_held << entry;
    m_heldChanged = true;
}

// Drops what waits for an item, since something newer settled it. A copy that
// was turned down stays remembered unless the item itself is gone: the same
// copy coming back, with whatever time is written on it, isn't a new question.
bool Backend::release(const QString &type, const QString &id, bool forget) {
    return m_held.removeIf([&](const QVariant &value) {
        const QVariantMap held = value.toMap();
        return held.value("type") == type && held.value("id") == id && (forget || held.value("state") == "pending");
    }) > 0;
}

// One list of synced items against this computer's. The newer copy of an
// item wins (a copy without a time, from an older Tether, counts as newer),
// but a copy that would change something guarded is held for the user.
bool Backend::mergeSynced(const QString &type, QVariantList &mine, const QVariantList &theirs) {
    bool changed = false;
    for (const QVariant &value : theirs) {
        const QVariantMap item = value.toMap();
        const QString id = item.value("id").toString();
        const int at = indexOfId(mine, id);
        if (at >= 0) {
            const QVariantMap local = mine[at].toMap();
            if (withoutStamp(local) == withoutStamp(item)) {
                m_heldChanged |= release(type, id);
                continue;
            }
            if (modifiedOf(item) != 0 && modifiedOf(item) < modifiedOf(local)) {
                // Ours was saved later: it stays, and goes back to the file.
                m_heldChanged |= release(type, id);
                m_syncStale = true;
                continue;
            }
            if (guardedDiffers(type, local, item)) {
                hold(type, item, {});
                continue;
            }
            mine[at] = item;
            m_heldChanged |= release(type, id);
            changed = true;
            continue;
        }
        // New here. An id a sync removed earlier is compared with what it
        // was then, so "removed, then back with another address" is a change.
        const QVariantMap tombstone = m_tombstones.value(id).toMap();
        QVariantMap was = tombstone.value("type") == type ? tombstone.value("item").toMap() : QVariantMap();
        if (type == "host" && !was.isEmpty())
            was = clean(was); // numbers as clean() writes them, to compare
        if ((!was.isEmpty() && guardedDiffers(type, was, item)) || (type == "host" && !riskyOptions(item).isEmpty())) {
            hold(type, item, was);
            continue;
        }
        mine << item;
        m_tombstones.remove(id);
        m_heldChanged |= release(type, id);
        changed = true;
    }
    return changed;
}

// Takes in what other computers wrote: the union of both sides, the newer
// copy where an item is on both, and nothing risky without being asked.
void Backend::readSyncFile() {
    const QString path = syncFile();
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_syncStatus = tr("Waiting for the first sync to %1").arg(path);
        emit syncStatusChanged();
        return;
    }
    const QByteArray data = file.readAll();
    if (data == m_lastSync)
        return;
    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (!document.isObject()) {
        m_syncStatus = tr("%1 couldn't be read; it may be mid-sync.").arg(path);
        emit syncStatusChanged();
        return;
    }
    const QJsonObject root = document.object();
    // Everything from the file goes through the same checks as the dialogs.
    QVariantList profiles, snippets, identities;
    for (const QVariant &value : root.value("profiles").toArray().toVariantList()) {
        const QVariantMap item = clean(value.toMap());
        if (!item.value("id").toString().isEmpty() && validate(item).isEmpty())
            profiles << item;
    }
    for (const QVariant &value : root.value("snippets").toArray().toVariantList()) {
        const QVariantMap item = cleanSnippet(value.toMap());
        if (!item.value("id").toString().isEmpty() && !item.value("name").toString().isEmpty()
            && !item.value("text").toString().isEmpty())
            snippets << item;
    }
    for (const QVariant &value : root.value("identities").toArray().toVariantList()) {
        QVariantMap item = cleanLogin(value.toMap());
        if (item.value("id").toString().isEmpty() || !loginProblem(item).isEmpty())
            continue;
        // Whether a password is in the keyring is a fact about this
        // computer; another computer's word for it isn't taken.
        item["hasPassword"] = identity(item.value("id").toString()).value("hasPassword").toBool();
        identities << item;
    }
    m_heldChanged = false;
    m_syncStale = false;
    const bool p = mergeSynced("host", m_profiles, profiles);
    const bool s = mergeSynced("snippet", m_snippets, snippets);
    const bool i = mergeSynced("login", m_identities, identities);
    // Removals: what another computer deleted since our last write. The last
    // copy is remembered here (not in the shared file), see mergeSynced.
    const QStringList removed = root.value("removed").toVariant().toStringList();
    auto drop = [&](const QString &type, QVariantList &list) {
        bool changed = false;
        for (int n = 0; n < list.size(); ++n) {
            const QString id = list[n].toMap().value("id").toString();
            if (!removed.contains(id))
                continue;
            m_tombstones.insert(id, QVariantMap{{"type", type}, {"item", list[n]},
                                                {"at", QDateTime::currentMSecsSinceEpoch()}});
            list.removeAt(n--);
            changed = true;
        }
        return changed;
    };
    const bool d = drop("host", m_profiles) | drop("snippet", m_snippets) | drop("login", m_identities);
    for (const QString &id : removed)
        m_heldChanged |= m_held.removeIf([&](const QVariant &value) { return value.toMap().value("id") == id; }) > 0;
    // The oldest go once there are many.
    while (m_tombstones.size() > 200) {
        QString oldest;
        for (auto it = m_tombstones.cbegin(); it != m_tombstones.cend(); ++it)
            if (oldest.isEmpty() || it.value().toMap().value("at").toLongLong()
                                        < m_tombstones.value(oldest).toMap().value("at").toLongLong())
                oldest = it.key();
        m_tombstones.remove(oldest);
    }
    m_removed = removed;
    m_lastSync = data;
    if (p || d)
        emit profilesChanged();
    if (s || d)
        emit snippetsChanged();
    if (i || d)
        emit identitiesChanged();
    if (m_heldChanged)
        emit syncReviewChanged();
    m_syncStatus = tr("Synced with %1 at %2").arg(path, QDateTime::currentDateTime().toString("HH:mm"));
    emit syncStatusChanged();
    if (p || s || i || d || m_heldChanged)
        save();
    else if (m_syncStale)
        writeSyncFile();
}

void Backend::writeSyncFile() {
    const QString path = syncFile();
    if (path.isEmpty())
        return;
    // What is held from sync goes back out as it came. It isn't this
    // computer's to drop: writing our older copy over it would undo the other
    // computer's edit there, and the two would overwrite each other in turn.
    // A copy that was turned down is different: this computer's own copy is
    // what it says, and the other computers are asked about that in turn.
    auto shared = [this](const QString &type, QVariantList list) {
        for (const QVariant &value : m_held) {
            const QVariantMap held = value.toMap();
            if (held.value("type") != type)
                continue;
            const int at = indexOfId(list, held.value("id").toString());
            if (at < 0)
                list << held.value("item");
            else if (held.value("state") == "pending")
                list[at] = held.value("item");
        }
        return list;
    };
    const QJsonObject root{{"version", 1}, {"app", "tether"},
                           {"profiles", QJsonArray::fromVariantList(shared("host", m_profiles))},
                           {"snippets", QJsonArray::fromVariantList(shared("snippet", m_snippets))},
                           {"identities", QJsonArray::fromVariantList(shared("login", m_identities))},
                           {"removed", QJsonArray::fromStringList(m_removed)}};
    const QByteArray data = QJsonDocument(root).toJson();
    if (data == m_lastSync)
        return;
    QSaveFile file(path);
    // Host names, user names, and key paths: nobody else's to read.
    if (file.open(QIODevice::WriteOnly) && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        && file.write(data) >= 0 && file.commit()) {
        m_lastSync = data;
        m_syncStatus = tr("Synced with %1 at %2").arg(path, QDateTime::currentDateTime().toString("HH:mm"));
    } else {
        m_syncStatus = tr("Couldn't write %1: %2").arg(path, file.errorString());
    }
    emit syncStatusChanged();
    watchSyncFile();
}

// What waits for the user, in words: which fields a synced copy would change.
QVariantList Backend::syncReview() const {
    // Spelled out one by one, for bin/i18n-check to find them.
    auto label = [](const QString &field) {
        return field == "kind" ? tr("Type") : field == "host" ? tr("Address") : field == "port" ? tr("Port")
             : field == "user" ? tr("User") : field == "options" ? tr("ssh options")
             : field == "jumpHost" ? tr("Jump host") : field == "identityFile" ? tr("Key file")
             : field == "identityId" ? tr("Login") : field == "forwards" ? tr("Port forwards")
             : field == "startupSnippets" ? tr("Startup snippets") : field == "device" ? tr("Device")
             : field == "text" ? tr("Text") : field == "run" ? tr("Presses Enter") : field;
    };
    auto nameOf = [this](const QVariantList &list, const QString &type, const QString &id) {
        const int at = indexOfId(list, id);
        if (at >= 0)
            return list[at].toMap().value("name").toString();
        for (const QVariant &value : m_held)
            if (value.toMap().value("type") == type && value.toMap().value("id") == id)
                return value.toMap().value("item").toMap().value("name").toString();
        return id;
    };
    auto shown = [&](const QString &field, const QVariant &value) -> QString {
        if (field == "forwards") {
            QStringList parts;
            for (const QVariant &f : value.toList())
                parts << f.toMap().value("type").toString() + " " + f.toMap().value("spec").toString();
            return parts.join(", ");
        }
        if (field == "startupSnippets") {
            QStringList names;
            for (const QVariant &id : value.toList())
                names << nameOf(m_snippets, "snippet", id.toString());
            return names.join(", ");
        }
        if (field == "identityId")
            return value.toString().isEmpty() ? QString() : nameOf(m_identities, "login", value.toString());
        if (field == "run")
            return value.toBool() ? tr("Yes") : tr("No");
        return value.toString();
    };
    QVariantList review;
    for (const QVariant &value : m_held) {
        const QVariantMap held = value.toMap();
        if (held.value("state") != "pending")
            continue;
        const QString type = held.value("type").toString(), id = held.value("id").toString();
        const QVariantMap item = held.value("item").toMap();
        const QVariantList &mine = type == "host" ? m_profiles : type == "snippet" ? m_snippets : m_identities;
        const int at = indexOfId(mine, id);
        const QVariantMap before = at >= 0 ? mine[at].toMap() : held.value("before").toMap();
        QVariantList changes;
        for (const QString &field : guardedFields.value(type)) {
            const QString was = before.isEmpty() ? QString() : shown(field, before.value(field));
            const QString now = shown(field, item.value(field));
            // A new item lists what it holds; a changed one, what differs.
            if (before.isEmpty() ? (!now.isEmpty() && field != "kind" && field != "run") : before.value(field) != item.value(field))
                changes << QVariantMap{{"label", label(field)}, {"before", was}, {"after", now}};
        }
        review << QVariantMap{{"id", id}, {"type", type}, {"name", item.value("name")}, {"isNew", at < 0},
                              {"risky", type == "host" ? riskyOptions(item) : QStringList()}, {"changes", changes}};
    }
    return review;
}

QString Backend::approveSync(const QString &id) {
    bool hosts = false, snippets = false, logins = false;
    for (int n = 0; n < m_held.size(); ++n) {
        const QVariantMap held = m_held[n].toMap();
        if (held.value("state") != "pending" || (!id.isEmpty() && held.value("id") != id))
            continue;
        const QString type = held.value("type").toString();
        const QVariantMap item = held.value("item").toMap();
        QVariantList &mine = type == "host" ? m_profiles : type == "snippet" ? m_snippets : m_identities;
        const int at = indexOfId(mine, held.value("id").toString());
        if (at >= 0)
            mine[at] = item;
        else
            mine << item;
        m_tombstones.remove(held.value("id").toString());
        (type == "host" ? hosts : type == "snippet" ? snippets : logins) = true;
        m_held.removeAt(n--);
    }
    if (hosts)
        emit profilesChanged();
    if (snippets)
        emit snippetsChanged();
    if (logins)
        emit identitiesChanged();
    emit syncReviewChanged();
    return save();
}

QString Backend::rejectSync(const QString &id) {
    bool hosts = false, snippets = false, logins = false;
    for (int n = 0; n < m_held.size(); ++n) {
        QVariantMap held = m_held[n].toMap();
        if (held.value("state") != "pending" || (!id.isEmpty() && held.value("id") != id))
            continue;
        const QString type = held.value("type").toString();
        QVariantList &mine = type == "host" ? m_profiles : type == "snippet" ? m_snippets : m_identities;
        const int at = indexOfId(mine, held.value("id").toString());
        if (at >= 0) {
            // Keeping this computer's copy is a decision made now, so it is
            // the newer one and the other computers are offered it.
            mine[at] = withStamp(mine[at].toMap());
            (type == "host" ? hosts : type == "snippet" ? snippets : logins) = true;
        }
        // Remembered as turned down, so the same copy isn't offered again,
        // whatever time it comes back with. Where there is nothing here to
        // keep, it stays in the shared file for the computers that want it.
        held["state"] = "rejected";
        m_held[n] = held;
    }
    if (hosts)
        emit profilesChanged();
    if (snippets)
        emit snippetsChanged();
    if (logins)
        emit identitiesChanged();
    emit syncReviewChanged();
    return save();
}

// ── Snippets ───────────────────────────────────────────────────────────────

QStringList Backend::snippetVariables(const QString &text) {
    static const QRegularExpression placeholder(R"(\{\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}\})");
    static const QStringList builtIn{"date", "time", "host", "user"};
    QStringList names;
    for (auto it = placeholder.globalMatch(text); it.hasNext();) {
        const QString name = it.next().captured(1);
        if (!names.contains(name) && !builtIn.contains(name))
            names << name;
    }
    return names;
}

QString Backend::snippetPayload(const QVariantMap &snippet, const QVariantMap &values) {
    static const QRegularExpression placeholder(R"(\{\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}\})");
    QVariantMap filled = values;
    filled.insert("date", QDate::currentDate().toString(Qt::ISODate));
    filled.insert("time", QTime::currentTime().toString("HH:mm:ss"));
    QString text;
    qsizetype last = 0;
    const QString source = snippet.value("text").toString();
    for (auto it = placeholder.globalMatch(source); it.hasNext();) {
        const auto match = it.next();
        text += source.mid(last, match.capturedStart() - last);
        const QString name = match.captured(1);
        text += filled.contains(name) ? filled.value(name).toString() : match.captured(0);
        last = match.capturedEnd();
    }
    text += source.mid(last);
    text.replace("\r\n", "\n");
    text.replace('\n', '\r');
    if (snippet.value("run").toBool() && !text.endsWith('\r'))
        text += '\r';
    return text;
}

QString Backend::saveProfile(const QVariantMap &profile) {
    const QString error = validate(profile);
    if (!error.isEmpty())
        return error;
    QVariantMap p = withStamp(clean(profile));
    if (p["id"].toString().isEmpty())
        p["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);

    const QVariantList before = m_profiles;
    // What is saved here now is newer than a synced copy still waiting.
    const QVariantList heldBefore = m_held;
    const bool released = release("host", p["id"].toString());
    bool replaced = false;
    for (QVariant &existing : m_profiles)
        if (existing.toMap().value("id") == p["id"]) {
            existing = p;
            replaced = true;
        }
    if (!replaced)
        m_profiles << p;

    if (const QString saveError = save(); !saveError.isEmpty()) {
        m_profiles = before;
        m_held = heldBefore;
        return saveError;
    }
    emit profilesChanged();
    if (released)
        emit syncReviewChanged();
    return {};
}

QString Backend::removeProfile(const QString &id) {
    const QVariantList before = m_profiles;
    m_profiles.removeIf([&](const QVariant &p) { return p.toMap().value("id").toString() == id; });
    if (!m_removed.contains(id))
        m_removed << id;
    const QVariantList heldBefore = m_held;
    const bool released = release("host", id, true);
    if (const QString error = save(); !error.isEmpty()) {
        m_profiles = before;
        m_held = heldBefore;
        return error;
    }
    emit profilesChanged();
    if (released)
        emit syncReviewChanged();
    return {};
}

QString Backend::saveSnippet(const QVariantMap &snippet) {
    QVariantMap s = withStamp(cleanSnippet(snippet));
    if (s["name"].toString().isEmpty())
        return tr("Give the snippet a name.");
    if (s["text"].toString().isEmpty())
        return tr("The snippet has no text to send.");
    if (s["id"].toString().isEmpty())
        s["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);

    const QVariantList before = m_snippets;
    bool replaced = false;
    for (QVariant &existing : m_snippets)
        if (existing.toMap().value("id") == s["id"]) {
            existing = s;
            replaced = true;
        }
    if (!replaced)
        m_snippets << s;
    const QVariantList heldBefore = m_held;
    const bool released = release("snippet", s["id"].toString());
    if (const QString error = save(); !error.isEmpty()) {
        m_snippets = before;
        m_held = heldBefore;
        return error;
    }
    emit snippetsChanged();
    if (released)
        emit syncReviewChanged();
    return {};
}

QString Backend::removeSnippet(const QString &id) {
    const QVariantList before = m_snippets;
    m_snippets.removeIf([&](const QVariant &s) { return s.toMap().value("id").toString() == id; });
    if (!m_removed.contains(id))
        m_removed << id;
    const QVariantList heldBefore = m_held;
    const bool released = release("snippet", id, true);
    if (const QString error = save(); !error.isEmpty()) {
        m_snippets = before;
        m_held = heldBefore;
        return error;
    }
    emit snippetsChanged();
    if (released)
        emit syncReviewChanged();
    return {};
}

void Backend::load() {
    QFile file(m_configFile);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        // Keep the unreadable file for the user rather than overwriting it.
        const QString aside = m_configFile + ".unreadable-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
        QFile::rename(m_configFile, aside);
        m_loadError = tr("Your saved hosts couldn't be read, so the file was moved to %1 and Tether started fresh.").arg(aside);
        return;
    }
    const QJsonObject root = document.object();
    for (const QJsonValue &value : root.value("profiles").toArray()) {
        const QVariantMap p = value.toObject().toVariantMap();
        if (validate(p).isEmpty() && !p.value("id").toString().isEmpty())
            m_profiles << clean(p);
    }
    for (const QVariant &value : root.value("snippets").toArray().toVariantList())
        m_snippets << cleanSnippet(value.toMap());
    m_fontSize = std::clamp(root.value("fontSize").toInt(13), 7, 40);
    for (const QVariant &value : root.value("identities").toArray().toVariantList())
        m_identities << cleanLogin(value.toMap());
    m_removed = root.value("removed").toVariant().toStringList();
    // What sync brought that waits for the user or was turned down, and the
    // last copies of what it removed: this computer's own notes, never synced.
    // Held items are checked again, since they came from outside.
    for (const QVariant &value : root.value("syncHeld").toArray().toVariantList()) {
        QVariantMap held = value.toMap();
        const QString type = held.value("type").toString();
        const QVariantMap raw = held.value("item").toMap();
        const QVariantMap item = type == "host" ? clean(raw) : type == "snippet" ? cleanSnippet(raw) : cleanLogin(raw);
        const bool valid = type == "host" ? validate(item).isEmpty()
                         : type == "snippet" ? !item.value("name").toString().isEmpty() && !item.value("text").toString().isEmpty()
                         : type == "login" && loginProblem(item).isEmpty();
        if (!valid || item.value("id").toString().isEmpty())
            continue;
        held["item"] = item;
        held["id"] = item.value("id");
        m_held << held;
    }
    m_tombstones = root.value("syncRemoved").toObject().toVariantMap();
    const QVariantMap stored = root.value("settings").toObject().toVariantMap();
    for (auto it = stored.cbegin(); it != stored.cend(); ++it)
        if (defaultSettings().contains(it.key()))
            m_settings.insert(it.key(), it.value());
}

QString Backend::save() {
    QDir().mkpath(QFileInfo(m_configFile).absolutePath());
    QSaveFile file(m_configFile);
    const QJsonObject root{{"version", 1},
                           {"profiles", QJsonArray::fromVariantList(m_profiles)},
                           {"snippets", QJsonArray::fromVariantList(m_snippets)},
                           {"fontSize", m_fontSize},
                           {"settings", QJsonObject::fromVariantMap(m_settings)},
                           {"identities", QJsonArray::fromVariantList(m_identities)},
                           {"removed", QJsonArray::fromStringList(m_removed.mid(qMax(0, int(m_removed.size()) - 500)))},
                           {"syncHeld", QJsonArray::fromVariantList(m_held)},
                           {"syncRemoved", QJsonObject::fromVariantMap(m_tombstones)}};
    if (!file.open(QIODevice::WriteOnly))
        return tr("Couldn't save to %1: %2").arg(m_configFile, file.errorString());
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    file.write(QJsonDocument(root).toJson());
    if (!file.commit())
        return tr("Couldn't save to %1: %2").arg(m_configFile, file.errorString());
    writeSyncFile();
    return {};
}

// Host aliases from ~/.ssh/config and the files it includes; wildcard
// patterns are left out because they aren't something to connect to.
QVariantList Backend::parseSshConfig(const QString &path) {
    QVariantList hosts;
    QStringList seen;
    std::function<void(const QString &, int)> read = [&](const QString &file, int depth) {
        QFile input(file);
        if (depth > 4 || !input.open(QIODevice::ReadOnly))
            return;
        QStringList current;
        QVariantMap settings;
        auto flush = [&] {
            for (const QString &alias : current) {
                if (seen.contains(alias))
                    continue;
                seen << alias;
                const QString host = settings.value("hostname", alias).toString();
                const QString user = settings.value("user").toString();
                const QString port = settings.value("port").toString();
                QString detail = (user.isEmpty() ? "" : user + "@") + host + (port.isEmpty() ? "" : ":" + port);
                hosts << QVariantMap{{"kind", "ssh"}, {"source", "config"}, {"name", alias},
                                     {"host", alias}, {"detail", detail}};
            }
            current.clear();
            settings.clear();
        };
        for (const QString &raw : QString::fromUtf8(input.readAll()).split('\n')) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith('#'))
                continue;
            const QStringList words = line.split(QRegularExpression("[\\s=]+"), Qt::SkipEmptyParts);
            const QString key = words.value(0).toLower();
            if (key == "host" || key == "match") {
                flush();
                if (key == "host")
                    for (const QString &alias : words.mid(1))
                        if (!alias.contains(QRegularExpression("[*?!]")) && hostPattern.match(alias).hasMatch())
                            current << alias;
            } else if (key == "include") {
                flush();
                for (const QString &pattern : words.mid(1)) {
                    const QString full = expandHome(pattern).startsWith('/')
                        ? expandHome(pattern) : QFileInfo(path).absolutePath() + "/" + pattern;
                    const QFileInfo info(full);
                    const QStringList matches = QDir(info.absolutePath())
                        .entryList({info.fileName()}, QDir::Files, QDir::Name);
                    for (const QString &match : matches)
                        read(info.absolutePath() + "/" + match, depth + 1);
                }
            } else if (!current.isEmpty() && !settings.contains(key)) {
                settings.insert(key, words.value(1));
            }
        }
        flush();
    };
    read(path, 0);
    return hosts;
}

void Backend::reloadSshConfig() {
    // Editors replace the file, which drops it from the watch, so re-arm.
    const auto watched = m_sshWatcher.files() + m_sshWatcher.directories();
    if (!watched.isEmpty())
        m_sshWatcher.removePaths(watched);
    for (const QString &path : {QFileInfo(m_sshConfig).absolutePath(), m_sshConfig})
        if (QFileInfo::exists(path))
            m_sshWatcher.addPath(path);

    const QVariantList hosts = parseSshConfig(m_sshConfig);
    if (hosts != m_configHosts) {
        m_configHosts = hosts;
        emit configHostsChanged();
    }
}
