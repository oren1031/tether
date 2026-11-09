// Runs the tst_*.qml files beside this one against Tether's real interface
// files and backend, with a throwaway home directory so nothing of the
// person running them is read or changed.
//
//   TETHER_TOUCH=1            the phone and tablet interface (bin/test runs both)
//   TETHER_UI_LANGUAGE=he     start in a language, to see right-to-left layout
//   TETHER_UI_CATALOGS=<dir>  read translations from a folder instead of the built-in ones
//   TETHER_SHOTS=<directory>  tst_shots.qml saves screenshots there

#include <QtQuickTest>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>

#include "assistant.h"
#include "backend.h"
#include "iconprovider.h"
#include "serialports.h"
#include "sftp.h"
#include "terminalview.h"
#include "theme.h"
#include "translator.h"
#include "updater.h"

// What the tests can ask of the program around them.
class Harness : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool shots READ shots CONSTANT)
    Q_PROPERTY(QString language READ language CONSTANT)

public:
    using QObject::QObject;
    bool shots() const { return !qEnvironmentVariableIsEmpty("TETHER_SHOTS"); }
    QString language() const { return qEnvironmentVariable("TETHER_UI_LANGUAGE", "en"); }

    // The whole window, sheets and menus included, as a PNG in TETHER_SHOTS.
    Q_INVOKABLE bool grab(QQuickWindow *window, const QString &name) const {
        if (!window || !shots())
            return false;
        QDir().mkpath(qEnvironmentVariable("TETHER_SHOTS"));
        return window->grabWindow().save(qEnvironmentVariable("TETHER_SHOTS") + "/" + name + ".png");
    }
    // The color of one pixel of the window, as "#rrggbb".
    Q_INVOKABLE QString pixel(QQuickWindow *window, int x, int y) const {
        const QImage image = window ? window->grabWindow() : QImage();
        const qreal ratio = window ? window->effectiveDevicePixelRatio() : 1;
        return image.valid(int(x * ratio), int(y * ratio)) ? image.pixelColor(int(x * ratio), int(y * ratio)).name() : QString();
    }
};

class Setup : public QObject {
    Q_OBJECT

public:
    explicit Setup(const QString &home) : m_home(home) {}

public slots:
    void applicationAvailable() {
        QCoreApplication::setApplicationName("tether");
        QQuickStyle::setStyle("Basic");
        qmlRegisterSingletonType(QUrl("qrc:/Style.qml"), "Tether", 1, 0, "Style");
        qmlRegisterType<SftpBrowser>("Tether", 1, 0, "SftpBrowser");
        qmlRegisterType<TerminalView>("Tether", 1, 0, "TerminalView");

        const QString languageFile = m_home + "/language";
        QFile file(languageFile);
        if (file.open(QIODevice::WriteOnly))
            file.write(qEnvironmentVariable("TETHER_UI_LANGUAGE", "en").toUtf8());
        file.close();
        // TETHER_UI_CATALOGS: translations from a folder, for one not built in yet.
        m_translator = new Translator(languageFile, qEnvironmentVariable("TETHER_UI_CATALOGS", ":/i18n"), this);
        QCoreApplication::installTranslator(m_translator);
        m_theme = new Theme(m_home + "/no-theme", this);
        m_backend = new Backend(m_home + "/tether.json", m_home + "/ssh_config", m_home + "/logs", this);
        m_serialPorts = new SerialPorts(m_home + "/no-devices", 60000, this);
        m_assistant = new Assistant(m_home + "/anthropic-key", this);
        // No repository: nothing here goes looking for updates.
        m_updater = new Updater(QString(), "0.0.0", this);
        m_harness = new Harness(this);
    }

    // Once for each test file.
    void qmlEngineAvailable(QQmlEngine *engine) {
        engine->addImageProvider("icon", new IconProvider);
        QQmlContext *context = engine->rootContext();
        context->setContextProperty("translator", m_translator);
        context->setContextProperty("theme", m_theme);
        context->setContextProperty("backend", m_backend);
        context->setContextProperty("serialPorts", m_serialPorts);
        context->setContextProperty("assistant", m_assistant);
        context->setContextProperty("updater", m_updater);
        context->setContextProperty("harness", m_harness);
    }

private:
    QString m_home;
    Translator *m_translator = nullptr;
    Theme *m_theme = nullptr;
    Backend *m_backend = nullptr;
    SerialPorts *m_serialPorts = nullptr;
    Assistant *m_assistant = nullptr;
    Updater *m_updater = nullptr;
    Harness *m_harness = nullptr;
};

int main(int argc, char **argv) {
    QTemporaryDir home;
    qputenv("HOME", home.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (home.path() + "/.config").toUtf8());
    qputenv("XDG_DATA_HOME", (home.path() + "/.local/share").toUtf8());
    qunsetenv("ANTHROPIC_API_KEY");
    QTEST_SET_MAIN_SOURCE_PATH
    Setup setup(home.path());
    return quick_test_main_with_setup(argc, argv, "tether_ui", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "main.moc"
