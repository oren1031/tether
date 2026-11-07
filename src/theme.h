#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariant>

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(QString terminalBackground READ terminalBackground NOTIFY changed)
    // Whether an Omarchy theme was found, so its colors can be offered.
    Q_PROPERTY(bool hasOmarchy READ hasOmarchy NOTIFY changed)
    // Bumped on every theme change, for bindings that call schemeColors().
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;

    QString accent() const { return m_accent; }
    QString accentForeground() const;
    int radius() const { return m_radius; }
    QString terminalBackground() const;
    bool hasOmarchy() const { return !m_colors.isEmpty(); }
    int revision() const { return m_revision; }

    // Tether's terminal color schemes by name, and the colors of one
    // ("omarchy" is the current Omarchy theme's, or Tether Dark without
    // one): background, foreground, cursor, palette (the 16 ANSI colors),
    // and preview (red, green, yellow, blue, magenta, cyan).
    Q_INVOKABLE QStringList builtinSchemes() const;
    Q_INVOKABLE QVariantMap schemeColors(const QString &name) const;
    // The 16 ANSI colors from an Omarchy colors.toml, mapped as Omarchy's
    // Alacritty template does: black is the background, white the
    // foreground, bright black the muted color.
    static QVariantMap omarchyColors(const QHash<QString, QString> &colors);

    // Installed font families, or only those with fixed-width letters.
    Q_INVOKABLE static QStringList fontFamilies(bool monospace);

    static QString readAccent(const QString &path);
    static QHash<QString, QString> readColors(const QString &path);

signals:
    void changed();

private:
    void reload();
    void refreshRounding();

    QString m_directory;
    QString m_accent = "#FFD60A";
    QHash<QString, QString> m_colors;
    int m_revision = 0;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
};
