// tether: SSH hosts and serial ports in tabs and split panes.

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml/qqml.h>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QUrl>


#include "assistant.h"
#include "backend.h"
#include "sftp.h"
#include "terminalview.h"
#include "updater.h"
#include "credentials.h"
#include "selftest.h"

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif
#ifndef TETHER_REPO
#define TETHER_REPO ""
#endif
#include "serialports.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // ssh runs tether.exe as its askpass helper on Windows: answer and exit.
    if (qEnvironmentVariable("TETHER_ASKPASS") == "1")
        return Credentials::askpass(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString());

    QGuiApplication app(argc, argv);
    // tether --self-test [screenshot.png]: checks this system, loads the
    // whole interface, and reports; nothing is saved.
    // On Android the arguments come from whichever app started Tether, so
    // only the build CI tests in an emulator (CONFIG+=tether_test) has it.
#if defined(Q_OS_ANDROID) && !defined(TETHER_TEST_HOOKS)
    const bool selfTest = false;
#else
    const bool selfTest = app.arguments().contains("--self-test");
#endif
    if (selfTest)
        startSelfTest();
    app.setApplicationName("tether");
    app.setApplicationVersion(APP_VERSION);

    // Matches the window to tether.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("tether");
    app.setWindowIcon(QIcon::fromTheme("tether"));

    // The Basic style is the plainest to draw over; every control Tether shows
    // is styled in QML against the Style singleton.
    QQuickStyle::setStyle("Basic");
    qmlRegisterSingletonType(QUrl("qrc:/Style.qml"), "Tether", 1, 0, "Style");
    qmlRegisterType<SftpBrowser>("Tether", 1, 0, "SftpBrowser");
    qmlRegisterType<TerminalView>("Tether", 1, 0, "TerminalView");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;
    SerialPorts serialPorts;
    Assistant assistant;
    Updater updater(QString(TETHER_REPO).trimmed(), APP_VERSION);
    updater.setMirror(backend.settings().value("updateMirror").toString());
    QObject::connect(&backend, &Backend::settingsChanged, &updater,
                     [&] { updater.setMirror(backend.settings().value("updateMirror").toString()); });

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("serialPorts", &serialPorts);
    engine.rootContext()->setContextProperty("assistant", &assistant);
    assistant.setProvider(backend.settings().value("aiProvider").toString());
    QObject::connect(&backend, &Backend::settingsChanged, &assistant,
                     [&] { assistant.setProvider(backend.settings().value("aiProvider").toString()); });
    engine.rootContext()->setContextProperty("updater", &updater);
    // Whether arguments can be trusted to come from the person using Tether:
    // not on a phone, where any app can start it with arguments of its own.
#if defined(Q_OS_ANDROID) && !defined(TETHER_TEST_HOOKS)
    engine.rootContext()->setContextProperty("argumentsTrusted", false);
#else
    engine.rootContext()->setContextProperty("argumentsTrusted", true);
#endif
    engine.load(QUrl("qrc:/Main.qml"));
    if (selfTest)
        return finishSelfTest(engine, app.arguments());
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
