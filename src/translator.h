#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonValue>
#include <QLocale>
#include <QTimer>
#include <QTranslator>
#include <QVariant>

// Follows the language picked in Settings; left on "System" that is Omarchy's
// Setup > Language where there is one, else the system's own. Catalogs are
// flat JSON objects from English source text to translation, one per Omarchy
// language code (i18n/da.json, i18n/pt-BR.json), so QML keeps plain
// qsTr("Save") and C++ keeps tr("Save"). A counted string,
// qsTr("%n clips", "", count), maps to an object of plural forms keyed by
// CLDR category ({"one": "%n klip", "other": "%n klip"}).
class Translator : public QTranslator {
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY changed)
    // The language chosen in Settings: a code from languages, or empty to
    // follow the system.
    Q_PROPERTY(QString preference READ preference WRITE setPreference NOTIFY changed)
    // The languages Tether ships, for the picker: code and its own name.
    Q_PROPERTY(QVariantList languages READ languages CONSTANT)

public:
    explicit Translator(const QString &languageFile = {}, const QString &catalogDirectory = ":/i18n",
                        QObject *parent = nullptr);

    QString language() const { return m_language; }
    QString preference() const { return m_preference; }
    void setPreference(const QString &code);
    QVariantList languages() const;
    bool isEmpty() const override { return m_strings.isEmpty(); }
    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr,
                      int n = -1) const override;

    static QString readLanguage(const QString &path);
    // The first of the system's languages ("he-IL", "en-US") that has a
    // catalog, by its full code or its base; English when none has.
    static QString systemLanguage(const QStringList &uiLanguages, const QStringList &available);
    static QString pluralCategory(const QString &language, long long count);
    static QLocale formatLocale(const QString &language);

signals:
    void changed();

private:
    void reload();
    QStringList available() const;

    QString m_languageFile;
    QString m_catalogDirectory;
    QString m_language;
    QString m_preference;
    QHash<QString, QJsonValue> m_strings;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
