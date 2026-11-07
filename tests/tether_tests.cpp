#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "assistant.h"
#include "backend.h"
#include "sftp.h"
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

    void loggedSessionsRunThroughScript()
    {
        QTemporaryDir dir;
        Backend backend(dir.filePath("tether.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        backend.setTool("ssh", "/usr/bin/ssh");
        backend.setTool("script", "/usr/bin/script");
        backend.setTool("env", "/usr/bin/env");
        backend.setSetting("shareConnections", false);
        const QVariantMap launch = backend.launch({{"kind", "ssh"}, {"name", "My box/1"}, {"host", "box"}, {"reconnect", false},
                                                   {"options", "RemoteCommand=echo 'hi'"}, {"log", true}});
        QVERIFY2(!launch.contains("error"), qPrintable(launch.value("error").toString()));
        const QStringList args = launch["args"].toStringList();
        QCOMPARE(launch["program"].toString(), QString("/usr/bin/env"));
        QCOMPARE(args.mid(0, 6), QStringList({"SHELL=/bin/sh", "/usr/bin/script", "--quiet", "--flush", "--return", "--command"}));
        QCOMPARE(args[6], QString("/usr/bin/ssh -o 'RemoteCommand=echo '\\''hi'\\''' -- box"));
        QVERIFY(args[7].startsWith(dir.filePath("logs") + "/My_box_1-"));
        QCOMPARE(args[7], launch["logPath"].toString());
        QVERIFY(QFileInfo(dir.filePath("logs")).isDir());
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
        write(dir.filePath("Night.colorscheme"),
              "[Background]\nColor=1,2,3\n\n[Foreground]\nColor=250,250,250\n\n[Color1]\nColor=255,0,0\n");
        write(dir.filePath("notes.txt"), "not a scheme");
        Theme theme(dir.filePath("no-omarchy"));
        theme.setBuiltinSchemeDirectory(dir.path());
        QCOMPARE(theme.builtinSchemes(), QStringList({"Night"}));
        const QVariantMap night = theme.schemeColors("Night");
        QCOMPARE(night.value("background").toString(), QString("#010203"));
        QCOMPARE(night.value("foreground").toString(), QString("#fafafa"));
        QCOMPARE(night.value("palette").toList().first().toString(), QString("#ff0000"));
        QVERIFY(theme.schemeColors("Missing").isEmpty());
        QCOMPARE(theme.schemeColors("omarchy").value("palette").toList().size(), 6);
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
        backend.setTool("env", "/usr/bin/env");
        const QVariantMap launch = backend.launch({{"kind", "ssh"}, {"name", "Web 01"}, {"host", "web"},
                                                   {"persistent", true}});
        QVERIFY2(!launch.contains("error"), qPrintable(launch.value("error").toString()));
        QCOMPARE(launch["program"].toString(), QString("/usr/bin/env"));
        const QStringList args = launch["args"].toStringList();
        // env TETHER_LOST=… /bin/sh -c 'loop' tether /usr/bin/ssh -t … -- web 'tmux …'
        QVERIFY(args[0].startsWith("TETHER_LOST="));
        QCOMPARE(args.mid(1, 2), QStringList({"/bin/sh", "-c"}));
        QVERIFY(args[3].contains("255"));
        QCOMPARE(args.mid(4, 3), QStringList({"tether", "/usr/bin/ssh", "-t"}));
        QVERIFY(args.contains("ServerAliveInterval=15"));
        QVERIFY(args.contains("ControlMaster=auto"));
        QCOMPARE(args.mid(args.size() - 3, 2), QStringList({"--", "web"}));
        QVERIFY(args.last().startsWith("tmux new-session -A -s tether-web-01 "));

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
        QCOMPARE(launch["program"].toString(), QString("/usr/bin/env"));
        QVERIFY(args.contains("SSH_ASKPASS_REQUIRE=force"));
        QVERIFY(args.contains("TETHER_IDENTITY=" + login["id"].toString()));
        QVERIFY(args.contains("deploy"));
        QVERIFY(args.contains(QDir::homePath() + "/.ssh/deploy"));
        QVERIFY(!args.join(' ').contains("hunter2"));

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
            QVariantMap{{"name", "Tether-0.3.0-setup.exe"}}};
        QCOMPARE(Updater::pickAsset(assets, "pacman")["name"].toString(), QString("tether-0.3.0-1-x86_64.pkg.tar.zst"));
        QCOMPARE(Updater::pickAsset(assets, "windows")["name"].toString(), QString("Tether-0.3.0-setup.exe"));
        QVERIFY(Updater::pickAsset(assets, "page").isEmpty());
        Updater none({}, "0.2.0");
        QVERIFY(!none.configured());
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
              "bright_red = \"#ff8080\"\nmuted = \"#7a7a7a\"\n");

        Theme theme(dir.path());
        QCOMPARE(theme.terminalScheme(), QString(Theme::FallbackScheme));
        QCOMPARE(theme.terminalBackground(), QString("#000000"));

        QStringList loaded;
        theme.setSchemeLoader([&](const QString &path) { loaded << path; return true; }, dir.filePath("schemes"));
        QCOMPARE(loaded.size(), 1);
        QVERIFY(theme.terminalScheme().startsWith("omarchy-"));
        QFile scheme(loaded.first());
        QVERIFY(scheme.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(scheme.readAll());
        QVERIFY(text.contains("[Color1]\nColor=255,0,0\n"));
        QVERIFY(text.contains("[Color1Intense]\nColor=255,128,128\n"));
        QVERIFY(text.contains("[Color0Intense]\nColor=122,122,122\n"));
        QVERIFY(text.contains("[Background]\nColor=0,0,0\n"));
        // Missing bright colors fall back to the normal ones.
        QVERIFY(Theme::schemeText({{"green", "#00ff00"}}).contains("[Color2Intense]\nColor=0,255,0\n"));

        // A scheme the widget refuses leaves the built-in one.
        Theme refused(dir.path());
        refused.setSchemeLoader([](const QString &) { return false; }, dir.filePath("schemes"));
        QCOMPARE(refused.terminalScheme(), QString(Theme::FallbackScheme));
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
