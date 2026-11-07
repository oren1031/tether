// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLibraryInfo>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
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
    const QString scheme = m_scheme, background = m_background;
    const bool accentChanged = accent != m_accent;
    m_accent = accent;
    reloadScheme(readColors(colorsFile));
    if (accentChanged || scheme != m_scheme || background != m_background)
        emit changed();
}

void Theme::setSchemeLoader(const SchemeLoader &loader, const QString &schemeDirectory) {
    m_schemeLoader = loader;
    m_schemeDirectory = schemeDirectory;
    reload();
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

// The mapping follows Omarchy's alacritty.toml template: black is the
// background, white the foreground, bright black the muted color.
QString Theme::schemeText(const QHash<QString, QString> &colors) {
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
    auto rgb = [](const QString &hex) {
        const QColor c(hex);
        return QString("%1,%2,%3").arg(c.red()).arg(c.green()).arg(c.blue());
    };

    QString text = "[General]\nDescription=Omarchy\nOpacity=1\n\n";
    auto section = [&](const QString &name, const QString &hex) {
        text += QString("[%1]\nColor=%2\n\n").arg(name, rgb(hex));
    };
    section("Background", color("background"));
    section("BackgroundIntense", color("background"));
    section("Foreground", color("foreground"));
    section("ForegroundIntense", color("bright_foreground"));
    const QStringList normal{"background", "red", "green", "yellow", "blue", "magenta", "cyan", "foreground"};
    const QStringList bright{"muted", "bright_red", "bright_green", "bright_yellow",
                             "bright_blue", "bright_magenta", "bright_cyan", "bright_foreground"};
    for (int i = 0; i < 8; ++i) {
        section(QString("Color%1").arg(i), color(normal[i]));
        section(QString("Color%1Intense").arg(i), color(bright[i]));
    }
    return text;
}

QStringList Theme::builtinSchemes() const {
    const QString directory = !m_builtinDirectory.isEmpty() ? m_builtinDirectory
        : QLibraryInfo::path(QLibraryInfo::QmlImportsPath) + "/QMLTermWidget/color-schemes";
    QStringList names;
    for (const QFileInfo &file : QDir(directory).entryInfoList({"*.colorscheme"}, QDir::Files, QDir::Name | QDir::IgnoreCase))
        names << file.completeBaseName();
    return names;
}

QVariantMap Theme::schemeColors(const QString &name) const {
    static const QStringList roles{"red", "green", "yellow", "blue", "magenta", "cyan"};
    QVariantMap result;
    QVariantList palette;
    if (name == "omarchy") {
        result = {{"background", m_colors.value("background", m_background)},
                  {"foreground", m_colors.value("foreground", "#e6e6e6")}};
        for (const auto &role : roles)
            palette << m_colors.value(role, "#808080");
    } else {
        const QString directory = !m_builtinDirectory.isEmpty() ? m_builtinDirectory
            : QLibraryInfo::path(QLibraryInfo::QmlImportsPath) + "/QMLTermWidget/color-schemes";
        const QString path = directory + "/" + name + ".colorscheme";
        if (!QFileInfo::exists(path))
            return {};
        // Konsole schemes are INI files with "Color=r,g,b" in each section,
        // which QSettings reads as a list.
        QSettings scheme(path, QSettings::IniFormat);
        auto color = [&](const QString &section) {
            const QStringList rgb = scheme.value(section + "/Color").toStringList();
            return rgb.size() == 3 ? QColor(rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt()).name() : QString("#000000");
        };
        result = {{"background", color("Background")}, {"foreground", color("Foreground")}};
        for (int i = 1; i <= 6; ++i)
            palette << color(QString("Color%1").arg(i));
    }
    result.insert("palette", palette);
    return result;
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

void Theme::reloadScheme(const QHash<QString, QString> &colors) {
    m_colors = colors;
    m_background = colors.value("background", "#121212");
    if (!m_schemeLoader) {
        m_scheme = FallbackScheme;
        return;
    }

    // The widget keeps every scheme it has loaded under its file name, so a
    // changed theme needs a new name; hashing the contents gives one.
    const QString text = schemeText(colors);
    const QString name = "omarchy-" + QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex().left(10));
    const QString path = m_schemeDirectory + "/" + name + ".colorscheme";
    if (!QFile::exists(path)) {
        QDir().mkpath(m_schemeDirectory);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(text.toUtf8()) < 0 || !file.commit()) {
            m_scheme = FallbackScheme;
            return;
        }
    }
    m_scheme = m_schemeLoader(path) ? name : QString(FallbackScheme);
}
