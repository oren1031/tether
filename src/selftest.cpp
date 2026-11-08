#include "selftest.h"

#include "credentials.h"
#include "terminalview.h"
#include "updater.h"

#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QImage>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#include <QCoreApplication>
#endif
#include <QInputMethodEvent>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#include <cstdio>

#ifdef TETHER_LIBSSH
#include <libssh/libssh.h>
#endif

// Checks, on the machine it runs on, the parts that differ between systems:
// the pseudo-terminal, the vault's password store, and answering ssh as its
// askpass helper. CI runs it on every system Tether is built for.
namespace {
int failures = 0;
QStringList qmlProblems;
QtMessageHandler previousHandler = nullptr;

// Android has no console; its log (logcat) gets the report there.
void print(const QString &line) {
#ifdef Q_OS_ANDROID
    qInfo().noquote() << "tether-self-test:" << line;
#else
    std::printf("%s\n", qPrintable(line));
    std::fflush(stdout);
#endif
}

void report(bool ok, const QString &what, const QString &detail = {}) {
    print(QString("%1 %2%3").arg(ok ? "ok  " : "FAIL", what, detail.isEmpty() ? QString() : "  (" + detail + ")"));
    if (!ok)
        ++failures;
}

// Warnings from Tether's own QML files are failures; Qt's own chatter isn't.
void collect(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type != QtDebugMsg && type != QtInfoMsg && message.contains("qrc:/"))
        qmlProblems << message;
    if (previousHandler)
        previousHandler(type, context, message);
}
} // namespace

void startSelfTest() {
    previousHandler = qInstallMessageHandler(collect);
}

int finishSelfTest(QQmlApplicationEngine &engine, const QStringList &arguments) {
    // The interface: it loads, and nothing in it complains once it's up.
    report(!engine.rootObjects().isEmpty(), "load the interface");
    auto *window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    {
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 2500)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    const int shotIndex = int(arguments.indexOf("--self-test")) + 1;
    const QString shot = shotIndex < arguments.size() ? arguments.at(shotIndex) : QString();
#ifndef Q_OS_ANDROID
    // On Android the emulator test captures the screen itself.
    if (window && !shot.isEmpty() && shot.endsWith(".png")) {
        const QImage image = window->grabWindow();
        report(!image.isNull() && image.save(shot), "save a screenshot", shot);
    }
#endif
    report(qmlProblems.isEmpty(), "no interface errors", qmlProblems.join(" | ").left(600));

    // 1. A program in a terminal: its output, its exit code, and the size it sees.
    {
        TerminalView view;
        view.setSize(QSizeF(900, 400));
        QEventLoop loop;
        int exitCode = -1;
        QObject::connect(&view, &TerminalView::finished, &loop, [&](int code) {
            exitCode = code;
            loop.quit();
        });
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
#ifdef Q_OS_WIN
        const bool started = view.start(qEnvironmentVariable("COMSPEC", "cmd.exe"),
                                        {"/d", "/c", "echo tether-self-test & mode con | findstr /i Columns & exit 7"});
#elif defined(Q_OS_ANDROID)
        const bool started = view.start("/system/bin/sh", {"-c", "echo tether-self-test; stty size; exit 7"});
#else
        const bool started = view.start("/bin/sh", {"-c", "echo tether-self-test; stty size; exit 7"});
#endif
        report(started, "start a program in a terminal");
        if (started)
            loop.exec();
        // ConPTY can send its last output just after the process ends.
        QElapsedTimer settle;
        settle.start();
        while (settle.elapsed() < 500)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        const QString text = view.screenText(40);
        report(text.contains("tether-self-test"), "see what it printed", text.left(300).replace('\n', " | "));
        report(exitCode == 7, "get its exit code", QString::number(exitCode));
        report(text.contains(QString::number(view.columns())), "it sees the terminal's width",
               QString("%1 columns").arg(view.columns()));
    }

#ifdef Q_OS_WIN
    // 2. The vault's store, and tether.exe answering ssh's password prompt.
    {
        const QString id = "self-test-" + QString::number(QCoreApplication::applicationPid());
        const QString secret = "s3cret phrase";
        const QString error = Credentials::store(id, "Tether self-test", secret);
        report(error.isEmpty(), "save a password in Credential Manager", error);
        report(Credentials::lookup(id) == secret, "read it back");

        QProcess askpass;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("TETHER_ASKPASS", "1");
        env.insert("TETHER_IDENTITY", id);
        askpass.setProcessEnvironment(env);
        askpass.start(QCoreApplication::applicationFilePath(), {"user@host's password: "});
        askpass.waitForFinished(15000);
        const QString answer = QString::fromUtf8(askpass.readAllStandardOutput()).trimmed();
        report(askpass.exitCode() == 0 && answer == secret, "answer ssh's password prompt as askpass",
               QString("exit %1").arg(askpass.exitCode()));

        Credentials::remove(id);
        report(Credentials::lookup(id).isEmpty(), "remove the password");
    }
#endif

    // 3. ssh, which Tether runs for hosts.
    // 3. Optionally a real login with the built-in ssh:
    //    --ssh-test user@host:port <private key file>
#ifdef TETHER_LIBSSH
    const int sshIndex = int(arguments.indexOf("--ssh-test"));
    if (sshIndex >= 0 && sshIndex + 2 < arguments.size()) {
        const QString target = arguments.at(sshIndex + 1), key = arguments.at(sshIndex + 2);
        const QString user = target.section('@', 0, 0), hostPort = target.section('@', 1);
        TerminalView view;
        view.setSize(QSizeF(900, 400));
        QEventLoop loop;
        int exitCode = -1;
        QObject::connect(&view, &TerminalView::finished, &loop, [&](int code) {
            exitCode = code;
            loop.quit();
        });
        // A first connection asks about the host key; answer as a person would.
        bool answered = false;
        QTimer watch;
        watch.setInterval(200);
        QObject::connect(&watch, &QTimer::timeout, &loop, [&] {
            if (!answered && view.screenText(20).contains("continue connecting")) {
                answered = true;
                // Typed the way a phone keyboard types: composing, then committed.
                for (const QString &preedit : {QString("y"), QString("ye"), QString("yes")}) {
                    QInputMethodEvent composing(preedit, {});
                    QCoreApplication::sendEvent(&view, &composing);
                }
                QInputMethodEvent commit;
                commit.setCommitString("yes");
                QCoreApplication::sendEvent(&view, &commit);
                view.sendKey("enter");
            }
        });
        watch.start();
        QTimer::singleShot(40000, &loop, &QEventLoop::quit);
        view.startSsh({{"host", hostPort.section(':', 0, 0)}, {"port", hostPort.section(':', 1).toInt()},
                       {"user", user}, {"identityFile", key},
                       {"knownHosts", QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/selftest_known_hosts"},
                       {"command", "echo tether-ssh-$((40+2)); exit 4"}});
        loop.exec();
        const QString text = view.screenText(30);
        report(text.contains("tether-ssh-42"), "log in with the built-in ssh and run a command",
               text.right(300).replace('\n', " | "));
        report(exitCode == 4, "get the remote exit code", QString::number(exitCode));
    }
#endif

#ifdef Q_OS_ANDROID
    // 4. Optionally, an update handed to Android's installer: this app's own
    //    APK, from where updates download to. The emulator test then checks
    //    the installer came up.
    if (arguments.contains("--try-install")) {
        const QJniObject context = QNativeInterface::QAndroidApplication::context();
        const QString own = context.callObjectMethod("getPackageCodePath", "()Ljava/lang/String;").toString();
        const QString directory = Updater::updatesDirectory();
        QDir().mkpath(directory);
        const QString copy = directory + "/self-test.apk";
        QFile::remove(copy);
        const bool copied = QFile::copy(own, copy);
        report(copied, "download an update where the installer can read it", copy);
        report(copied && Updater::openAndroidInstaller(copy), "hand it to Android's installer");
        // Stay open a moment, as Tether does when updating, so the emulator
        // test sees the installer over it.
        QElapsedTimer stay;
        stay.start();
        while (stay.elapsed() < 12000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
#endif

    const QString ssh = QStandardPaths::findExecutable("ssh");
#ifdef TETHER_LIBSSH
    report(true, "built-in ssh", QString("libssh %1").arg(QString::fromLatin1(ssh_version(0))));
#endif
    print("info ssh program: " + (ssh.isEmpty() ? QString("not found") : ssh));
    print(failures ? "SELF-TEST FAILED" : "SELF-TEST PASSED");
    qInstallMessageHandler(previousHandler);
    return failures ? 1 : 0;
}
