#include "updater.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>

#include <cstdio>

Updater::Updater(const QString &repository, const QString &currentVersion, QObject *parent)
    : QObject(parent), m_repository(repository), m_current(currentVersion) {
    m_timer.setInterval(6 * 60 * 60 * 1000);
    connect(&m_timer, &QTimer::timeout, this, [this] { check(true); });
}

QString Updater::installKind() const {
#if defined(Q_OS_WIN)
    return "windows";
#elif defined(Q_OS_ANDROID)
    return "android";
#else
    // An AppImage replaces its own file; the Arch package goes through pacman.
    if (qEnvironmentVariableIsSet("APPIMAGE"))
        return "appimage";
    if (QFileInfo::exists("/usr/bin/pacman") && QFileInfo::exists("/var/lib/pacman/local"))
        return "pacman";
    return "page";
#endif
}

int Updater::compareVersions(const QString &a, const QString &b) {
    auto parts = [](QString v) {
        v = v.trimmed();
        if (v.startsWith('v') || v.startsWith('V'))
            v = v.mid(1);
        QList<int> out;
        for (const QString &p : v.section('-', 0, 0).split('.'))
            out << p.toInt();
        while (out.size() < 3)
            out << 0;
        return out;
    };
    const QList<int> x = parts(a), y = parts(b);
    for (int i = 0; i < qMax(x.size(), y.size()); ++i) {
        const int l = x.value(i), r = y.value(i);
        if (l != r)
            return l < r ? -1 : 1;
    }
    return 0;
}

QVariantMap Updater::pickAsset(const QVariantList &assets, const QString &kind) {
    for (const QVariant &value : assets) {
        const QVariantMap asset = value.toMap();
        const QString name = asset.value("name").toString();
        const bool match = kind == "pacman" ? name.endsWith("-x86_64.pkg.tar.zst") && !name.contains("-debug-")
                         : kind == "windows" ? name.endsWith("-setup.exe")
                         : kind == "appimage" ? name.endsWith("-x86_64.AppImage")
                         : kind == "android" ? name.endsWith("-android-arm64.apk")
                         : false;
        if (match)
            return asset;
    }
    return {};
}

void Updater::startAutomaticChecks(bool enabled) {
    if (!configured() || !enabled) {
        m_timer.stop();
        return;
    }
    // A little after launch, then every few hours.
    QTimer::singleShot(8000, this, [this] { check(true); });
    m_timer.start();
}

void Updater::setState(const QString &state, const QString &message) {
    m_state = state;
    m_message = message;
    emit changed();
}

void Updater::check(bool quiet) {
    if (!configured() || m_reply || m_state == "downloading")
        return;
    QNetworkRequest request(QUrl("https://api.github.com/repos/" + m_repository + "/releases/latest"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setHeader(QNetworkRequest::UserAgentHeader, "Tether/" + m_current);
    request.setTransferTimeout(20000);
    if (!quiet)
        setState("checking", tr("Checking for updates…"));
    m_reply = m_network.get(request);
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, quiet] {
        reply->deleteLater();
        m_reply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) {
            const QString why = status == 404 ? tr("No releases have been published yet.")
                              : status == 0 ? tr("Couldn't reach GitHub: %1").arg(reply->errorString())
                              : tr("GitHub answered with HTTP %1.").arg(status);
            setState(quiet ? "idle" : "error", quiet ? QString() : why);
            return;
        }
        const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
        m_latest = release.value("tag_name").toString().remove(QRegularExpression("^[vV]"));
        m_notes = release.value("body").toString().trimmed();
        m_releaseUrl = release.value("html_url").toString();
        m_asset = pickAsset(release.value("assets").toArray().toVariantList(), installKind());
        m_downloadUrl = m_asset.value("browser_download_url").toString();
        if (available())
            setState("idle", tr("Tether %1 is available.").arg(m_latest));
        else
            setState("idle", quiet ? QString() : tr("Tether %1 is the newest version.").arg(m_current));
    });
}

void Updater::download() {
    if (!available() || m_reply)
        return;
    if (m_asset.isEmpty()) {
        setState("error", tr("This release has no download for this system; get it from the release page."));
        return;
    }
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/updates";
    QDir().mkpath(directory);
    const QString name = QFileInfo(m_asset.value("name").toString()).fileName();
    const QString target = directory + "/" + name;
    QNetworkRequest request(QUrl(m_asset.value("browser_download_url").toString()));
    request.setHeader(QNetworkRequest::UserAgentHeader, "Tether/" + m_current);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    m_progress = 0;
    setState("downloading", tr("Downloading Tether %1…").arg(m_latest));
    m_reply = m_network.get(request);
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        m_progress = total > 0 ? double(got) / total : 0;
        emit changed();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, target] {
        reply->deleteLater();
        m_reply = nullptr;
        const QByteArray data = reply->readAll();
        const qint64 expected = m_asset.value("size").toLongLong();
        if (reply->error() != QNetworkReply::NoError || (expected > 0 && data.size() != expected)) {
            setState("error", tr("The download failed: %1").arg(reply->errorString()));
            return;
        }
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            setState("error", tr("Couldn't save the update: %1").arg(file.errorString()));
            return;
        }
        m_file = target;
        m_progress = 1;
        setState("ready", tr("Tether %1 is downloaded and ready to install.").arg(m_latest));
    });
}

QVariantMap Updater::installCommand() const {
    if (m_file.isEmpty())
        return {};
    if (installKind() == "pacman")
        return {{"program", "sudo"}, {"args", QStringList{"pacman", "-U", "--needed", m_file}}};
    if (installKind() == "windows")
        return {{"program", m_file}, {"args", QStringList{"/SILENT", "/RESTARTAPPLICATIONS"}}};
    return {};
}

void Updater::restart() {
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
    QCoreApplication::quit();
}

bool Updater::launchInstaller() {
    const QVariantMap command = installCommand();
    if (command.isEmpty() || installKind() != "windows")
        return false;
    if (!QProcess::startDetached(command["program"].toString(), command["args"].toStringList()))
        return false;
    QCoreApplication::quit();
    return true;
}

bool Updater::replaceAppImage() {
    const QString current = qEnvironmentVariable("APPIMAGE");
    if (installKind() != "appimage" || m_file.isEmpty() || current.isEmpty())
        return false;
    // Copy beside the old file, then rename over it: the running copy keeps
    // its open file, and a failure leaves the old one in place.
    const QString staged = current + ".new";
    QFile::remove(staged);
    if (!QFile::copy(m_file, staged)) {
        setState("error", tr("Couldn't write next to %1.").arg(current));
        return false;
    }
    QFile::setPermissions(staged, QFile::permissions(current) | QFile::ExeOwner | QFile::ReadOwner);
    if (::rename(QFile::encodeName(staged).constData(), QFile::encodeName(current).constData()) != 0) {
        QFile::remove(staged);
        setState("error", tr("Couldn't replace %1.").arg(current));
        return false;
    }
    QProcess::startDetached(current, {});
    QCoreApplication::quit();
    return true;
}
