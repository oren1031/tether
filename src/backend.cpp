#include "backend.h"

#include "serialports.h"
#include "credentials.h"

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
            {"terminalScheme", "omarchy"}, // "omarchy" or a scheme the terminal ships
            {"terminalFont", ""},         // empty is the system monospace font
            {"lineSpacing", 0},
            {"blinkingCursor", false},
            {"boldText", true},
            {"paneDimming", true},
            {"shell", ""},                // empty uses $SHELL
            {"confirmQuit", true},
            {"localTmux", false},         // local shells run inside tmux, so they outlive Tether
            {"shareConnections", true},   // ssh ControlMaster: one login per host for panes and files
            {"syncFolder", ""},
            {"checkUpdates", true},
            {"aiProvider", "auto"}};      // auto, api, or claude-code
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
#ifdef Q_OS_WIN
            // tether.exe can answer password prompts but not "trust this new
            // host?", so a first-seen host key is accepted; a changed one is
            // still refused.
            if (!environment.isEmpty())
                args << "-o" << "StrictHostKeyChecking=accept-new";
#endif
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

bool Backend::connectionReady(const QVariantMap &profile) const {
    const QVariantMap p = resolved(clean(profile));
    if (p["kind"] != "ssh" || p["transport"] == "mosh" || !sharesConnections())
        return true;
    QStringList args{"-O", "check", "-o", "ControlPath=" + controlPath(), "-p", QString::number(p["port"].toInt())};
    if (!p["user"].toString().isEmpty())
        args << "-l" << p["user"].toString();
    args << "--" << p["host"].toString();
    QProcess check;
    check.start(tool("ssh"), args);
    return check.waitForFinished(1500) && check.exitStatus() == QProcess::NormalExit && check.exitCode() == 0;
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

// Runs with stdin closed and a time limit, so a locked keyring's prompt can't
// hang the app.
static void clearSecret(const QString &secretTool, const QString &id) {
    QProcess clear;
    clear.start(secretTool, {"clear", "service", "tether", "identity", id});
    clear.closeWriteChannel();
    if (!clear.waitForFinished(15000))
        clear.kill();
}

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
dir="${XDG_RUNTIME_DIR:-/tmp}/tether"
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
    const QString path = QFileInfo(controlPath()).absolutePath() + "/askpass";
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == QByteArray(askpassScript))
        return path;
    existing.close();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(askpassScript) < 0 || !file.commit())
        return {};
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

QString Backend::saveIdentity(const QVariantMap &input, const QString &password, bool clearPassword) {
    QVariantMap login{{"id", input.value("id").toString()},
                      {"name", input.value("name").toString().trimmed()},
                      {"user", input.value("user").toString().trimmed()},
                      {"identityFile", input.value("identityFile").toString().trimmed()},
                      {"hasPassword", false}};
    if (login["name"].toString().isEmpty())
        return tr("Give the login a name.");
    if (!login["user"].toString().isEmpty() && !userPattern.match(login["user"].toString()).hasMatch())
        return tr("“%1” isn't a valid user name.").arg(login["user"].toString());
    if (login["identityFile"].toString().contains('\n'))
        return tr("The key file path can't span lines.");
    if (login["id"].toString().isEmpty())
        login["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString id = login["id"].toString();
    const QVariantMap before = identity(id);
    login["hasPassword"] = before.value("hasPassword").toBool();

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
        QProcess store;
        store.start(secretTool, {"store", "--label=Tether: " + login["name"].toString(), "service", "tether", "identity", id});
        store.write(password.toUtf8());
        store.closeWriteChannel();
        if (!store.waitForFinished(30000) || store.exitCode() != 0)
            return tr("The keyring didn't take the password: %1").arg(QString::fromUtf8(store.readAllStandardError()).trimmed());
        login["hasPassword"] = true;
    } else if (clearPassword && login["hasPassword"].toBool()) {
        if (!secretTool.isEmpty())
            clearSecret(secretTool, id);
        login["hasPassword"] = false;
    }
#endif

    bool replaced = false;
    for (QVariant &existing : m_identities)
        if (existing.toMap().value("id") == id) {
            existing = login;
            replaced = true;
        }
    if (!replaced)
        m_identities << login;
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
        clearSecret(tool("secret-tool"), id);
#endif
    for (int i = 0; i < m_identities.size(); ++i)
        if (m_identities[i].toMap().value("id") == id)
            m_identities.removeAt(i--);
    if (!m_removed.contains(id))
        m_removed << id;
    // Hosts that used it go back to their own fields.
    bool profilesTouched = false;
    for (QVariant &value : m_profiles) {
        QVariantMap p = value.toMap();
        if (p.value("identityId") == id) {
            p["identityId"] = QString();
            value = p;
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

// Takes in what other computers wrote: the union of both sides, with the
// shared file's copy winning where an item is on both.
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
    auto merge = [](QVariantList &mine, const QVariantList &theirs, const std::function<bool(const QVariantMap &)> &ok) {
        bool changed = false;
        for (const QVariant &value : theirs) {
            const QVariantMap item = value.toMap();
            if (item.value("id").toString().isEmpty() || !ok(item))
                continue;
            bool found = false;
            for (QVariant &existing : mine)
                if (existing.toMap().value("id") == item.value("id")) {
                    found = true;
                    if (existing.toMap() != item) {
                        existing = item;
                        changed = true;
                    }
                }
            if (!found) {
                mine << item;
                changed = true;
            }
        }
        return changed;
    };
    QVariantList profiles;
    for (const QVariant &value : root.value("profiles").toArray().toVariantList())
        profiles << clean(value.toMap());
    const bool p = merge(m_profiles, profiles, [](const QVariantMap &item) { return validate(item).isEmpty(); });
    const bool s = merge(m_snippets, root.value("snippets").toArray().toVariantList(),
                         [](const QVariantMap &item) { return !item.value("name").toString().isEmpty(); });
    const bool i = merge(m_identities, root.value("identities").toArray().toVariantList(),
                         [](const QVariantMap &item) { return !item.value("name").toString().isEmpty(); });
    // Removals: what another computer deleted since our last write.
    const QStringList removed = root.value("removed").toVariant().toStringList();
    auto drop = [&](QVariantList &list) {
        bool changed = false;
        for (int n = 0; n < list.size(); ++n)
            if (removed.contains(list[n].toMap().value("id").toString())) {
                list.removeAt(n--);
                changed = true;
            }
        return changed;
    };
    const bool d = drop(m_profiles) | drop(m_snippets) | drop(m_identities);
    m_removed = removed;
    m_lastSync = data;
    if (p || d)
        emit profilesChanged();
    if (s || d)
        emit snippetsChanged();
    if (i || d)
        emit identitiesChanged();
    m_syncStatus = tr("Synced with %1 at %2").arg(path, QDateTime::currentDateTime().toString("HH:mm"));
    emit syncStatusChanged();
    if (p || s || i || d)
        save();
}

void Backend::writeSyncFile() {
    const QString path = syncFile();
    if (path.isEmpty())
        return;
    const QJsonObject root{{"version", 1}, {"app", "tether"},
                           {"profiles", QJsonArray::fromVariantList(m_profiles)},
                           {"snippets", QJsonArray::fromVariantList(m_snippets)},
                           {"identities", QJsonArray::fromVariantList(m_identities)},
                           {"removed", QJsonArray::fromStringList(m_removed)}};
    const QByteArray data = QJsonDocument(root).toJson();
    if (data == m_lastSync)
        return;
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(data) >= 0 && file.commit()) {
        m_lastSync = data;
        m_syncStatus = tr("Synced with %1 at %2").arg(path, QDateTime::currentDateTime().toString("HH:mm"));
    } else {
        m_syncStatus = tr("Couldn't write %1: %2").arg(path, file.errorString());
    }
    emit syncStatusChanged();
    watchSyncFile();
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
    QVariantMap p = clean(profile);
    if (p["id"].toString().isEmpty())
        p["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);

    const QVariantList before = m_profiles;
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
        return saveError;
    }
    emit profilesChanged();
    return {};
}

QString Backend::removeProfile(const QString &id) {
    const QVariantList before = m_profiles;
    m_profiles.removeIf([&](const QVariant &p) { return p.toMap().value("id").toString() == id; });
    if (!m_removed.contains(id))
        m_removed << id;
    if (const QString error = save(); !error.isEmpty()) {
        m_profiles = before;
        return error;
    }
    emit profilesChanged();
    return {};
}

QString Backend::saveSnippet(const QVariantMap &snippet) {
    QVariantMap s{{"id", snippet.value("id").toString()},
                  {"name", snippet.value("name").toString().trimmed()},
                  {"text", snippet.value("text").toString()},
                  {"run", snippet.value("run").toBool()}};
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
    if (const QString error = save(); !error.isEmpty()) {
        m_snippets = before;
        return error;
    }
    emit snippetsChanged();
    return {};
}

QString Backend::removeSnippet(const QString &id) {
    const QVariantList before = m_snippets;
    m_snippets.removeIf([&](const QVariant &s) { return s.toMap().value("id").toString() == id; });
    if (!m_removed.contains(id))
        m_removed << id;
    if (const QString error = save(); !error.isEmpty()) {
        m_snippets = before;
        return error;
    }
    emit snippetsChanged();
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
    m_snippets = root.value("snippets").toArray().toVariantList();
    m_fontSize = std::clamp(root.value("fontSize").toInt(13), 7, 40);
    m_identities = root.value("identities").toArray().toVariantList();
    m_removed = root.value("removed").toVariant().toStringList();
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
                           {"removed", QJsonArray::fromStringList(m_removed.mid(qMax(0, int(m_removed.size()) - 500)))}};
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
