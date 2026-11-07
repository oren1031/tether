#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariant>
#include <functional>

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(QString terminalScheme READ terminalScheme NOTIFY changed)
    Q_PROPERTY(QString terminalBackground READ terminalBackground NOTIFY changed)

public:
    using SchemeLoader = std::function<bool(const QString &path)>;

    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;

    QString accent() const { return m_accent; }
    QString accentForeground() const;
    int radius() const { return m_radius; }
    QString terminalScheme() const { return m_scheme; }
    QString terminalBackground() const { return m_background; }

    // Terminal colors come from the theme as a Konsole color scheme written to
    // schemeDirectory and handed to the terminal widget through loader.
    void setSchemeLoader(const SchemeLoader &loader, const QString &schemeDirectory);

    // Color schemes the terminal widget ships, by name, and the colors of one
    // ("omarchy" is the theme's) as background, foreground, and palette: red,
    // green, yellow, blue, magenta, and cyan, for previews.
    Q_INVOKABLE QStringList builtinSchemes() const;
    Q_INVOKABLE QVariantMap schemeColors(const QString &name) const;
    void setBuiltinSchemeDirectory(const QString &directory) { m_builtinDirectory = directory; }

    // Installed font families, or only those with fixed-width letters.
    Q_INVOKABLE static QStringList fontFamilies(bool monospace);

    static QString readAccent(const QString &path);
    static QHash<QString, QString> readColors(const QString &path);
    static QString schemeText(const QHash<QString, QString> &colors);

    // A scheme the terminal widget ships, used when the theme's can't be loaded.
    static constexpr const char *FallbackScheme = "Linux";

signals:
    void changed();

private:
    void reload();
    void refreshRounding();
    void reloadScheme(const QHash<QString, QString> &colors);

    QString m_directory;
    QString m_accent = "#FFD60A";
    QString m_scheme = FallbackScheme;
    QString m_background = "#121212";
    QString m_schemeDirectory;
    QString m_builtinDirectory;
    QHash<QString, QString> m_colors;
    SchemeLoader m_schemeLoader;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
};
