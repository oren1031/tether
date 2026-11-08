#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QProcess>
#include <QInputMethodEvent>
#include <QSignalSpy>
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

class TetherTests : public QObject {
    Q_OBJECT

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
        Updater none({}, "0.2.0");
        QVERIFY(!none.configured());
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

        // A passphrase typed on a phone keyboard: composing text with a
        // correction, a space, and stars for each character.
        {
            QVariantMap asking = settings;
            asking.remove("passphrase");
            TerminalView phone;
            phone.setSize(QSizeF(900, 400));
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
