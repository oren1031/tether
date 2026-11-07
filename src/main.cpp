// tether: SSH hosts and serial ports in tabs and split panes.

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml/qqml.h>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QUrl>

#include <dlfcn.h>

#include "assistant.h"
#include "backend.h"
#include "sftp.h"
#include "updater.h"

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif
#ifndef TETHER_REPO
#define TETHER_REPO ""
#endif
#include "serialports.h"
#include "theme.h"
#include "translator.h"

// The terminal widget only looks for color schemes in its own directory, but
// its ColorSchemeManager can load one from any path. That isn't part of the
// QML API, so resolve it from the plugin the QML engine loads anyway; dlopen
// hands both of us the same library, and so the same manager.
static Theme::SchemeLoader terminalSchemeLoader() {
    const QString plugin = QLibraryInfo::path(QLibraryInfo::QmlImportsPath) + "/QMLTermWidget/libqmltermwidget.so";
    void *library = dlopen(QFile::encodeName(plugin).constData(), RTLD_NOW | RTLD_GLOBAL);
    if (!library)
        return {};
    using Instance = void *(*)();
    using Load = bool (*)(void *, const QString &);
    const auto instance = reinterpret_cast<Instance>(dlsym(library, "_ZN7Konsole18ColorSchemeManager8instanceEv"));
    const auto load = reinterpret_cast<Load>(
        dlsym(library, "_ZN7Konsole18ColorSchemeManager21loadCustomColorSchemeERK7QString"));
    if (!instance || !load)
        return {};
    return [instance, load](const QString &path) { return load(instance(), path); };
}

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
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

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    if (const auto loader = terminalSchemeLoader())
        theme.setSchemeLoader(loader, QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                                          + "/tether/color-schemes");
    Backend backend;
    SerialPorts serialPorts;
    Assistant assistant;
    Updater updater(QString(TETHER_REPO).trimmed(), APP_VERSION);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("serialPorts", &serialPorts);
    engine.rootContext()->setContextProperty("assistant", &assistant);
    engine.rootContext()->setContextProperty("updater", &updater);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
