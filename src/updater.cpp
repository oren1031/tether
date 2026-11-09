#include "updater.h"

#include <QCoreApplication>
#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif
#include <QUrl>
#include <QCryptographicHash>
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

// A release as Tether's own manifest (latest.json) or GitHub's API gives it:
// version, notes, page, and the asset for this system with every place it can
// be downloaded from and its checksum.
bool Updater::readRelease(const QJsonObject &release, const QUrl &base) {
    const bool manifest = release.contains("version");
    m_latest = (manifest ? release.value("version") : release.value("tag_name")).toString().remove(QRegularExpression("^[vV]"));
    m_notes = (manifest ? release.value("notes") : release.value("body")).toString().trimmed();
    m_releaseUrl = (manifest ? release.value("page") : release.value("html_url")).toString();
    m_asset = pickAsset(release.value("assets").toArray().toVariantList(), installKind());
    m_sources.clear();
    if (!m_asset.isEmpty()) {
        if (manifest) {
            m_sources << base.resolved(QUrl(m_asset.value("url").toString()));
            m_sha256 = m_asset.value("sha256").toString();
        } else {
            m_sha256 = m_asset.value("digest").toString().remove("sha256:");
        }
        // GitHub's API serves the file even when its download links don't.
        const QString api = m_asset.value("url").toString();
        if (!manifest && api.startsWith("https://api.github.com/"))
            m_sources << QUrl(api);
        const QString direct = m_asset.value("browser_download_url").toString();
        if (!direct.isEmpty())
            m_sources << QUrl(direct);
    }
    m_downloadUrl = m_sources.isEmpty() ? QString() : m_sources.first().toString();
    return !m_latest.isEmpty();
}

void Updater::check(bool quiet) {
    if (!configured() || m_reply || m_state == "downloading")
        return;
    if (!quiet)
        setState("checking", tr("Checking for updates…"));
    // Your own server first, when there is one; GitHub otherwise.
    if (!m_mirror.isEmpty())
        checkAt(QUrl(m_mirror + "/latest.json"), quiet, true);
    else
        checkAt(QUrl("https://api.github.com/repos/" + m_repository + "/releases/latest"), quiet, false);
}

void Updater::checkAt(const QUrl &url, bool quiet, bool mirror) {
    QNetworkRequest request(url);
    request.setRawHeader("Accept", mirror ? "application/json" : "application/vnd.github+json");
    request.setHeader(QNetworkRequest::UserAgentHeader, "Tether/" + m_current);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(mirror ? 10000 : 20000);
    m_reply = m_network.get(request);
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, quiet, mirror, url] {
        reply->deleteLater();
        m_reply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
        if (status != 200 || !readRelease(release, url)) {
            if (mirror && !m_repository.isEmpty()) {
                checkAt(QUrl("https://api.github.com/repos/" + m_repository + "/releases/latest"), quiet, false);
                return;
            }
            const QString why = status == 404 ? tr("No releases have been published yet.")
                              : status == 0 ? tr("Couldn't reach the update server: %1").arg(reply->errorString())
                              : tr("The update server answered with HTTP %1.").arg(status);
            setState(quiet ? "idle" : "error", quiet ? QString() : why);
            return;
        }
        m_source = mirror ? url.host() : QString("GitHub");
        if (available())
            setState("idle", tr("Tether %1 is available.").arg(m_latest));
        else
            setState("idle", quiet ? QString() : tr("Tether %1 is the newest version.").arg(m_current));
    });
}

void Updater::download() {
    if (!available() || m_reply)
        return;
    if (m_sources.isEmpty()) {
        setState("error", tr("This release has no download for this system; get it from the release page."));
        return;
    }
    m_progress = 0;
    setState("downloading", tr("Downloading Tether %1…").arg(m_latest));
    downloadFrom(0);
}

// Tries each place the file is offered until one delivers it whole, with
// the right checksum.
void Updater::downloadFrom(int index) {
    if (index >= m_sources.size()) {
        // Your server's copy didn't work out: ask GitHub for the same release.
        if (m_source != "GitHub" && !m_repository.isEmpty()) {
            const QString wanted = m_latest;
            QNetworkRequest request(QUrl("https://api.github.com/repos/" + m_repository + "/releases/latest"));
            request.setRawHeader("Accept", "application/vnd.github+json");
            request.setHeader(QNetworkRequest::UserAgentHeader, "Tether/" + m_current);
            request.setTransferTimeout(20000);
            m_reply = m_network.get(request);
            QNetworkReply *reply = m_reply;
            connect(reply, &QNetworkReply::finished, this, [this, reply, wanted] {
                reply->deleteLater();
                m_reply = nullptr;
                const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
                if (reply->error() == QNetworkReply::NoError && readRelease(release, QUrl()) && m_latest == wanted) {
                    m_source = "GitHub";
                    downloadFrom(0);
                } else {
                    setState("error", tr("The download failed from every source: %1").arg(m_lastDownloadError));
                }
            });
            return;
        }
        setState("error", tr("The download failed from every source: %1").arg(m_lastDownloadError));
        return;
    }
    const QString directory = updatesDirectory();
    QDir().mkpath(directory);
    const QString target = directory + "/" + QFileInfo(m_asset.value("name").toString()).fileName();
    const QUrl url = m_sources.at(index);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Tether/" + m_current);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (url.host() == "api.github.com")
        request.setRawHeader("Accept", "application/octet-stream");
    request.setTransferTimeout(60000);
    m_reply = m_network.get(request);
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        m_progress = total > 0 ? double(got) / total : 0;
        emit changed();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, target, index, url] {
        reply->deleteLater();
        m_reply = nullptr;
        const QByteArray data = reply->readAll();
        const qint64 expected = m_asset.value("size").toLongLong();
        QString problem;
        if (reply->error() != QNetworkReply::NoError)
            problem = reply->errorString();
        else if (expected > 0 && data.size() != expected)
            problem = tr("got %1 bytes instead of %2").arg(data.size()).arg(expected);
        else if (!m_sha256.isEmpty()
                 && QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() != m_sha256.toLatin1())
            problem = tr("the file's checksum doesn't match");
        if (!problem.isEmpty()) {
            m_lastDownloadError = url.host() + ": " + problem;
            downloadFrom(index + 1);
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

// Where updates download to. On Android that's the app's files folder, the
// one Qt's file provider shares with the system installer (not the cache).
QString Updater::updatesDirectory() {
#ifdef Q_OS_ANDROID
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/updates";
#else
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/updates";
#endif
}

// Android: the APK goes to the system installer through Qt's file provider,
// which asks "Do you want to update this app?" and installs it over this one.
bool Updater::openAndroidInstaller(const QString &apk) {
#ifdef Q_OS_ANDROID
    // An ACTION_VIEW intent for the APK through Qt's FileProvider (which
    // shares the files folder), with permission for the installer to read it.
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid())
        return false;
    const QString package = context.callObjectMethod("getPackageName", "()Ljava/lang/String;").toString();
    const QJniObject file("java/io/File", "(Ljava/lang/String;)V", QJniObject::fromString(apk).object<jstring>());
    const QJniObject uri = QJniObject::callStaticObjectMethod(
        "androidx/core/content/FileProvider", "getUriForFile",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/io/File;)Landroid/net/Uri;",
        context.object(), QJniObject::fromString(package + ".qtprovider").object<jstring>(), file.object());
    QJniEnvironment env;
    if (env.checkAndClearExceptions() || !uri.isValid())
        return false;
    QJniObject intent("android/content/Intent", "(Ljava/lang/String;)V",
                      QJniObject::fromString("android.intent.action.VIEW").object<jstring>());
    intent.callObjectMethod("setDataAndType", "(Landroid/net/Uri;Ljava/lang/String;)Landroid/content/Intent;",
                            uri.object(), QJniObject::fromString("application/vnd.android.package-archive").object<jstring>());
    const jint grantRead = 0x00000001, newTask = 0x10000000;
    intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", grantRead | newTask);
    context.callMethod<void>("startActivity", "(Landroid/content/Intent;)V", intent.object());
    return !env.checkAndClearExceptions();
#else
    Q_UNUSED(apk)
    return false;
#endif
}

bool Updater::launchInstaller() {
    if (installKind() == "android")
        return !m_file.isEmpty() && openAndroidInstaller(m_file);
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
