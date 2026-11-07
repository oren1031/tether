#include "selftest.h"

#include "credentials.h"
#include "terminalview.h"

#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QImage>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#include <cstdio>

// Checks, on the machine it runs on, the parts that differ between systems:
// the pseudo-terminal, the vault's password store, and answering ssh as its
// askpass helper. CI runs it on every system Tether is built for.
namespace {
int failures = 0;
QStringList qmlProblems;
QtMessageHandler previousHandler = nullptr;

void report(bool ok, const QString &what, const QString &detail = {}) {
    std::printf("%s %s%s\n", ok ? "ok  " : "FAIL", qPrintable(what),
                detail.isEmpty() ? "" : qPrintable("  (" + detail + ")"));
    std::fflush(stdout);
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
    if (window && !shot.isEmpty() && shot.endsWith(".png")) {
        const QImage image = window->grabWindow();
        report(!image.isNull() && image.save(shot), "save a screenshot", shot);
    }
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
    const QString ssh = QStandardPaths::findExecutable("ssh");
    std::printf("info ssh: %s\n", ssh.isEmpty() ? "not found" : qPrintable(ssh));

    std::printf("%s\n", failures ? "SELF-TEST FAILED" : "SELF-TEST PASSED");
    std::fflush(stdout);
    qInstallMessageHandler(previousHandler);
    return failures ? 1 : 0;
}
