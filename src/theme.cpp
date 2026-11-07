// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding, which themes and personal
    // overrides can both set, so ask hyprctl rather than reading its config.
    // Without Hyprland the corners stay sharp, like Omarchy's default.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                  ? QStandardPaths::findExecutable("hyprctl") : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    m_roundingPoll.setInterval(1000);
    connect(&m_roundingPoll, &QTimer::timeout, this, &Theme::refreshRounding);
    if (!m_hyprctl.isEmpty())
        m_roundingPoll.start();
    reload();
}

Theme::~Theme() {
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty() || m_roundingProcess.state() != QProcess::NotRunning)
        return;
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

QString Theme::readAccent(const QString &path) {
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QRegularExpression expression(R"re(^\s*accent\s*=\s*["'](#[0-9a-fA-F]{6})["'])re",
                                            QRegularExpression::MultilineOption);
        const auto match = expression.match(QString::fromUtf8(file.readAll()));
        if (match.hasMatch())
            return match.captured(1);
    }
    return "#FFD60A";
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QString Theme::accentForeground() const {
    const QColor c(m_accent);
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance = .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    return luminance > .179 ? "black" : "white";
}

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change. Watching the nearest existing ancestor means
    // installing Omarchy later works without restarting the app.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml"};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    const QString colorsFile = m_directory + "/theme/colors.toml";
    const auto accent = readAccent(colorsFile);
    const auto colors = readColors(colorsFile);
    if (accent != m_accent || colors != m_colors) {
        m_accent = accent;
        m_colors = colors;
        ++m_revision;
        emit changed();
    }
}

// Every key = "value" pair in colors.toml, so the terminal can use the same
// sixteen colors Omarchy gives Alacritty, Ghostty, and Kitty.
QHash<QString, QString> Theme::readColors(const QString &path) {
    QHash<QString, QString> colors;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return colors;
    const QRegularExpression line(R"re(^\s*([a-z_]+)\s*=\s*["'](#[0-9a-fA-F]{6})["'])re",
                                  QRegularExpression::MultilineOption);
    auto it = line.globalMatch(QString::fromUtf8(file.readAll()));
    while (it.hasNext()) {
        const auto match = it.next();
        colors.insert(match.captured(1), match.captured(2));
    }
    return colors;
}

namespace {

struct Scheme {
    const char *name;
    const char *background;
    const char *foreground;
    const char *palette[16];
};

// Tether's own schemes. The well-known ones use their published colors.
const Scheme schemes[] = {
    {"Tether Dark", "#1e1e20", "#e5e5ea",
     {"#1c1c1e", "#ff453a", "#32d74b", "#ffd60a", "#0a84ff", "#bf5af2", "#64d2ff", "#e5e5ea",
      "#636366", "#ff6961", "#4ee06a", "#ffe066", "#409cff", "#da8fff", "#70d7ff", "#ffffff"}},
    {"Tether Light", "#ffffff", "#1d1d1f",
     {"#1d1d1f", "#d70015", "#248a3d", "#a05a00", "#0040dd", "#8944ab", "#0071a4", "#8e8e93",
      "#636366", "#ff3b30", "#34c759", "#b25000", "#007aff", "#af52de", "#32ade6", "#d1d1d6"}},
    {"Solarized Dark", "#002b36", "#839496",
     {"#073642", "#dc322f", "#859900", "#b58900", "#268bd2", "#d33682", "#2aa198", "#eee8d5",
      "#002b36", "#cb4b16", "#586e75", "#657b83", "#839496", "#6c71c4", "#93a1a1", "#fdf6e3"}},
    {"Solarized Light", "#fdf6e3", "#657b83",
     {"#073642", "#dc322f", "#859900", "#b58900", "#268bd2", "#d33682", "#2aa198", "#eee8d5",
      "#002b36", "#cb4b16", "#586e75", "#657b83", "#839496", "#6c71c4", "#93a1a1", "#fdf6e3"}},
    {"Gruvbox Dark", "#282828", "#ebdbb2",
     {"#282828", "#cc241d", "#98971a", "#d79921", "#458588", "#b16286", "#689d6a", "#a89984",
      "#928374", "#fb4934", "#b8bb26", "#fabd2f", "#83a598", "#d3869b", "#8ec07c", "#ebdbb2"}},
    {"Nord", "#2e3440", "#d8dee9",
     {"#3b4252", "#bf616a", "#a3be8c", "#ebcb8b", "#81a1c1", "#b48ead", "#88c0d0", "#e5e9f0",
      "#4c566a", "#bf616a", "#a3be8c", "#ebcb8b", "#81a1c1", "#b48ead", "#8fbcbb", "#eceff4"}},
    {"One Dark", "#282c34", "#abb2bf",
     {"#282c34", "#e06c75", "#98c379", "#e5c07b", "#61afef", "#c678dd", "#56b6c2", "#abb2bf",
      "#5c6370", "#e06c75", "#98c379", "#e5c07b", "#61afef", "#c678dd", "#56b6c2", "#ffffff"}},
    {"Tango", "#1e1e1e", "#d3d7cf",
     {"#2e3436", "#cc0000", "#4e9a06", "#c4a000", "#3465a4", "#75507b", "#06989a", "#d3d7cf",
      "#555753", "#ef2929", "#8ae234", "#fce94f", "#729fcf", "#ad7fa8", "#34e2e2", "#eeeeec"}},
    {"Classic", "#000000", "#e5e5e5",
     {"#000000", "#cd3131", "#0dbc79", "#e5e510", "#2472c8", "#bc3fbc", "#11a8cd", "#e5e5e5",
      "#666666", "#f14c4c", "#23d18b", "#f5f543", "#3b8eea", "#d670d6", "#29b8db", "#ffffff"}},
};

QVariantMap schemeMap(const Scheme &scheme) {
    QVariantList palette;
    for (const char *c : scheme.palette)
        palette << QString(c);
    return {{"background", scheme.background}, {"foreground", scheme.foreground},
            {"cursor", scheme.foreground}, {"palette", palette}};
}

QVariantMap withPreview(QVariantMap colors) {
    const QVariantList palette = colors.value("palette").toList();
    colors.insert("preview", palette.mid(1, 6));
    return colors;
}

} // namespace

QStringList Theme::builtinSchemes() const {
    QStringList names;
    for (const Scheme &scheme : schemes)
        names << scheme.name;
    return names;
}

QVariantMap Theme::omarchyColors(const QHash<QString, QString> &colors) {
    static const QHash<QString, QString> defaults{
        {"background", "#121212"}, {"foreground", "#e6e6e6"}, {"bright_foreground", "#ffffff"},
        {"muted", "#7a7a7a"}, {"red", "#e06c75"}, {"green", "#98c379"}, {"yellow", "#e5c07b"},
        {"blue", "#61afef"}, {"magenta", "#c678dd"}, {"cyan", "#56b6c2"}};
    auto color = [&](const QString &key) {
        if (colors.contains(key))
            return colors.value(key);
        // Bright colors fall back to their normal ones, as Omarchy's themes do.
        const QString base = key.startsWith("bright_") ? key.mid(7) : key;
        return colors.value(base, defaults.value(key, defaults.value(base)));
    };
    const QStringList normal{"background", "red", "green", "yellow", "blue", "magenta", "cyan", "foreground"};
    const QStringList bright{"muted", "bright_red", "bright_green", "bright_yellow",
                             "bright_blue", "bright_magenta", "bright_cyan", "bright_foreground"};
    QVariantList palette;
    for (const QString &key : normal + bright)
        palette << color(key);
    return {{"background", color("background")}, {"foreground", color("foreground")},
            {"cursor", colors.value("cursor", color("foreground"))}, {"palette", palette}};
}

QString Theme::terminalBackground() const {
    return schemeColors("omarchy").value("background").toString();
}

QVariantMap Theme::schemeColors(const QString &name) const {
    if (name == "omarchy")
        return withPreview(m_colors.isEmpty() ? schemeMap(schemes[0]) : omarchyColors(m_colors));
    for (const Scheme &scheme : schemes)
        if (name == scheme.name)
            return withPreview(schemeMap(scheme));
    return {};
}

QStringList Theme::fontFamilies(bool monospace) {
    QStringList families;
    for (const QString &family : QFontDatabase::families()) {
        if (QFontDatabase::isPrivateFamily(family) || (monospace && !QFontDatabase::isFixedPitch(family)))
            continue;
        families << family;
    }
    return families;
}

