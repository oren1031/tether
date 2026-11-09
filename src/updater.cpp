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
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>

#include <cstdio>
#include <memory>

// The public half of the key releases are signed with (ed25519, the base64
// part of the OpenSSH public key). The private half is on no server: it signs
// latest.json when a release is published. A fork that publishes its own
// releases builds with its own key in TETHER_UPDATE_KEY. install.sh and
// bin/mirror-release carry the same key; bin/sign-release checks they agree.
#ifndef TETHER_UPDATE_KEY
#define TETHER_UPDATE_KEY "AAAAC3NzaC1lZDI1NTE5AAAAIGlNzf+gXq5FiE2CBcdgvlgMfKFqLNDo4wnYITdFhF2Q"
#endif

Updater::Updater(const QString &repository, const QString &currentVersion, QObject *parent)
    : QObject(parent), m_repository(repository), m_current(currentVersion) {
    m_timer.setInterval(6 * 60 * 60 * 1000);
    connect(&m_timer, &QTimer::timeout, this, [this] { check(true); });
}

QString Updater::installKind() const {
    if (!m_kind.isEmpty())
        return m_kind;
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

QString Updater::cleanVersion(const QString &version) {
    QString v = version.trimmed();
    if (v.startsWith('v') || v.startsWith('V'))
        v = v.mid(1);
    static const QRegularExpression valid("^[0-9]+(\\.[0-9]+){0,3}$");
    return valid.match(v).hasMatch() ? v : QString();
}

// Anything fetched or opened is https; plain http only reaches this computer
// itself, which is how the tests stand in for a server.
bool Updater::safeUrl(const QUrl &url) {
    if (!url.isValid() || url.host().isEmpty())
        return false;
    if (url.scheme() == "https")
        return true;
    const QString host = url.host();
    return url.scheme() == "http" && (host == "localhost" || host == "127.0.0.1" || host == "::1");
}

void Updater::setMirror(const QString &url) {
    QString clean = url.trimmed();
    while (clean.endsWith('/'))
        clean.chop(1);
    QString problem;
    if (!clean.isEmpty() && !safeUrl(QUrl(clean))) {
        // Over plain http anyone on the way could answer for the server.
        problem = tr("The update server must be an https:// address; GitHub is used instead.");
        clean.clear();
    }
    if (clean != m_mirror || problem != m_mirrorProblem) {
        m_mirror = clean;
        m_mirrorProblem = problem;
        emit changed();
    }
}

QUrl Updater::latestReleaseUrl() const {
    return QUrl(m_api + "repos/" + m_repository + "/releases/latest");
}

// Where GitHub offers a release's file: its API first (it serves the file
// even when the download links don't), then the download link.
QList<QUrl> Updater::githubLinks(const QJsonArray &assets, const QString &name) const {
    QList<QUrl> links;
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        if (asset.value("name").toString() != name)
            continue;
        const QString api = asset.value("url").toString();
        if (api.startsWith(m_api) && safeUrl(QUrl(api)))
            links << QUrl(api);
        const QUrl direct(asset.value("browser_download_url").toString());
        if (safeUrl(direct))
            links << direct;
        break;
    }
    return links;
}

bool Updater::adopt(const QJsonObject &manifest, const QString &expectedVersion, const QUrl &mirrorBase,
                    const QJsonArray &githubAssets, QString *problem) {
    const QString version = cleanVersion(manifest.value("version").toString());
    if (version.isEmpty()) {
        *problem = tr("The update's signed file list names no version.");
        return false;
    }
    // An old signed list can't be passed off as a new release, or as this one.
    if (!expectedVersion.isEmpty() && version != expectedVersion) {
        *problem = tr("The update's signed file list is for Tether %1, not %2.").arg(version, expectedVersion);
        return false;
    }
    if (compareVersions(version, m_current) <= 0) {
        *problem = tr("The update's signed file list is for Tether %1, which isn't newer than this one.").arg(version);
        return false;
    }
    const QVariantMap asset = pickAsset(manifest.value("assets").toArray().toVariantList(), installKind());
    QList<QUrl> sources;
    QString sha256;
    qint64 size = 0;
    if (!asset.isEmpty()) {
        const QString name = asset.value("name").toString();
        sha256 = asset.value("sha256").toString().toLower();
        size = asset.value("size").toLongLong();
        static const QRegularExpression hex("^[0-9a-f]{64}$");
        static const QRegularExpression plain("^[A-Za-z0-9][A-Za-z0-9._+-]*$");
        if (!plain.match(name).hasMatch()) {
            *problem = tr("The update's file list has a file name that can't be used.");
            return false;
        }
        if (!hex.match(sha256).hasMatch() || size <= 0) {
            *problem = tr("The update's file list has no checksum or size for %1, so it won't be installed.").arg(name);
            return false;
        }
        // On your server a release's files sit in a folder named for the version.
        if (safeUrl(mirrorBase))
            sources << mirrorBase.resolved(QUrl(version + "/" + name));
        sources << githubLinks(githubAssets, name);
    }
    m_latest = version;
    m_asset = asset;
    m_sha256 = sha256;
    m_size = size;
    m_sources = sources;
    m_downloadUrl = sources.isEmpty() ? QString() : sources.first().toString();
    const QString notes = manifest.value("notes").toString().trimmed();
    if (!notes.isEmpty())
        m_notes = notes;
    const QString page = manifest.value("page").toString();
    if (QUrl(page).scheme() == "https")
        m_releaseUrl = page;
    return true;
}

// One request at a time; done gets the HTTP status (0 when nothing usable
// came back) and at most limit bytes of the answer.
void Updater::fetch(const QUrl &url, const QByteArray &accept, int timeout, qint64 limit, const Fetched &done) {
    if (!safeUrl(url)) {
        done(0, {}, tr("%1 isn't an https address").arg(url.toString()));
        return;
    }
    QNetworkRequest request(url);
    request.setRawHeader("Accept", accept);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Tether/" + m_current);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(timeout);
    m_reply = m_network.get(request);
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, limit, done] {
        reply->deleteLater();
        m_reply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->read(limit + 1);
        if (body.size() > limit)
            done(0, {}, tr("the answer is too big"));
        else if (reply->error() != QNetworkReply::NoError && status == 200)
            done(0, {}, reply->errorString()); // cut off part-way
        else
            done(status, body, reply->errorString());
    });
}

void Updater::fetchFirst(const QList<QUrl> &urls, qint64 limit, const std::function<void(bool, const QByteArray &)> &done) {
    if (urls.isEmpty()) {
        done(false, {});
        return;
    }
    const QByteArray accept = urls.first().toString().startsWith(m_api) ? "application/octet-stream" : "*/*";
    fetch(urls.first(), accept, 20000, limit, [this, urls, limit, done](int status, const QByteArray &body, const QString &) {
        if (status == 200 && !body.isEmpty())
            done(true, body);
        else
            fetchFirst(urls.mid(1), limit, done);
    });
}

// ssh-keygen checks the signature against the built-in key: the list on its
// input, the signature and the one allowed signer in a private folder.
void Updater::verify(const QByteArray &manifest, const QByteArray &signature,
                     const std::function<void(Signature, const QString &)> &done) {
    QString program = m_keygen;
    if (!m_keygenSet) {
        program = QStandardPaths::findExecutable("ssh-keygen");
#ifdef Q_OS_WIN
        // Windows keeps its OpenSSH where not every program's search path reaches.
        if (program.isEmpty())
            program = QStandardPaths::findExecutable(
                "ssh-keygen", {qEnvironmentVariable("SystemRoot", "C:\\Windows") + "\\System32\\OpenSSH"});
#endif
    }
    auto folder = std::make_shared<QTemporaryDir>();
    if (program.isEmpty() || !folder->isValid()) {
        done(Signature::Unavailable, {});
        return;
    }
    const QString key = m_publicKey.isEmpty() ? QStringLiteral(TETHER_UPDATE_KEY) : m_publicKey;
    const QString signersPath = folder->filePath("allowed_signers"), sigPath = folder->filePath("latest.json.sig");
    QFile signers(signersPath), sig(sigPath);
    if (!signers.open(QIODevice::WriteOnly) || !sig.open(QIODevice::WriteOnly)) {
        done(Signature::Unavailable, {});
        return;
    }
    signers.write("tether-update namespaces=\"tether-update\" ssh-ed25519 " + key.toLatin1() + "\n");
    signers.close();
    sig.write(signature);
    sig.close();

    auto *process = new QProcess(this);
    auto *limit = new QTimer(process);
    limit->setSingleShot(true);
    connect(limit, &QTimer::timeout, process, &QProcess::kill);
    connect(process, &QProcess::errorOccurred, this, [process, folder, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return; // finished() reports the rest
        process->deleteLater();
        done(Signature::Unavailable, {});
    });
    connect(process, &QProcess::finished, this, [process, folder, done](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        const bool good = status == QProcess::NormalExit && code == 0;
        const QString said = QString::fromLocal8Bit(process->readAllStandardError() + process->readAllStandardOutput()).trimmed();
        // Before OpenSSH 8.1 ssh-keygen had no -Y: it answers with its usage.
        const bool tooOld = !good && (said.contains("unknown option") || said.contains("illegal option")
                                      || said.contains("usage:", Qt::CaseInsensitive));
        done(good ? Signature::Good : tooOld ? Signature::Unavailable : Signature::Bad, said.left(200));
    });
    process->start(program, {"-Y", "verify", "-f", signersPath, "-I", "tether-update",
                             "-n", "tether-update", "-s", sigPath});
    if (process->state() == QProcess::NotRunning)
        return; // it couldn't start, and that has been reported
    process->write(manifest);
    process->closeWriteChannel();
    limit->start(20000);
}

bool Updater::signatureAccepted(Signature result, const QString &detail, QString *problem) const {
    if (result == Signature::Good)
        return true;
    if (result == Signature::Unavailable) {
#ifdef Q_OS_ANDROID
        // No ssh-keygen on a phone. Android itself refuses an APK that isn't
        // signed with Tether's key, and the checksum is still checked.
        Q_UNUSED(detail)
        Q_UNUSED(problem)
        return true;
#else
        *problem = tr("Updates are signed, and checking the signature needs ssh-keygen from OpenSSH 8.1 or later, which wasn't found.");
        return false;
#endif
    }
    if (!detail.isEmpty())
        qWarning("tether: update signature refused: %s", qPrintable(detail));
    *problem = tr("The update's signature isn't Tether's, so it won't be installed.");
    return false;
}

void Updater::check(bool quiet) {
    if (!configured() || m_checking || m_reply || m_state == "downloading")
        return;
    m_checking = true;
    if (!quiet)
        setState("checking", tr("Checking for updates…"));
    // Your own server first, when there is one; GitHub otherwise.
    if (!m_mirror.isEmpty())
        checkMirror(quiet);
    else
        checkGitHub(quiet);
}

void Updater::finishCheck(bool quiet) {
    m_checking = false;
    if (available())
        setState("idle", tr("Tether %1 is available.").arg(m_latest));
    else
        setState("idle", quiet ? QString() : tr("Tether %1 is the newest version.").arg(m_current));
}

// A release that can't be trusted is not offered at all.
void Updater::refuse(bool quiet, const QString &why) {
    m_checking = false;
    m_latest.clear();
    m_asset.clear();
    m_sources.clear();
    m_sha256.clear();
    m_size = 0;
    m_downloadUrl.clear();
    setState(quiet ? "idle" : "error", quiet ? QString() : why);
}

void Updater::nothingNewer(const QString &version, const QString &source, bool quiet) {
    m_latest = version;
    m_asset.clear();
    m_sources.clear();
    m_sha256.clear();
    m_size = 0;
    m_downloadUrl.clear();
    m_source = source;
    finishCheck(quiet);
}

// Your server: latest.json and its signature, as bin/mirror-release copied
// them from the release. Whatever goes wrong there, GitHub is asked instead.
void Updater::checkMirror(bool quiet) {
    const QUrl base(m_mirror + "/latest.json");
    fetch(base, "application/json", 10000, 256 * 1024, [this, quiet, base](int status, const QByteArray &manifest, const QString &) {
        const QJsonObject release = QJsonDocument::fromJson(manifest).object();
        const QString version = cleanVersion(release.value("version").toString());
        if (status != 200 || version.isEmpty()) {
            checkGitHub(quiet);
            return;
        }
        if (compareVersions(version, m_current) <= 0) {
            nothingNewer(version, base.host(), quiet);
            return;
        }
        fetch(QUrl(m_mirror + "/latest.json.sig"), "*/*", 10000, 16 * 1024,
              [this, quiet, base, manifest, release](int status, const QByteArray &signature, const QString &) {
            if (status != 200 || signature.isEmpty()) {
                checkGitHub(quiet);
                return;
            }
            verify(manifest, signature, [this, quiet, base, release](Signature result, const QString &detail) {
                QString problem;
                m_notes.clear();
                m_releaseUrl.clear();
                if (!signatureAccepted(result, detail, &problem) || !adopt(release, {}, base, {}, &problem)) {
                    checkGitHub(quiet);
                    return;
                }
                m_source = base.host();
                finishCheck(quiet);
            });
        });
    });
}

// GitHub: the newest release from its API, then that release's latest.json
// and signature, which say what its files must be.
void Updater::checkGitHub(bool quiet) {
    fetch(latestReleaseUrl(), "application/vnd.github+json", 20000, 4 * 1024 * 1024,
          [this, quiet](int status, const QByteArray &body, const QString &error) {
        const QJsonObject release = QJsonDocument::fromJson(body).object();
        const QString version = cleanVersion(release.value("tag_name").toString());
        if (status != 200 || version.isEmpty()) {
            const QString why = status == 404 ? tr("No releases have been published yet.")
                              : status == 0 ? tr("Couldn't reach the update server: %1").arg(error)
                              : tr("The update server answered with HTTP %1.").arg(status);
            m_checking = false;
            setState(quiet ? "idle" : "error", quiet ? QString() : why);
            return;
        }
        if (compareVersions(version, m_current) <= 0) {
            nothingNewer(version, "GitHub", quiet);
            return;
        }
        const QJsonArray assets = release.value("assets").toArray();
        const QList<QUrl> list = githubLinks(assets, "latest.json"), sig = githubLinks(assets, "latest.json.sig");
        if (list.isEmpty() || sig.isEmpty()) {
            refuse(quiet, tr("Tether %1 was published without a signature, so it can't be checked and won't be installed.").arg(version));
            return;
        }
        fetchFirst(list, 256 * 1024, [this, quiet, release, version, assets, sig](bool ok, const QByteArray &manifest) {
            if (!ok) {
                refuse(quiet, tr("Couldn't download the signed file list of Tether %1.").arg(version));
                return;
            }
            fetchFirst(sig, 16 * 1024, [this, quiet, release, version, assets, manifest](bool ok, const QByteArray &signature) {
                if (!ok) {
                    refuse(quiet, tr("Couldn't download the signature of Tether %1.").arg(version));
                    return;
                }
                verify(manifest, signature, [this, quiet, release, version, assets, manifest](Signature result, const QString &detail) {
                    QString problem;
                    m_notes = release.value("body").toString().trimmed();
                    const QString page = release.value("html_url").toString();
                    m_releaseUrl = QUrl(page).scheme() == "https" ? page : QString();
                    if (!signatureAccepted(result, detail, &problem)
                        || !adopt(QJsonDocument::fromJson(manifest).object(), version, {}, assets, &problem)) {
                        refuse(quiet, problem);
                        return;
                    }
                    m_source = "GitHub";
                    finishCheck(quiet);
                });
            });
        });
    });
}

void Updater::download() {
    if (!available() || m_reply || m_checking)
        return;
    if (m_sources.isEmpty()) {
        setState("error", tr("This release has no download for this system; get it from the release page."));
        return;
    }
    // Nothing is downloaded that couldn't be checked afterwards.
    if (m_sha256.isEmpty() || m_size <= 0) {
        setState("error", tr("This release has no checksum for its file, so it won't be installed."));
        return;
    }
    m_progress = 0;
    setState("downloading", tr("Downloading Tether %1…").arg(m_latest));
    downloadFrom(0);
}

// Tries each place the file is offered until one delivers it whole, with
// the size and checksum the signed list gives. The file is written as it
// arrives, beside its final name, and only renamed once it checks out.
void Updater::downloadFrom(int index) {
    if (index >= m_sources.size()) {
        // Your server's copy didn't work out: ask GitHub for the same release.
        // The checksum to meet stays the one from the signed list.
        if (m_source != "GitHub" && !m_repository.isEmpty()) {
            fetch(latestReleaseUrl(), "application/vnd.github+json", 20000, 4 * 1024 * 1024,
                  [this](int status, const QByteArray &body, const QString &) {
                const QJsonObject release = QJsonDocument::fromJson(body).object();
                const QList<QUrl> links = githubLinks(release.value("assets").toArray(), m_asset.value("name").toString());
                if (status == 200 && cleanVersion(release.value("tag_name").toString()) == m_latest && !links.isEmpty()) {
                    m_source = "GitHub";
                    m_sources = links;
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
    const QString directory = m_directory.isEmpty() ? updatesDirectory() : m_directory;
    QDir().mkpath(directory);
    const QString target = directory + "/" + QFileInfo(m_asset.value("name").toString()).fileName();
    const QUrl url = m_sources.at(index);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Tether/" + m_current);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (url.toString().startsWith(m_api))
        request.setRawHeader("Accept", "application/octet-stream");
    request.setTransferTimeout(60000);
    m_reply = m_network.get(request);
    QNetworkReply *reply = m_reply;

    struct Arriving {
        QFile part;
        QCryptographicHash hash{QCryptographicHash::Sha256};
        qint64 received = 0;
        QString problem;
    };
    auto state = std::make_shared<Arriving>();
    state->part.setFileName(target + ".part");
    if (!state->part.open(QIODevice::WriteOnly | QIODevice::Truncate))
        state->problem = tr("couldn't write %1: %2").arg(state->part.fileName(), state->part.errorString());
    const qint64 expected = m_size;
    auto take = [this, reply, state, expected] {
        // A reply cut off for being too long is closed, with nothing to read.
        const QByteArray chunk = reply->isOpen() ? reply->readAll() : QByteArray();
        if (chunk.isEmpty() || !state->problem.isEmpty())
            return;
        state->received += chunk.size();
        if (state->received > expected) {
            state->problem = tr("the file is bigger than the %1 bytes it should be").arg(expected);
            reply->abort();
            return;
        }
        state->hash.addData(chunk);
        if (state->part.write(chunk) != chunk.size())
            state->problem = tr("couldn't write %1: %2").arg(state->part.fileName(), state->part.errorString());
        m_progress = double(state->received) / expected;
        emit changed();
    };
    connect(reply, &QNetworkReply::readyRead, this, take);
    connect(reply, &QNetworkReply::finished, this, [this, reply, state, take, target, index, url, expected] {
        take();
        reply->deleteLater();
        m_reply = nullptr;
        state->part.close();
        QString problem = state->problem;
        if (!problem.isEmpty())
            ; // already known
        else if (reply->error() != QNetworkReply::NoError)
            problem = reply->errorString();
        else if (state->received != expected)
            problem = tr("got %1 bytes instead of %2").arg(state->received).arg(expected);
        else if (state->hash.result().toHex() != m_sha256.toLatin1())
            problem = tr("the file's checksum doesn't match");
        if (!problem.isEmpty()) {
            state->part.remove();
            m_lastDownloadError = url.host() + ": " + problem;
            m_progress = 0;
            downloadFrom(index + 1);
            return;
        }
        QFile::remove(target);
        if (!state->part.rename(target)) {
            const QString why = state->part.errorString();
            state->part.remove();
            setState("error", tr("Couldn't save the update: %1").arg(why));
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
