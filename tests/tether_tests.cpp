#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QProcess>
#include <QElapsedTimer>
#include <QInputMethodEvent>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include "assistant.h"
#include "backend.h"
#include "sftp.h"
#include "terminalview.h"
#ifdef TETHER_LIBSSH
#include "sshsession.h"
#endif
#include "updater.h"
#include "serialports.h"
#include "theme.h"
#include "translator.h"

// A web server on this computer for the update tests: each path answers
// with the bytes it was given, anything else with 404.
struct WebServer {
    QTcpServer server;
    QHash<QString, QByteArray> files;
    QStringList asked;

    WebServer()
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    if (!socket->peek(64 * 1024).contains("\r\n\r\n"))
                        return;
                    const QString path = QString::fromLatin1(socket->readAll()).section(' ', 1, 1);
                    asked << path;
                    const bool found = files.contains(path);
                    const QByteArray body = files.value(path);
                    socket->write((found ? "HTTP/1.1 200 OK" : "HTTP/1.1 404 Not Found")
                                  + QByteArray("\r\nConnection: close\r\nContent-Length: ")
                                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return QString("http://127.0.0.1:%1").arg(server.serverPort()); }
};

// A release as the publishing workflow makes it, signed with a key made for
// the test: latest.json, its signature, and the package it lists.
struct SignedRelease {
    QTemporaryDir dir;
    QString publicKey;       // the base64 part, as built into Tether
    QByteArray package = QByteArray("the package").repeated(5000);
    QString name;

    bool makeKey()
    {
        const QString keygen = QStandardPaths::findExecutable("ssh-keygen");
        if (keygen.isEmpty() || QProcess::execute(keygen, {"-q", "-t", "ed25519", "-N", "", "-f", dir.filePath("key")}) != 0)
            return false;
        QFile pub(dir.filePath("key.pub"));
        if (!pub.open(QIODevice::ReadOnly))
            return false;
        publicKey = QString::fromLatin1(pub.readAll()).section(' ', 1, 1);
        return !publicKey.isEmpty();
    }
    QByteArray manifest(const QString &version, const QJsonObject &changes = {}) const
    {
        QJsonObject asset{{"name", name.isEmpty() ? QString("tether-%1-1-x86_64.pkg.tar.zst").arg(version) : name},
                          {"size", package.size()},
                          {"sha256", QString::fromLatin1(QCryptographicHash::hash(package, QCryptographicHash::Sha256).toHex())}};
        for (auto it = changes.begin(); it != changes.end(); ++it) {
            if (it.value().isNull())
                asset.remove(it.key());
            else
                asset[it.key()] = it.value();
        }
        return QJsonDocument(QJsonObject{{"version", version}, {"assets", QJsonArray{asset}}}).toJson();
    }
    QByteArray sign(const QByteArray &bytes, const QString &key = {}) const
    {
        const QString path = dir.filePath("signed");
        QFile::remove(path + ".sig");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return {};
        file.write(bytes);
        file.close();
        QProcess keygen;
        keygen.start("ssh-keygen", {"-Y", "sign", "-f", key.isEmpty() ? dir.filePath("key") : key, "-n", "tether-update", path});
        keygen.waitForFinished(20000);
        QFile signature(path + ".sig");
        return signature.open(QIODevice::ReadOnly) ? signature.readAll() : QByteArray();
    }
    // What GitHub's API answers for the newest release, its files on web.
    void publish(WebServer &web, const QString &version, const QByteArray &list, const QByteArray &signature,
                 const QByteArray &file) const
    {
        const QString pkg = name.isEmpty() ? QString("tether-%1-1-x86_64.pkg.tar.zst").arg(version) : name;
        QJsonArray assets;
        auto add = [&](const QString &asset, const QByteArray &bytes) {
            if (bytes.isNull())
                return;
            const QString path = "/assets/" + asset;
            web.files[path] = bytes;
            assets << QJsonObject{{"name", asset}, {"size", bytes.size()}, {"url", web.url() + path},
                                  {"browser_download_url", web.url() + path}};
        };
        add("SHA256SUMS", "unused here");
        add("latest.json", list);
        add("latest.json.sig", signature);
        add(pkg, file);
        web.files["/repos/oren1031/tether/releases/latest"] = QJsonDocument(QJsonObject{
            {"tag_name", "v" + version}, {"body", "Notes"}, {"html_url", "https://example.com/release"},
            {"assets", assets}}).toJson();
    }
    // A Tether older than the release, asking this web server as if it were GitHub.
    void pointAt(Updater &updater, const WebServer &web) const
    {
        updater.setPublicKey(publicKey);
        updater.setApiBase(web.url() + "/");
        updater.setInstallKind("pacman");
        updater.setDownloadDirectory(dir.filePath("updates"));
    }
};

class TetherTests : public QObject {
    Q_OBJECT

    // Runs a check for updates to its end and gives what Tether would say.
    static QString checkedMessage(Updater &updater)
    {
        updater.check(false);
        if (!QTest::qWaitFor([&] { return updater.state() != "checking"; }, 30000))
            return "the check never finished";
        return updater.message();
    }

private slots:
    void sshCommandUsesEveryField()
    {
        QTemporaryDir dir;
        Backend backend(dir.filePath("tether.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        backend.setTool("ssh", "/usr/bin/ssh");
        backend.setSetting("shareConnections", false);
        const QVariantMap launch = backend.launch({
            {"kind", "ssh"}, {"name", "Pi"}, {"host", "example.com"}, {"user", "pi"}, {"port", 2222}, {"reconnect", false},
            {"identityFile", "~/.ssh/id_ed25519"}, {"jumpHost", "me@bastion"},
            {"options", "ServerAliveInterval 30\n\nCompression=yes"},
            {"forwards", QVariantList{QVariantMap{{"type", "L"}, {"spec", "8080:localhost:80"}},
                                      QVariantMap{{"type", "D"}, {"spec", "1080"}}}}});
        QVERIFY2(!launch.contains("error"), qPrintable(launch.value("error").toString()));
        QCOMPARE(launch["program"].toString(), QString("/usr/bin/ssh"));
        QCOMPARE(launch["args"].toStringList(),
                 QStringList({"-p", "2222", "-l", "pi", "-i", QDir::homePath() + "/.ssh/id_ed25519",
                              "-J", "me@bastion", "-L", "8080:localhost:80", "-D", "1080",
                              "-o", "ServerAliveInterval=30", "-o", "Compression=yes", "--", "example.com"}));
        QCOMPARE(launch["title"].toString(), QString("Pi"));
        QVERIFY(launch["splittable"].toBool());
    }

    void fieldsCantBecomeSshOptions()
    {
        auto error = [](const QVariantMap &extra) {
            QVariantMap p{{"kind", "ssh"}, {"name", "x"}, {"host", "example.com"}};
            p.insert(extra);
            return Backend::validate(p);
        };
        QVERIFY(error({}).isEmpty());
        QVERIFY(!error({{"host", "-oProxyCommand=touch /tmp/x"}}).isEmpty());
        QVERIFY(!error({{"host", "a b"}}).isEmpty());
        QVERIFY(!error({{"user", "-F/etc/passwd"}}).isEmpty());
        QVERIFY(!error({{"jumpHost", "-oProxyCommand=x"}}).isEmpty());
        QVERIFY(!error({{"port", 70000}}).isEmpty());
        QVERIFY(!error({{"options", "-oProxyCommand=x"}}).isEmpty());
        QVERIFY(!error({{"options", "=nokey"}}).isEmpty());
        QVERIFY(!error({{"forwards", QVariantList{QVariantMap{{"type", "L"}, {"spec", "-oX"}}}}}).isEmpty());
        QVERIFY(!error({{"forwards", QVariantList{QVariantMap{{"type", "X"}, {"spec", "80"}}}}}).isEmpty());
        QVERIFY(error({{"forwards", QVariantList{QVariantMap{{"type", "R"}, {"spec", "*:9000:[::1]:22"}}}}}).isEmpty());
        QVERIFY(error({{"host", "fe80::1%eth0"}}).isEmpty());
    }

    void serialCommandAndChecks()
    {
        QTemporaryDir dir;
        Backend backend(dir.filePath("tether.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        backend.setTool("picocom", "/usr/bin/picocom");

        // A file the test can open stands in for the device; validation only
        // takes /dev paths, so check the command through quickConnect-free launch.
        QVariantMap p{{"kind", "serial"}, {"name", "Board"}, {"device", "/dev/null"}, {"baud", 9600},
                      {"dataBits", 7}, {"parity", "even"}, {"stopBits", 2}, {"flow", "hardware"},
                      {"enter", "crlf"}, {"echo", true}};
        const QVariantMap launch = backend.launch(p);
        QVERIFY2(!launch.contains("error"), qPrintable(launch.value("error").toString()));
        QCOMPARE(launch["args"].toStringList(),
                 QStringList({"--baud", "9600", "--databits", "7", "--parity", "e", "--stopbits", "2",
                              "--flow", "h", "--omap", "crcrlf", "--echo", "/dev/null"}));
        QVERIFY(!launch["splittable"].toBool());

        p["device"] = "/dev/ttyDoesNotExist9";
        QVERIFY(backend.launch(p).value("error").toString().contains("not connected"));
        p["device"] = "/dev/../etc/passwd";
        QVERIFY(!Backend::validate(p).isEmpty());
        p["device"] = "/dev/null";
        p["baud"] = 0;
        QVERIFY(!Backend::validate(p).isEmpty());

        backend.setTool("picocom", "");
        p["baud"] = 9600;
        QVERIFY(backend.launch(p).value("error").toString().contains("picocom"));
    }

    void terminalIsATextFieldToTouchKeyboards()
    {
        TerminalView view;
        view.setSize(QSizeF(800, 400));
        // Qt for Android drops everything an on-screen keyboard sends as text
        // (every letter) unless the focused item has a property with this
        // name, as Qt's own text fields do.
        QVERIFY(view.property("inputMethodHints").isValid());
        QInputMethodQueryEvent query(Qt::ImHints | Qt::ImEnterKeyType | Qt::ImSurroundingText | Qt::ImCursorPosition);
        QCoreApplication::sendEvent(&view, &query);
        const auto hints = Qt::InputMethodHints(query.value(Qt::ImHints).toInt());
        QVERIFY(hints.testFlag(Qt::ImhNoPredictiveText));
        QVERIFY(!hints.testFlag(Qt::ImhHiddenText));
        QCOMPARE(view.property("inputMethodHints").toInt(), int(hints));
        // A Return key: "Done", the default, closes the keyboard after every command.
        QCOMPARE(query.value(Qt::ImEnterKeyType).toInt(), int(Qt::EnterKeyReturn));
        // Something to delete before the cursor, for keyboards that delete that way.
        const QString around = query.value(Qt::ImSurroundingText).toString();
        QVERIFY(!around.isEmpty());
        QCOMPARE(query.value(Qt::ImCursorPosition).toInt(), int(around.size()));

#ifndef Q_OS_WIN
        // Such a keyboard removing the character before the cursor is a Backspace.
        QSignalSpy done(&view, &TerminalView::finished);
        QVERIFY(view.start("/bin/sh", {"-c", "stty -echo; read a; echo \"got:$a\""}));
        QTest::qWait(300);
        auto commit = [&](const QString &text, int from = 0, int length = 0) {
            QInputMethodEvent event;
            event.setCommitString(text, from, length);
            QCoreApplication::sendEvent(&view, &event);
        };
        commit("l"); commit("s"); commit("x"); commit("y");
        commit({}, -1, 1);       // one character back
        commit({}, -1, 1);
        commit(" "); commit("-"); commit("l");
        commit("\n");
        QVERIFY(done.wait(5000));
        QVERIFY2(view.screenText(10).contains("got:ls -l"), qPrintable(view.screenText(10)));
#endif
    }

    void touchKeyboardTextReachesTheProgram()
    {
#ifndef Q_OS_WIN
        // Gboard's way: a word grows as composing text, gets corrected, and is
        // committed later; then Enter. Each letter must reach the program.
        TerminalView view;
        view.setSize(QSizeF(800, 400));
        QSignalSpy done(&view, &TerminalView::finished);
        QVERIFY(view.start("/bin/sh", {"-c", "stty -echo; read a; echo \"got:$a\""}));
        QTest::qWait(300);
        auto compose = [&](const QString &preedit, const QString &commit = {}) {
            QInputMethodEvent event(preedit, {});
            if (!commit.isNull())
                event.setCommitString(commit);
            QCoreApplication::sendEvent(&view, &event);
        };
        compose("y");
        compose("ye");
        compose("yes");
        compose("yez");          // the keyboard changes its mind
        compose("yes");
        compose("", "yes");      // committed, nothing more to type
        compose("x");            // a new word, then deleted by the keyboard
        compose("");
        view.sendKey("enter");
        QVERIFY(done.wait(5000));
        QVERIFY2(view.screenText(10).contains("got:yes"), qPrintable(view.screenText(10)));
#endif
    }

    void loggedSessionsGetALogFile()
    {
        QTemporaryDir dir;
        Backend backend(dir.filePath("tether.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        backend.setTool("ssh", "/usr/bin/ssh");
        backend.setSetting("shareConnections", false);
        const QVariantMap launch = backend.launch({{"kind", "ssh"}, {"name", "My box/1"}, {"host", "box"}, {"reconnect", false},
                                                   {"options", "RemoteCommand=echo 'hi'"}, {"log", true}});
        QVERIFY2(!launch.contains("error"), qPrintable(launch.value("error").toString()));
        // The terminal writes the log itself, so the command is plain ssh.
        QCOMPARE(launch["program"].toString(), QString("/usr/bin/ssh"));
        QCOMPARE(launch["args"].toStringList(), QStringList({"-o", "RemoteCommand=echo 'hi'", "--", "box"}));
        QVERIFY(launch["logPath"].toString().startsWith(dir.filePath("logs") + "/My_box_1-"));
        QVERIFY(QFileInfo(dir.filePath("logs")).isDir());

        // The terminal appends what it prints to that file.
        TerminalView view;
        view.setLogFile(launch["logPath"].toString());
        QSignalSpy done(&view, &TerminalView::finished);
#ifndef Q_OS_WIN
        QVERIFY(view.start("/bin/sh", {"-c", "echo logged-line"}));
        QVERIFY(done.wait(5000));
        QFile log(launch["logPath"].toString());
        QVERIFY(log.open(QIODevice::ReadOnly));
        QVERIFY(log.readAll().contains("logged-line"));
#endif
    }

    void shellQuoting()
    {
        QCOMPARE(Backend::shellQuote("plain-1.2:3"), QString("plain-1.2:3"));
        QCOMPARE(Backend::shellQuote("a b"), QString("'a b'"));
        QCOMPARE(Backend::shellQuote("it's"), QString("'it'\\''s'"));
        QCOMPARE(Backend::shellQuote("$(rm -rf ~)"), QString("'$(rm -rf ~)'"));
        QCOMPARE(Backend::shellQuote(""), QString("''"));
    }

    void quickConnectParses()
    {
        QTemporaryDir dir;
        Backend backend(dir.filePath("tether.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        QVariantMap p = backend.quickConnect("pi@10.0.0.2:2222");
        QCOMPARE(p["user"].toString(), QString("pi"));
        QCOMPARE(p["host"].toString(), QString("10.0.0.2"));
        QCOMPARE(p["port"].toInt(), 2222);
        p = backend.quickConnect("/dev/ttyUSB0 9600");
        QCOMPARE(p["kind"].toString(), QString("serial"));
        QCOMPARE(p["baud"].toInt(), 9600);
        QVERIFY(backend.quickConnect("-oProxyCommand=x").contains("error"));
        QVERIFY(backend.quickConnect("host:notaport").contains("error"));
        QCOMPARE(backend.quickConnect("fe80::1").value("host").toString(), QString("fe80::1"));
    }

    void profilesAndSnippetsPersist()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("conf/tether.json");
        {
            Backend backend(file, dir.filePath("ssh_config"), dir.filePath("logs"));
            QVERIFY(backend.saveProfile({{"kind", "ssh"}, {"name", "Web"}, {"host", "web.example"},
                                         {"password", "never stored"}}).isEmpty());
            QVERIFY(!backend.saveProfile({{"kind", "ssh"}, {"name", "Bad"}, {"host", "-x"}}).isEmpty());
            QVERIFY(backend.saveSnippet({{"name", "Uptime"}, {"text", "uptime"}, {"run", true}}).isEmpty());
            backend.setFontSize(15);
            QCOMPARE(backend.profiles().size(), 1);

            // Saving an existing id replaces it.
            QVariantMap web = backend.profiles().first().toMap();
            web["name"] = "Web 2";
            QVERIFY(backend.saveProfile(web).isEmpty());
            QCOMPARE(backend.profiles().size(), 1);
        }
        QCOMPARE(QFile::permissions(file) & 0x0077, QFileDevice::Permissions());
        QFile raw(file);
        QVERIFY(raw.open(QIODevice::ReadOnly));
        QVERIFY(!raw.readAll().contains("never stored"));

        Backend reloaded(file, dir.filePath("ssh_config"), dir.filePath("logs"));
        QCOMPARE(reloaded.profiles().size(), 1);
        QCOMPARE(reloaded.profiles().first().toMap()["name"].toString(), QString("Web 2"));
        QCOMPARE(reloaded.snippets().size(), 1);
        QCOMPARE(reloaded.fontSize(), 15);
        QVERIFY(reloaded.removeProfile(reloaded.profiles().first().toMap()["id"].toString()).isEmpty());
        QCOMPARE(reloaded.profiles().size(), 0);
    }

    void settingsPersistAndAreChecked()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("tether.json");
        {
            Backend backend(file, dir.filePath("ssh_config"), dir.filePath("logs"));
            QCOMPARE(backend.settings().value("accent").toString(), QString("blue"));
            QSignalSpy changed(&backend, &Backend::settingsChanged);
            QVERIFY(backend.setSetting("accent", "green").isEmpty());
            QVERIFY(backend.setSetting("interfaceFontSize", 99).isEmpty());
            QVERIFY(backend.setSetting("blinkingCursor", true).isEmpty());
            QCOMPARE(changed.count(), 3);
            // Setting a value it already has changes nothing.
            QVERIFY(backend.setSetting("accent", "green").isEmpty());
            QCOMPARE(changed.count(), 3);
            QVERIFY(!backend.setSetting("nonsense", 1).isEmpty());
            QVERIFY(!backend.setSetting("lineSpacing", "wide").isEmpty());
            QVERIFY(!backend.setSetting("shell", dir.filePath("no-such-shell")).isEmpty());
            QCOMPARE(backend.settings().value("interfaceFontSize").toInt(), 18);
        }
        QFile raw(file);
        QVERIFY(raw.open(QIODevice::ReadOnly));
        const QJsonObject stored = QJsonDocument::fromJson(raw.readAll()).object().value("settings").toObject();
        // Only what differs from the defaults is written.
        QCOMPARE(stored.keys(), QStringList({"accent", "blinkingCursor", "interfaceFontSize"}));

        Backend reloaded(file, dir.filePath("ssh_config"), dir.filePath("logs"));
        QCOMPARE(reloaded.settings().value("accent").toString(), QString("green"));
        QCOMPARE(reloaded.settings().value("blinkingCursor").toBool(), true);
        QVERIFY(reloaded.resetSettings().isEmpty());
        QCOMPARE(reloaded.settings(), Backend::defaultSettings());
    }

    void localShellSetting()
    {
        QTemporaryDir dir;
        const QString shell = dir.filePath("myshell");
        write(shell, "#!/bin/sh\n");
        QFile::setPermissions(shell, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        Backend backend(dir.filePath("tether.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        QVERIFY(backend.setSetting("shell", shell).isEmpty());
        QCOMPARE(backend.launch({{"kind", "local"}}).value("program").toString(), shell);
    }

    void builtinSchemeColors()
    {
        QTemporaryDir dir;
        Theme theme(dir.filePath("no-omarchy"));
        QVERIFY(!theme.hasOmarchy());
        QVERIFY(theme.builtinSchemes().contains("Solarized Dark"));
        const QVariantMap solarized = theme.schemeColors("Solarized Dark");
        QCOMPARE(solarized.value("background").toString(), QString("#002b36"));
        QCOMPARE(solarized.value("palette").toList().size(), 16);
        QCOMPARE(solarized.value("preview").toList().first().toString(), QString("#dc322f"));
        QVERIFY(theme.schemeColors("Missing").isEmpty());
        // Without Omarchy, "omarchy" is Tether Dark.
        QCOMPARE(theme.schemeColors("omarchy").value("background"), theme.schemeColors("Tether Dark").value("background"));
    }

    void unreadableFileIsKept()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath("tether.json");
        write(file, "{ not json");
        Backend backend(file, dir.filePath("ssh_config"), dir.filePath("logs"));
        QVERIFY(!backend.loadError().isEmpty());
        QVERIFY(!QFile::exists(file));
        QCOMPARE(QDir(dir.path()).entryList({"tether.json.unreadable-*"}).size(), 1);
    }

    void droppedConnectionsReconnectAndTmuxKeepsTheShell()
    {
        QTemporaryDir dir;
        Backend backend(dir.filePath("tether.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        backend.setTool("ssh", "/usr/bin/ssh");
        const QVariantMap launch = backend.launch({{"kind", "ssh"}, {"name", "Web 01"}, {"host", "web"},
                                                   {"persistent", true}});
        QVERIFY2(!launch.contains("error"), qPrintable(launch.value("error").toString()));
        // The pane reconnects when ssh exits with 255; the command is plain ssh.
        QCOMPARE(launch["program"].toString(), QString("/usr/bin/ssh"));
        QVERIFY(launch["reconnect"].toBool());
        const QStringList args = launch["args"].toStringList();
        QCOMPARE(args.first(), QString("-t"));
        QVERIFY(args.contains("ServerAliveInterval=15"));
        QVERIFY(args.contains("ControlMaster=auto"));
        QCOMPARE(args.mid(args.size() - 3, 2), QStringList({"--", "web"}));
        QVERIFY(args.last().startsWith("tmux new-session -A -s tether-web-01 "));
        QVERIFY(!backend.launch({{"kind", "ssh"}, {"name", "x"}, {"host", "web"}, {"reconnect", false}})["reconnect"].toBool());

        // A split gets its own tmux session.
        const QVariantMap split = backend.launch({{"kind", "ssh"}, {"name", "Web 01"}, {"host", "web"},
                                                  {"persistent", true}, {"tmuxSession", "Web 01 2"}});
        QVERIFY(split["args"].toStringList().last().contains("tether-web-01-2"));
        QCOMPARE(Backend::tmuxSessionName("  "), QString("tether-session"));
    }

    void moshCarriesTheSshOptions()
    {
        QTemporaryDir dir;
        Backend backend(dir.filePath("tether.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        backend.setTool("ssh", "/usr/bin/ssh");
        backend.setTool("mosh", "/usr/bin/mosh");
        const QVariantMap launch = backend.launch({{"kind", "ssh"}, {"name", "Pi"}, {"host", "pi"}, {"user", "me"},
                                                   {"port", 2200}, {"transport", "mosh"}});
        QVERIFY2(!launch.contains("error"), qPrintable(launch.value("error").toString()));
        QCOMPARE(launch["program"].toString(), QString("/usr/bin/mosh"));
        const QStringList args = launch["args"].toStringList();
        QVERIFY(args[0].startsWith("--ssh=/usr/bin/ssh -p 2200 -l me"));
        QVERIFY(!args[0].contains("ControlMaster"));
        QCOMPARE(args.mid(1), QStringList({"--", "me@pi"}));
        QVERIFY(!Backend::validate({{"kind", "ssh"}, {"name", "x"}, {"host", "pi"}, {"transport", "mosh"},
                                    {"forwards", QVariantList{QVariantMap{{"type", "D"}, {"spec", "1080"}}}}}).isEmpty());
        QVERIFY(!Backend::validate({{"kind", "ssh"}, {"name", "x"}, {"host", "pi"}, {"transport", "telnet"}}).isEmpty());
    }

    void snippetPlaceholders()
    {
        QCOMPARE(Backend::snippetVariables("git checkout {{branch}} && echo {{ branch }} {{host}} {{env}}"),
                 QStringList({"branch", "env"}));
        QCOMPARE(Backend::snippetPayload({{"text", "deploy {{branch}} to {{host}} {{missing}}"}},
                                         {{"branch", "main"}, {"host", "web"}}),
                 QString("deploy main to web {{missing}}"));
        QVERIFY(Backend::snippetPayload({{"text", "{{date}}"}}).contains('-'));
    }

    void vaultLoginsFillHostsAndKeepPasswordsInTheKeyring()
    {
        QTemporaryDir dir;
        // A stand-in for secret-tool that records what it was asked.
        const QString fake = dir.filePath("secret-tool");
        QFile script(fake);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(QString("#!/bin/sh\necho \"$@\" >> '%1'\ncat >> '%1'\necho >> '%1'\n").arg(dir.filePath("calls")).toUtf8());
        script.close();
        QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        const QString file = dir.filePath("tether.json");
        Backend backend(file, dir.filePath("ssh_config"), dir.filePath("logs"));
        backend.setTool("ssh", "/usr/bin/ssh");
        backend.setTool("env", "/usr/bin/env");
        backend.setTool("secret-tool", fake);
        QVERIFY(!backend.saveIdentity({{"name", ""}}, {}).isEmpty());
        QCOMPARE(backend.saveIdentity({{"name", "Deploy"}, {"user", "deploy"}, {"identityFile", "~/.ssh/deploy"}}, "hunter2"),
                 QString());
        const QVariantMap login = backend.identities().value(0).toMap();
        QVERIFY(login["hasPassword"].toBool());

        QFile calls(dir.filePath("calls"));
        QVERIFY(calls.open(QIODevice::ReadOnly));
        const QString recorded = QString::fromUtf8(calls.readAll());
        QVERIFY(recorded.contains("store --label=Tether: Deploy service tether identity " + login["id"].toString()));
        QVERIFY(recorded.contains("hunter2"));

        // The password is nowhere in Tether's own file.
        QFile saved(file);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(!saved.readAll().contains("hunter2"));

        const QVariantMap launch = backend.launch({{"kind", "ssh"}, {"name", "Web"}, {"host", "web"},
                                                   {"identityId", login["id"]}, {"reconnect", false}});
        const QStringList args = launch["args"].toStringList();
        const QStringList env = launch["environment"].toStringList();
        QCOMPARE(launch["program"].toString(), QString("/usr/bin/ssh"));
        QVERIFY(env.contains("SSH_ASKPASS_REQUIRE=force"));
        QVERIFY(env.contains("TETHER_IDENTITY=" + login["id"].toString()));
        QVERIFY(args.contains("deploy"));
        QVERIFY(args.contains(QDir::homePath() + "/.ssh/deploy"));
        QVERIFY(!(args + env).join(' ').contains("hunter2"));

        QCOMPARE(backend.removeIdentity(login["id"].toString()), QString());
        QVERIFY(backend.identities().isEmpty());
    }

    void syncFolderCarriesHostsBetweenComputers()
    {
        QTemporaryDir dir;
        QDir().mkpath(dir.filePath("shared"));
        Backend a(dir.filePath("a.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        Backend b(dir.filePath("b.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        QVERIFY(!a.setSetting("syncFolder", dir.filePath("missing")).isEmpty());
        QCOMPARE(a.saveProfile({{"kind", "ssh"}, {"name", "Web"}, {"host", "web.example"}}), QString());
        QCOMPARE(a.setSetting("syncFolder", dir.filePath("shared")), QString());
        QVERIFY(QFile::exists(dir.filePath("shared/tether-hosts.json")));

        QCOMPARE(b.saveProfile({{"kind", "ssh"}, {"name", "Db"}, {"host", "db.example"}}), QString());
        QCOMPARE(b.setSetting("syncFolder", dir.filePath("shared")), QString());
        QCOMPARE(b.profiles().size(), 2);
        // a hears about b's host through the file watcher.
        QTRY_COMPARE_WITH_TIMEOUT(a.profiles().size(), 2, 5000);

        const QString id = a.profiles().value(0).toMap().value("id").toString();
        QCOMPARE(a.removeProfile(id), QString());
        QTRY_COMPARE_WITH_TIMEOUT(b.profiles().size(), 1, 5000);
    }

    void sftpReusesTheLogin()
    {
        QTemporaryDir dir;
        Backend backend(dir.filePath("tether.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        backend.setTool("sftp", "/usr/bin/sftp");
        const QVariantMap command = backend.sftpCommand({{"kind", "ssh"}, {"name", "Web"}, {"host", "web"},
                                                         {"user", "me"}, {"port", 2222},
                                                         {"options", "RemoteCommand=tmux\nCompression=yes"}});
        QVERIFY2(!command.contains("error"), qPrintable(command.value("error").toString()));
        const QStringList args = command["args"].toStringList();
        QCOMPARE(args.mid(0, 3), QStringList({"-q", "-b", "-"}));
        QVERIFY(args.contains("Port=2222"));
        QVERIFY(args.contains("User=me"));
        QVERIFY(args.contains("Compression=yes"));
        QVERIFY(!args.join(' ').contains("RemoteCommand"));
        QVERIFY(args.contains("ControlMaster=auto"));
        QCOMPARE(args.mid(args.size() - 2), QStringList({"--", "web"}));
        QVERIFY(command["environment"].toStringList().contains("TETHER_ASKPASS_NOTTY=1"));
        QVERIFY(backend.sftpCommand({{"kind", "serial"}}).contains("error"));
    }

    void sftpListingsAndQuoting()
    {
        const QString out =
            "sftp> pwd\nRemote working directory: /home/me\nsftp> ls -lan\n"
            "drwxr-xr-x    ? 1000     1000         4096 Nov  7 12:00 .\n"
            "drwxr-xr-x    ? 0        0            4096 Jan  2  2025 ..\n"
            "-rw-r--r--    ? 1000     1000          220 Nov  7 12:00 b file.txt\n"
            "drwx------    ? 1000     1000         4096 Nov  6 09:15 Projects\n"
            "lrwxrwxrwx    ? 1000     1000           11 Nov  6 09:15 latest -> Projects/x\n";
        QCOMPARE(SftpBrowser::parsePwd(out), QString("/home/me"));
        const QVariantList entries = SftpBrowser::parseListing(out);
        QCOMPARE(entries.size(), 3);
        QCOMPARE(entries[0].toMap()["name"].toString(), QString("Projects"));
        QVERIFY(entries[0].toMap()["folder"].toBool());
        QCOMPARE(entries[1].toMap()["name"].toString(), QString("b file.txt"));
        QCOMPARE(entries[1].toMap()["size"].toLongLong(), 220);
        QCOMPARE(entries[2].toMap()["name"].toString(), QString("latest"));
        QVERIFY(entries[2].toMap()["link"].toBool());
        QCOMPARE(SftpBrowser::quote("a \"b\" *.txt"), QString("\"a \\\"b\\\" \\*.txt\""));
    }

    void assistantAsksForOneCommand()
    {
        const QJsonObject body = QJsonDocument::fromJson(
            Assistant::requestBody("find big files", {{"kind", "ssh"}, {"host", "web"}, {"user", "me"}})).object();
        QCOMPARE(body["model"].toString(), QString("claude-opus-5-5"));
        QCOMPARE(body["fallbacks"].toString(), QString("default"));
        QCOMPARE(body["output_config"].toObject()["format"].toObject()["type"].toString(), QString("json_schema"));
        QVERIFY(body["messages"].toArray()[0].toObject()["content"].toString().contains("ssh to me@web"));

        const QJsonObject answerJson{{"command", "du -ah . | sort -rh | head"}, {"explanation", "Lists."}, {"risky", false}};
        const QJsonObject reply{
            {"stop_reason", "end_turn"},
            {"content", QJsonArray{QJsonObject{{"type", "thinking"}, {"thinking", ""}},
                                   QJsonObject{{"type", "text"},
                                               {"text", QString::fromUtf8(QJsonDocument(answerJson).toJson())}}}}};
        const QByteArray ok = QJsonDocument(reply).toJson();
        const QVariantMap answer = Assistant::parseResponse(200, ok);
        QCOMPARE(answer["command"].toString(), QString("du -ah . | sort -rh | head"));
        QVERIFY(!answer["risky"].toBool());
        QVERIFY(Assistant::parseResponse(200, QJsonDocument(QJsonObject{{"stop_reason", "refusal"}}).toJson()).contains("error"));
        QVERIFY(Assistant::parseResponse(401, "{}").contains("error"));

        QTemporaryDir dir;
        Assistant assistant(dir.filePath("key"));
        if (qEnvironmentVariableIsEmpty("ANTHROPIC_API_KEY")) {
            QCOMPARE(assistant.keySource(), QString());
            QCOMPARE(assistant.saveKey("sk-ant-test"), QString());
            QCOMPARE(assistant.keySource(), QString("saved"));
            QCOMPARE(QFileInfo(dir.filePath("key")).permissions() & (QFile::ReadOther | QFile::ReadGroup), QFile::Permissions());
        }
    }

    void updatesCompareVersionsAndPickTheRightDownload()
    {
        QCOMPARE(Updater::compareVersions("0.2.0", "0.1.9"), 1);
        QCOMPARE(Updater::compareVersions("v0.10.0", "0.9.9"), 1);
        QCOMPARE(Updater::compareVersions("1.0", "1.0.0"), 0);
        QCOMPARE(Updater::compareVersions("0.2.0-rc1", "0.2.1"), -1);
        const QVariantList assets{
            QVariantMap{{"name", "tether-debug-0.3.0-1-x86_64.pkg.tar.zst"}},
            QVariantMap{{"name", "tether-0.3.0-1-x86_64.pkg.tar.zst"}},
            QVariantMap{{"name", "Tether-0.3.0-setup.exe"}},
            QVariantMap{{"name", "Tether-0.3.0-x86_64.AppImage"}},
            QVariantMap{{"name", "Tether-0.3.0-android-arm64.apk"}}};
        QCOMPARE(Updater::pickAsset(assets, "pacman")["name"].toString(), QString("tether-0.3.0-1-x86_64.pkg.tar.zst"));
        QCOMPARE(Updater::pickAsset(assets, "windows")["name"].toString(), QString("Tether-0.3.0-setup.exe"));
        QCOMPARE(Updater::pickAsset(assets, "appimage")["name"].toString(), QString("Tether-0.3.0-x86_64.AppImage"));
        QCOMPARE(Updater::pickAsset(assets, "android")["name"].toString(), QString("Tether-0.3.0-android-arm64.apk"));
        QVERIFY(Updater::pickAsset(assets, "page").isEmpty());
        // A signed list: the file is taken from your own server's folder for
        // that version, then from GitHub's API link (it works when the
        // download links don't), then from the download link.
        Updater updater("oren1031/tether", "0.4.0");
        updater.setInstallKind("pacman");
        const QString sum(64, 'a');
        const QJsonObject list{{"version", "0.5.1"}, {"notes", "Fixes"}, {"page", "https://example.com/r"},
            {"assets", QJsonArray{QJsonObject{{"name", "tether-0.5.1-1-x86_64.pkg.tar.zst"}, {"size", 10}, {"sha256", sum},
                                              {"url", "https://elsewhere.example/evil.pkg.tar.zst"}}}}};
        const QJsonArray github{QJsonObject{{"name", "tether-0.5.1-1-x86_64.pkg.tar.zst"}, {"size", 99},
                                            {"digest", "sha256:def"},
                                            {"url", "https://api.github.com/repos/o/t/releases/assets/1"},
                                            {"browser_download_url", "https://github.com/o/t/releases/download/v0.5.1/x"}}};
        QString problem;
        QVERIFY2(updater.adopt(list, "0.5.1", QUrl("https://tether.example.com/latest.json"), github, &problem), qPrintable(problem));
        QCOMPARE(updater.latestVersion(), QString("0.5.1"));
        QVERIFY(updater.available());
        // The checksum and size are the signed list's, never GitHub's own.
        QCOMPARE(updater.expectedSha256(), sum);
        QCOMPARE(updater.expectedSize(), qint64(10));
        QCOMPARE(updater.sources().size(), 3);
        // A link written into the list is not followed: the place is fixed.
        QCOMPARE(updater.sources().value(0).toString(), QString("https://tether.example.com/0.5.1/tether-0.5.1-1-x86_64.pkg.tar.zst"));
        QCOMPARE(updater.sources().value(1).host(), QString("api.github.com"));
        QCOMPARE(updater.sources().value(2).host(), QString("github.com"));
        QCOMPARE(updater.releaseUrl(), QString("https://example.com/r"));
        // A list for another release than the one offered, or for one that
        // isn't newer than this Tether, is refused: no going back.
        QVERIFY(!updater.adopt(list, "0.5.2", {}, github, &problem));
        Updater newer("oren1031/tether", "0.5.1");
        newer.setInstallKind("pacman");
        QVERIFY(!newer.adopt(list, "0.5.1", {}, github, &problem));
        QVERIFY(!newer.available());
        // So is a file name that could leave the downloads folder.
        QJsonObject odd = list;
        odd["assets"] = QJsonArray{QJsonObject{{"name", "../tether-0.5.1-1-x86_64.pkg.tar.zst"}, {"size", 10}, {"sha256", sum}}};
        QVERIFY(!updater.adopt(odd, "0.5.1", {}, github, &problem));
        Updater none({}, "0.2.0");
        QVERIFY(!none.configured());
    }

    void updateServerMustBeHttps()
    {
        Updater updater("oren1031/tether", "0.5.4");
        updater.setMirror("https://tether.example.com/");
        QCOMPARE(updater.mirror(), QString("https://tether.example.com"));
        QCOMPARE(updater.mirrorProblem(), QString());
        // Over plain http anyone on the way could answer for the server.
        updater.setMirror("http://tether.example.com");
        QCOMPARE(updater.mirror(), QString());
        QVERIFY(updater.mirrorProblem().contains("https"));
        updater.setMirror("ftp://tether.example.com");
        QCOMPARE(updater.mirror(), QString());
        // This computer itself is how the tests stand in for a server.
        updater.setMirror("http://127.0.0.1:8123");
        QCOMPARE(updater.mirror(), QString("http://127.0.0.1:8123"));
        QCOMPARE(updater.mirrorProblem(), QString());
        updater.setMirror("");
        QCOMPARE(updater.mirrorProblem(), QString());
        QVERIFY(Updater::safeUrl(QUrl("https://github.com/x")));
        QVERIFY(!Updater::safeUrl(QUrl("http://github.com/x")));
        QVERIFY(!Updater::safeUrl(QUrl("file:///etc/passwd")));
        QVERIFY(!Updater::safeUrl(QUrl("http://127.0.0.1.example.com/x")));
    }

    void updatesInstallOnlyWhatTheSignedListNames()
    {
        SignedRelease release;
        if (!release.makeKey())
            QSKIP("ssh-keygen is needed to sign a test release");
        const QByteArray list = release.manifest("9.9.9"), signature = release.sign(list);
        QVERIFY(!signature.isEmpty());

        // A release as it should be: offered, downloaded, and kept.
        {
            WebServer web;
            release.publish(web, "9.9.9", list, signature, release.package);
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, web);
            QCOMPARE(checkedMessage(updater), QString("Tether 9.9.9 is available."));
            QVERIFY(updater.available());
            QCOMPARE(updater.source(), QString("GitHub"));
            updater.download();
            QTRY_COMPARE_WITH_TIMEOUT(updater.state(), QString("ready"), 30000);
            QFile got(updater.downloadedFile());
            QVERIFY(got.open(QIODevice::ReadOnly));
            QCOMPARE(got.readAll(), release.package);
            QCOMPARE(QDir(release.dir.filePath("updates")).entryList(QDir::Files).size(), 1);
            QCOMPARE(updater.installCommand().value("program").toString(), QString("sudo"));
        }
        // The file isn't the one the list names: not kept, nothing to install.
        {
            WebServer web;
            QDir(release.dir.filePath("updates")).removeRecursively();
            release.publish(web, "9.9.9", list, signature, QByteArray(release.package).replace("the", "THE"));
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, web);
            QCOMPARE(checkedMessage(updater), QString("Tether 9.9.9 is available."));
            updater.download();
            QTRY_COMPARE_WITH_TIMEOUT(updater.state(), QString("error"), 30000);
            QVERIFY2(updater.message().contains("checksum"), qPrintable(updater.message()));
            QCOMPARE(updater.downloadedFile(), QString());
            QVERIFY(updater.installCommand().isEmpty());
            QVERIFY(QDir(release.dir.filePath("updates")).entryList(QDir::Files).isEmpty());
        }
        // Longer than the list says: cut off, not kept.
        {
            WebServer web;
            release.publish(web, "9.9.9", list, signature, release.package + "and more");
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, web);
            QCOMPARE(checkedMessage(updater), QString("Tether 9.9.9 is available."));
            updater.download();
            QTRY_COMPARE_WITH_TIMEOUT(updater.state(), QString("error"), 30000);
            QVERIFY(QDir(release.dir.filePath("updates")).entryList(QDir::Files).isEmpty());
        }
        // The list was changed after it was signed.
        {
            WebServer web;
            release.publish(web, "9.9.9", QByteArray(list).replace("\"size\"", "\"size\" "), signature, release.package);
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, web);
            QVERIFY2(checkedMessage(updater).contains("signature isn't Tether's"), qPrintable(updater.message()));
            QCOMPARE(updater.state(), QString("error"));
            QVERIFY(!updater.available());
            updater.download();
            QCOMPARE(updater.state(), QString("error"));
            QVERIFY(!web.asked.contains("/assets/tether-9.9.9-1-x86_64.pkg.tar.zst"));
        }
        // Signed, but with somebody else's key.
        {
            WebServer web;
            QCOMPARE(QProcess::execute("ssh-keygen", {"-q", "-t", "ed25519", "-N", "", "-f", release.dir.filePath("other")}), 0);
            release.publish(web, "9.9.9", list, release.sign(list, release.dir.filePath("other")), release.package);
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, web);
            QVERIFY2(checkedMessage(updater).contains("signature isn't Tether's"), qPrintable(updater.message()));
            QVERIFY(!updater.available());
        }
        // No signature at all, as releases before signing were published.
        {
            WebServer web;
            release.publish(web, "9.9.9", list, QByteArray(), release.package);
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, web);
            QVERIFY2(checkedMessage(updater).contains("without a signature"), qPrintable(updater.message()));
            QVERIFY(!updater.available());
            // A quiet check (the one every few hours) says nothing, and offers nothing.
            updater.check(true);
            QTest::qWait(300);
            QTRY_VERIFY(web.asked.count("/repos/oren1031/tether/releases/latest") >= 2);
            QTest::qWait(200);
            QCOMPARE(updater.state(), QString("idle"));
            QVERIFY(!updater.available());
        }
        // A properly signed list of an older release, offered as the new one.
        {
            WebServer web;
            const QByteArray old = release.manifest("9.9.8");
            release.name = "tether-9.9.8-1-x86_64.pkg.tar.zst";
            release.publish(web, "9.9.9", old, release.sign(old), release.package);
            release.name.clear();
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, web);
            QVERIFY2(checkedMessage(updater).contains("is for Tether 9.9.8, not 9.9.9"), qPrintable(updater.message()));
            QVERIFY(!updater.available());
        }
        // A signed list with no checksum, a bad one, or no size for the file.
        for (const QJsonObject &change : {QJsonObject{{"sha256", QJsonValue()}}, QJsonObject{{"sha256", "abc"}},
                                          QJsonObject{{"size", QJsonValue()}}, QJsonObject{{"size", 0}}}) {
            WebServer web;
            const QByteArray bare = release.manifest("9.9.9", change);
            release.publish(web, "9.9.9", bare, release.sign(bare), release.package);
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, web);
            QVERIFY2(checkedMessage(updater).contains("no checksum or size"), qPrintable(updater.message()));
            QVERIFY(!updater.available());
        }
        // Without ssh-keygen the signature can't be checked, so nothing is offered.
        {
            WebServer web;
            release.publish(web, "9.9.9", list, signature, release.package);
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, web);
            updater.setKeygen({});
            QVERIFY2(checkedMessage(updater).contains("ssh-keygen"), qPrintable(updater.message()));
            QVERIFY(!updater.available());
        }
        // The key built into Tether is not the test's: its releases don't pass.
        {
            WebServer web;
            release.publish(web, "9.9.9", list, signature, release.package);
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, web);
            updater.setPublicKey({});
            QVERIFY2(checkedMessage(updater).contains("signature isn't Tether's"), qPrintable(updater.message()));
        }
    }

    void updatesFromYourOwnServerAreCheckedTheSameWay()
    {
        SignedRelease release;
        if (!release.makeKey())
            QSKIP("ssh-keygen is needed to sign a test release");
        const QByteArray list = release.manifest("9.9.9"), signature = release.sign(list);
        const QString file = "/9.9.9/tether-9.9.9-1-x86_64.pkg.tar.zst";

        // The server has the signed list and the file: GitHub isn't asked.
        {
            WebServer mirror, github;
            mirror.files = {{"/latest.json", list}, {"/latest.json.sig", signature}, {file, release.package}};
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, github);
            updater.setMirror(mirror.url());
            QCOMPARE(checkedMessage(updater), QString("Tether 9.9.9 is available."));
            QCOMPARE(updater.source(), QString("127.0.0.1"));
            updater.download();
            QTRY_COMPARE_WITH_TIMEOUT(updater.state(), QString("ready"), 30000);
            QVERIFY(github.asked.isEmpty());
        }
        // The server's list was tampered with: GitHub's release is used instead.
        {
            WebServer mirror, github;
            const QByteArray forged = QByteArray(list).replace("9.9.9", "9.9.7");
            mirror.files = {{"/latest.json", forged}, {"/latest.json.sig", signature},
                            {"/9.9.7/tether-9.9.7-1-x86_64.pkg.tar.zst", "another program"}};
            release.publish(github, "9.9.9", list, signature, release.package);
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, github);
            updater.setMirror(mirror.url());
            QCOMPARE(checkedMessage(updater), QString("Tether 9.9.9 is available."));
            QCOMPARE(updater.source(), QString("GitHub"));
        }
        // The server has the list without a signature, and GitHub can't be
        // reached: nothing is offered.
        {
            WebServer mirror, github;
            mirror.files = {{"/latest.json", list}, {file, release.package}};
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, github);
            updater.setMirror(mirror.url());
            checkedMessage(updater);
            QCOMPARE(updater.state(), QString("error"));
            QVERIFY(!updater.available());
        }
        // The server's copy of the file is wrong: GitHub's copy of the same
        // release is downloaded, and must match the same signed list.
        {
            WebServer mirror, github;
            QDir(release.dir.filePath("updates")).removeRecursively();
            mirror.files = {{"/latest.json", list}, {"/latest.json.sig", signature}, {file, QByteArray(release.package).replace("the", "THE")}};
            release.publish(github, "9.9.9", list, signature, release.package);
            Updater updater("oren1031/tether", "0.5.4");
            release.pointAt(updater, github);
            updater.setMirror(mirror.url());
            QCOMPARE(checkedMessage(updater), QString("Tether 9.9.9 is available."));
            QCOMPARE(updater.source(), QString("127.0.0.1"));
            updater.download();
            QTRY_COMPARE_WITH_TIMEOUT(updater.state(), QString("ready"), 30000);
            QCOMPARE(updater.source(), QString("GitHub"));
            QFile got(updater.downloadedFile());
            QVERIFY(got.open(QIODevice::ReadOnly));
            QCOMPARE(got.readAll(), release.package);
        }
    }

    void assistantCanUseClaudeCodeOnASubscription()
    {
        const QStringList args = Assistant::claudeCodeArguments("list ports", {{"kind", "local"}});
        QCOMPARE(args[0], QString("-p"));
        QVERIFY(args[1].contains("list ports"));
        QVERIFY(args.contains("--json-schema"));
        // No tools: Claude Code only answers, it never runs anything itself.
        QCOMPARE(args[args.indexOf("--tools") + 1], QString());
        QVERIFY(args.contains("--no-session-persistence"));

        const QJsonObject ok{{"type", "result"}, {"subtype", "success"}, {"is_error", false},
                             {"structured_output", QJsonObject{{"command", "ss -tlnp"}, {"explanation", "Lists."},
                                                               {"risky", false}}}};
        QCOMPARE(Assistant::parseClaudeCode(0, QJsonDocument(ok).toJson(), {})["command"].toString(), QString("ss -tlnp"));
        const QJsonObject bad{{"type", "result"}, {"subtype", "success"}, {"is_error", true}, {"result", "Not logged in"}};
        QCOMPARE(Assistant::parseClaudeCode(1, QJsonDocument(bad).toJson(), {})["error"].toString(), QString("Not logged in"));
        QVERIFY(Assistant::parseClaudeCode(1, "", "Please run /login").value("error").toString().contains("signed in"));

        QTemporaryDir dir;
        Assistant assistant(dir.filePath("key"));
        assistant.setProvider("claude-code");
        QCOMPARE(assistant.route(), assistant.claudeCode().isEmpty() ? QString() : QString("claude-code"));
        assistant.setProvider("nonsense");
        QCOMPARE(assistant.provider(), QString("auto"));
    }

    void forgettingAChangedHostKey()
    {
        QTemporaryDir dir;
        // A stand-in ssh-keygen that records what it was asked, so the real
        // known_hosts isn't touched.
        const QString fake = dir.filePath("ssh-keygen");
        write(fake, QString("#!/bin/sh\necho \"$@\" > '%1'\n").arg(dir.filePath("args")).toUtf8());
        QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        Backend backend(dir.filePath("tether.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        backend.setTool("ssh", "/usr/bin/ssh");
        backend.setTool("ssh-keygen", fake);
        QCOMPARE(backend.forgetHostKey({{"kind", "ssh"}, {"name", "x"}, {"host", "web"}, {"port", 2222}}), QString());
        QFile args(dir.filePath("args"));
        QVERIFY(args.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(args.readAll()).trimmed(), QString("-R [web]:2222"));
        QVERIFY(!backend.forgetHostKey({{"kind", "ssh"}, {"host", "-oX"}}).isEmpty());

#ifdef TETHER_LIBSSH
        write(dir.filePath("known_hosts"), "web ssh-ed25519 AAAA1\n[web]:2222 ssh-ed25519 AAAA2\nother ssh-ed25519 AAAA3\n");
        QVERIFY(SshSession::forgetHost(dir.filePath("known_hosts"), "web", 2222));
        QFile known(dir.filePath("known_hosts"));
        QVERIFY(known.open(QIODevice::ReadOnly));
        const QByteArray left = known.readAll();
        QVERIFY(left.contains("web ssh-ed25519 AAAA1"));
        QVERIFY(!left.contains("AAAA2"));
        QVERIFY(left.contains("other"));
#endif
    }

    void hostsUseBuiltInSshWithoutTheProgram()
    {
#ifdef TETHER_LIBSSH
        QTemporaryDir dir;
        Backend backend(dir.filePath("tether.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        backend.setTool("ssh", "");
        const QVariantMap launch = backend.launch({{"kind", "ssh"}, {"name", "Pi"}, {"host", "pi.local"},
                                                   {"user", "me"}, {"port", 2222}, {"persistent", true}});
        QVERIFY2(!launch.contains("error"), qPrintable(launch.value("error").toString()));
        const QVariantMap ssh = launch["ssh"].toMap();
        QCOMPARE(ssh["host"].toString(), QString("pi.local"));
        QCOMPARE(ssh["port"].toInt(), 2222);
        QCOMPARE(ssh["user"].toString(), QString("me"));
        QVERIFY(ssh["command"].toString().startsWith("tmux new-session -A -s tether-pi "));
        QVERIFY(ssh["knownHosts"].toString().endsWith("/known_hosts"));
        QVERIFY(launch["reconnect"].toBool());
        QVERIFY(backend.launch({{"kind", "ssh"}, {"name", "x"}, {"host", "h"}, {"jumpHost", "b"}}).contains("error"));
#else
        QSKIP("Built without libssh.");
#endif
    }

    void builtInSshLogsInAndRuns()
    {
#if defined(TETHER_LIBSSH) && !defined(Q_OS_WIN)
        const QString sshd = QFileInfo::exists("/usr/bin/sshd") ? "/usr/bin/sshd" : QFileInfo::exists("/usr/sbin/sshd") ? "/usr/sbin/sshd" : QString();
        const QString keygen = QStandardPaths::findExecutable("ssh-keygen");
        if (sshd.isEmpty() || keygen.isEmpty())
            QSKIP("No sshd here to test against.");
        QTemporaryDir dir;
        auto run = [](const QString &program, const QStringList &args) {
            QProcess p;
            p.start(program, args);
            return p.waitForFinished(30000) && p.exitCode() == 0;
        };
        QVERIFY(run(keygen, {"-q", "-t", "ed25519", "-N", "", "-f", dir.filePath("host")}));
        QVERIFY(run(keygen, {"-q", "-t", "ed25519", "-N", "pass phrase", "-f", dir.filePath("client")}));
        QFile::copy(dir.filePath("client.pub"), dir.filePath("authorized_keys"));
        const int port = 20000 + QRandomGenerator::global()->bounded(20000);
        write(dir.filePath("sshd_config"), QString(
            "Port %1\nListenAddress 127.0.0.1\nHostKey %2\nAuthorizedKeysFile %3\nPasswordAuthentication no\n"
            "KbdInteractiveAuthentication no\nUsePAM no\nStrictModes no\nPidFile %4\n")
            .arg(port).arg(dir.filePath("host"), dir.filePath("authorized_keys"), dir.filePath("sshd.pid")).toUtf8());
        QProcess server;
        server.start(sshd, {"-D", "-f", dir.filePath("sshd_config"), "-E", dir.filePath("sshd.log")});
        QVERIFY(server.waitForStarted());
        QTest::qWait(500);
        auto stopServer = qScopeGuard([&] { server.kill(); server.waitForFinished(); });

        QString user = qEnvironmentVariable("USER");
        if (user.isEmpty())
            user = qEnvironmentVariable("LOGNAME");
        const QVariantMap settings{{"host", "127.0.0.1"}, {"port", port}, {"user", user},
                                   {"identityFile", dir.filePath("client")}, {"passphrase", "pass phrase"},
                                   {"knownHosts", dir.filePath("known_hosts")},
                                   {"command", "echo ssh-ok-$((2+3)); exit 5"}};
        TerminalView view;
        view.setSize(QSizeF(900, 400));
        QSignalSpy finished(&view, &TerminalView::finished);
        QVERIFY(view.startSsh(settings));
        // A new host: answer the question in the terminal, as a person would.
        QTRY_VERIFY_WITH_TIMEOUT(view.screenText(10).contains("continue connecting"), 10000);
        view.sendText("yes\r");
        QVERIFY(finished.wait(15000));
        const QString text = view.screenText(20);
        QVERIFY2(text.contains("ssh-ok-5"), qPrintable(text));
        QCOMPARE(finished.first().first().toInt(), 5);

        // Known now: no question the second time.
        TerminalView again;
        again.setSize(QSizeF(900, 400));
        QSignalSpy done(&again, &TerminalView::finished);
        QVERIFY(again.startSsh(settings));
        QVERIFY(done.wait(15000));
        QVERIFY(!again.screenText(20).contains("continue connecting"));
        QVERIFY2(again.screenText(20).contains("ssh-ok-5"), qPrintable(again.screenText(20)));

        // Closing a tab on a running shell is immediate: it doesn't wait
        // for an exit status that a shell still running never sends.
        {
            QVariantMap shell = settings;
            shell.remove("command");
            auto *live = new TerminalView;
            live->setSize(QSizeF(900, 400));
            QVERIFY(live->startSsh(shell));
            QTest::qWait(1500);
            live->sendText("echo live-$((40+2))\r");
            QTRY_VERIFY2_WITH_TIMEOUT(live->screenText(30).contains("live-42"), qPrintable(live->screenText(30)), 10000);
            QElapsedTimer closing;
            closing.start();
            delete live;
            QVERIFY2(closing.elapsed() < 1000, qPrintable(QString("closing took %1 ms").arg(closing.elapsed())));
            // The connection's thread ends and cleans up on its own.
            QTest::qWait(1500);
        }

        // A passphrase typed on a phone keyboard: composing text with a
        // correction, a space, and stars for each character.
        {
            QVariantMap asking = settings;
            asking.remove("passphrase");
            TerminalView phone;
            // Narrow, like a phone: the prompt and the stars wrap.
            phone.setSize(QSizeF(380, 400));
            QSignalSpy ended(&phone, &TerminalView::finished);
            QVERIFY(phone.startSsh(asking));
            QTRY_VERIFY_WITH_TIMEOUT(phone.screenText(10).contains("Enter passphrase"), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(phone.secretInput(), 2000);
            auto compose = [&](const QString &preedit, const QString &commit = {}) {
                QInputMethodEvent event(preedit, {});
                if (!commit.isNull())
                    event.setCommitString(commit);
                QCoreApplication::sendEvent(&phone, &event);
            };
            compose("p"); compose("pa"); compose("pas"); compose("pass");
            compose("", "pass ");
            compose("phr"); compose("phrr"); compose("phr"); compose("phrase");
            compose("", "phrase");
            // The prompt and stars may wrap onto two lines, depending on the font.
            QTRY_VERIFY2_WITH_TIMEOUT(phone.screenText(10).remove('\n').contains("***********"),
                                      qPrintable(phone.screenText(10)), 3000);
            QVERIFY(!phone.screenText(10).contains("pass phrase"));
            phone.sendKey("enter");
            QVERIFY(ended.wait(15000));
            QCOMPARE(ended.first().first().toInt(), 5);
            QVERIFY(!phone.secretInput());
        }

        // The server gets a new key: the question says so; turning it down
        // ends without a reconnect (254), trusting it replaces the old key.
        server.kill();
        server.waitForFinished();
        QFile::remove(dir.filePath("host"));
        QFile::remove(dir.filePath("host.pub"));
        QVERIFY(run(keygen, {"-q", "-t", "ed25519", "-N", "", "-f", dir.filePath("host")}));
        server.start(sshd, {"-D", "-f", dir.filePath("sshd_config"), "-E", dir.filePath("sshd.log")});
        QVERIFY(server.waitForStarted());
        QTest::qWait(500);
        for (const bool trust : {false, true}) {
            TerminalView changed;
            changed.setSize(QSizeF(900, 400));
            QSignalSpy question(&changed, &TerminalView::hostKeyQuestion);
            QSignalSpy ended(&changed, &TerminalView::finished);
            QVERIFY(changed.startSsh(settings));
            QVERIFY(question.wait(10000));
            QVERIFY(question.first().at(3).toBool()); // changed
            changed.answerHostKey(trust);              // as the dialog does
            QVERIFY(ended.wait(15000));
            if (!trust) {
                QCOMPARE(ended.first().first().toInt(), 254);
                QVERIFY(changed.screenText(30).contains("Host key verification failed"));
            } else {
                QVERIFY2(changed.screenText(30).contains("ssh-ok-5"), qPrintable(changed.screenText(30)));
            }
        }
        // And from then on the new key is simply known.
        TerminalView after;
        after.setSize(QSizeF(900, 400));
        QSignalSpy afterDone(&after, &TerminalView::finished);
        QSignalSpy afterQuestion(&after, &TerminalView::hostKeyQuestion);
        QVERIFY(after.startSsh(settings));
        QVERIFY(afterDone.wait(15000));
        QCOMPARE(afterQuestion.count(), 0);
        QCOMPARE(afterDone.first().first().toInt(), 5);
#else
        QSKIP("Built without libssh.");
#endif
    }

    void snippetPayloadPressesEnter()
    {
        QCOMPARE(Backend::snippetPayload({{"text", "cd /tmp\r\nls"}, {"run", true}}), QString("cd /tmp\rls\r"));
        QCOMPARE(Backend::snippetPayload({{"text", "partial "}, {"run", false}}), QString("partial "));
        QCOMPARE(Backend::snippetPayload({{"text", "ls\n"}, {"run", true}}), QString("ls\r"));
    }

    void sshConfigHosts()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("conf.d");
        write(dir.filePath("config"),
              "# comment\nHost web db\n  HostName 10.0.0.5\n  User admin\n  Port 2200\n"
              "Host *.internal !skip\n  User x\nInclude conf.d/*.conf\nHost=router\n");
        write(dir.filePath("conf.d/a.conf"), "Host nas\n  HostName nas.lan\n");
        const QVariantList hosts = Backend::parseSshConfig(dir.filePath("config"));
        QStringList names, details;
        for (const QVariant &h : hosts) {
            names << h.toMap()["name"].toString();
            details << h.toMap()["detail"].toString();
        }
        QCOMPARE(names, QStringList({"web", "db", "nas", "router"}));
        QCOMPARE(details[0], QString("admin@10.0.0.5:2200"));
        QCOMPARE(details[2], QString("nas.lan"));
    }

    void serialDevicesAreFound()
    {
        QTemporaryDir dir;
        for (const char *name : {"ttyUSB10", "ttyUSB2", "ttyACM0", "tty0", "ttyS0"})
            write(dir.filePath(QString::fromLatin1(name)), "");
        QDir(dir.path()).mkpath("serial/by-id");
        QVERIFY(QFile::link("../../ttyUSB2", dir.filePath("serial/by-id/usb-FTDI_FT232R_USB_UART_A5-if00-port0")));

        SerialPorts ports(dir.path(), 0);
        QStringList paths;
        for (const QVariant &d : ports.devices())
            paths << QFileInfo(d.toMap()["path"].toString()).fileName();
        QCOMPARE(paths, QStringList({"ttyACM0", "ttyUSB2", "ttyUSB10"}));
        const QVariantMap ftdi = ports.devices()[1].toMap();
        QCOMPARE(ftdi["label"].toString(), QString("FTDI FT232R USB UART A5"));
        QVERIFY(ftdi["stablePath"].toString().endsWith("by-id/usb-FTDI_FT232R_USB_UART_A5-if00-port0"));
        QVERIFY(ftdi["accessible"].toBool());

        QFile::setPermissions(dir.filePath("ttyACM0"), QFileDevice::Permissions());
        ports.refresh();
        QVERIFY(!ports.devices()[0].toMap()["accessible"].toBool());
        QVERIFY(ports.devices()[0].toMap()["problem"].toString().contains("No permission"));
        QVERIFY(SerialPorts::problem(dir.filePath("gone")).contains("not connected"));
    }

    void terminalColorsFollowTheTheme()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.filePath("theme/colors.toml"),
              "accent = \"#112233\"\nbackground = \"#000000\"\nforeground = \"#ffffff\"\nred = \"#ff0000\"\n"
              "bright_red = \"#ff8080\"\nmuted = \"#7a7a7a\"\ngreen = \"#00ff00\"\n");

        Theme theme(dir.path());
        QVERIFY(theme.hasOmarchy());
        QCOMPARE(theme.terminalBackground(), QString("#000000"));
        const QVariantList palette = theme.schemeColors("omarchy").value("palette").toList();
        QCOMPARE(palette.size(), 16);
        QCOMPARE(palette[0].toString(), QString("#000000"));  // black is the background
        QCOMPARE(palette[1].toString(), QString("#ff0000"));
        QCOMPARE(palette[9].toString(), QString("#ff8080"));
        QCOMPARE(palette[8].toString(), QString("#7a7a7a"));  // bright black is muted
        QCOMPARE(palette[10].toString(), QString("#00ff00")); // missing brights use the normal color
        QCOMPARE(palette[7].toString(), QString("#ffffff"));
    }

    // Servers send whatever they like; the terminal must take any bytes
    // without crashing (and under the sanitizers, without misusing memory).
    void terminalSurvivesRandomOutput()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 300));
        QRandomGenerator random(20261109); // the same bytes on every run
        const QList<QByteArray> pieces{"\x1b[", "\x1b]", "\x1b[?1049h", "\x1b[?1049l", "\x1b[2J", "\x1b[999;999H", "\x1b[38;2;", "\x1bP",
                                       "\x1b\\", "\x07", "\x9b", "\x90", "\xff\xfe", "\xe2\x82", "\x1b[1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1m",
                                       "\x1b]52;c;AAAA\x07", "\x1b[2000000000C", "\x1b[5000S", "\x1b[@", "\x1b[3000M", "\x1b#8", "\x1b(0"};
        for (int round = 0; round < 400; ++round) {
            QByteArray data;
            const int length = 1 + int(random.bounded(2048));
            while (data.size() < length) {
                if (random.bounded(3) == 0) {
                    data += pieces.at(int(random.bounded(uint(pieces.size()))));
                } else {
                    data += char(random.bounded(256));
                }
            }
            view.feed(data);
            if (round % 50 == 0)
                view.setSize(QSizeF(100 + random.bounded(700), 60 + random.bounded(400)));
            // Reading the screen back walks every cell.
            QVERIFY(view.screenText(view.rows()).size() >= 0);
            QVERIFY(view.lineText(view.scrollbackLines()).size() >= 0);
        }
        view.feed("\x1b" "c\x1b[2J\x1b[Hstill works"); // reset, then plain text
        QVERIFY(view.screenText(view.rows()).contains("still works"));
    }

    void terminalKeepsTheScreenAndHistory()
    {
        TerminalView view;
        view.setSize(QSizeF(800, 400));
        const int rows = view.rows();
        QVERIFY(rows > 5);
        view.feed("hello \x1b[1;31mred\x1b[0m world\r\nsecond line");
        const int first = view.scrollbackLines();
        QCOMPARE(view.lineText(first), QString("hello red world"));
        QCOMPARE(view.lineText(first + 1), QString("second line"));

        // Enough lines to scroll pushes the first ones into the history.
        QByteArray lots;
        for (int i = 0; i < rows + 10; ++i)
            lots += "line " + QByteArray::number(i) + "\r\n";
        view.feed(lots);
        QVERIFY(view.scrollbackLines() >= 10);
        QCOMPARE(view.lineText(0), QString("hello red world"));
        QVERIFY(view.screenText(3).contains(QString("line %1").arg(rows + 9)));

        // Wide characters take two cells and come back whole.
        view.feed("\x1b[2J\x1b[H日本 ok");
        QCOMPARE(view.lineText(view.scrollbackLines()), QString("日本 ok"));

        // The title comes from OSC 0.
        view.feed("\x1b]0;my title\x07");
        QCOMPARE(view.title(), QString("my title"));
    }

    void terminalRunsAProgramInAPty()
    {
#ifndef Q_OS_WIN
        TerminalView view;
        view.setSize(QSizeF(800, 400));
        QSignalSpy finished(&view, &TerminalView::finished);
        QVERIFY(view.start("/bin/sh", {"-c", "printf 'cols=%s\\n' \"$(tput cols 2>/dev/null || stty size | cut -d' ' -f2)\"; echo \"term=$TERM\"; exit 3"}));
        QVERIFY(finished.wait(5000));
        QCOMPARE(finished.first().first().toInt(), 3);
        const QString text = view.screenText(10);
        QVERIFY2(text.contains(QString("cols=%1").arg(view.columns())), qPrintable(text));
        QVERIFY2(text.contains("term=xterm-256color"), qPrintable(text));
        QVERIFY(!view.running());

        // Typing reaches the program.
        QSignalSpy done(&view, &TerminalView::finished);
        QVERIFY(view.start("/bin/sh", {"-c", "read line; echo \"got:$line\""}));
        view.sendText("abc\r");
        QVERIFY(done.wait(5000));
        QVERIFY2(view.screenText(10).contains("got:abc"), qPrintable(view.screenText(10)));
#endif
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "en\n");
        Translator translator(dir.path() + "/language", ":/no-catalogs");
        QCOMPARE(Translator::pluralCategory("en", 1), QString("one"));
        QCOMPARE(Translator::pluralCategory("ru", 22), QString("few"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
    }

private:
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(TetherTests)
#include "tether_tests.moc"
