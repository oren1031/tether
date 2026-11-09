#pragma once

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QJsonObject>
#include <QUrl>
#include <QVariant>

#include <functional>

class QNetworkReply;

// Checks for a newer Tether (your own update server first, then GitHub
// Releases), downloads the build for this system, and hands it to the
// installer: pacman on Arch (run in a terminal tab, so sudo can ask for the
// password there), the setup program on Windows, or the release page
// anywhere else.
//
// What is installed is decided by a signed list, not by the server: every
// release carries latest.json (its version and each file's size and SHA-256)
// and latest.json.sig, made with a key that is on no server. The public half
// is built in, ssh-keygen checks the signature, and a file is only kept when
// it matches the list. A release without a valid signature, or without a
// checksum for the file, is refused. (On Android there is no ssh-keygen; the
// system itself refuses an APK not signed with Tether's key.)
class Updater : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(QString notes READ notes NOTIFY changed)
    Q_PROPERTY(QString releaseUrl READ releaseUrl NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool configured READ configured CONSTANT)
    Q_PROPERTY(QString state READ state NOTIFY changed)   // idle, checking, downloading, ready, error
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString downloadedFile READ downloadedFile NOTIFY changed)
    Q_PROPERTY(QString installKind READ installKind CONSTANT) // pacman, appimage, windows, android, page
    // The new version's file for this system; Android installs it through the browser.
    Q_PROPERTY(QString downloadUrl READ downloadUrl NOTIFY changed)

public:
    explicit Updater(const QString &repository, const QString &currentVersion, QObject *parent = nullptr);

    // A server of your own with Tether's latest.json and files, checked
    // before GitHub: https://tether.example.com (empty for GitHub alone).
    // Only https is taken (http for this computer itself, for testing); the
    // reason anything else was refused is in mirrorProblem.
    Q_PROPERTY(QString mirror READ mirror WRITE setMirror NOTIFY changed)
    Q_PROPERTY(QString mirrorProblem READ mirrorProblem NOTIFY changed)
    QString mirror() const { return m_mirror; }
    QString mirrorProblem() const { return m_mirrorProblem; }
    void setMirror(const QString &url);
    // https, or http to this computer itself.
    static bool safeUrl(const QUrl &url);
    // Where the last check found the release: the server's name or GitHub.
    Q_PROPERTY(QString source READ source NOTIFY changed)
    QString source() const { return m_source; }

    QString currentVersion() const { return m_current; }
    QString latestVersion() const { return m_latest; }
    QString notes() const { return m_notes; }
    QString releaseUrl() const { return m_releaseUrl; }
    bool available() const { return !m_latest.isEmpty() && compareVersions(m_latest, m_current) > 0; }
    bool configured() const { return !m_repository.isEmpty(); }
    QString state() const { return m_state; }
    QString message() const { return m_message; }
    double progress() const { return m_progress; }
    QString downloadedFile() const { return m_file; }
    QString downloadUrl() const { return m_downloadUrl; }
    QString installKind() const;

    // quiet checks don't report "you're up to date" or network errors.
    Q_INVOKABLE void check(bool quiet = false);
    Q_INVOKABLE void download();
    // What to run to install the downloaded update: program and args.
    Q_INVOKABLE QVariantMap installCommand() const;
    Q_INVOKABLE void startAutomaticChecks(bool enabled);
    // Starts the installed Tether again and quits this one.
    Q_INVOKABLE void restart();
    // Windows: starts the setup program, which closes Tether and reopens it.
    // Android: opens the system installer on the downloaded APK.
    Q_INVOKABLE bool launchInstaller();
    static bool openAndroidInstaller(const QString &apk);
    static QString updatesDirectory();
    // Takes a release from its signed list: the version (which must be newer
    // than this one, and the one expected when that is given), and this
    // system's file with its size, checksum, and where to download it: under
    // the update server's address, and GitHub's links for the same name.
    // False with the reason when the list can't be used.
    bool adopt(const QJsonObject &manifest, const QString &expectedVersion, const QUrl &mirrorBase,
               const QJsonArray &githubAssets, QString *problem);
    QList<QUrl> sources() const { return m_sources; }
    QString expectedSha256() const { return m_sha256; }
    qint64 expectedSize() const { return m_size; }
    // "1.2.3" from "v1.2.3"; empty for anything that isn't a version.
    static QString cleanVersion(const QString &version);

    // The key updates must be signed with: the base64 part of an ed25519
    // OpenSSH public key. Tests sign with a key of their own, and stand in
    // for GitHub's API, the kind of install, ssh-keygen, and the folder.
    void setPublicKey(const QString &base64) { m_publicKey = base64; }
    void setApiBase(const QString &url) { m_api = url; }
    void setInstallKind(const QString &kind) { m_kind = kind; }
    void setKeygen(const QString &program) { m_keygen = program; m_keygenSet = true; }
    void setDownloadDirectory(const QString &directory) { m_directory = directory; }
    // AppImage: swaps the downloaded file in for the running one and restarts.
    Q_INVOKABLE bool replaceAppImage();

    // Asks ssh-keygen whether a signature over these bytes was made with the
    // update key. Unavailable when there is no ssh-keygen to ask. (Public for
    // the self-test, which proves each system's ssh-keygen can do it.)
    enum class Signature { Good, Bad, Unavailable };
    void verify(const QByteArray &manifest, const QByteArray &signature,
                const std::function<void(Signature, const QString &)> &done);

    // -1, 0, or 1, comparing "1.2.10" with "v1.2.9" and the like.
    static int compareVersions(const QString &a, const QString &b);
    // The asset for this kind of install from a release's assets.
    static QVariantMap pickAsset(const QVariantList &assets, const QString &kind);

signals:
    void changed();

private:
    using Fetched = std::function<void(int status, const QByteArray &body, const QString &error)>;

    void setState(const QString &state, const QString &message = {});
    void checkMirror(bool quiet);
    void checkGitHub(bool quiet);
    void finishCheck(bool quiet);
    void refuse(bool quiet, const QString &why);
    void nothingNewer(const QString &version, const QString &source, bool quiet);
    void fetch(const QUrl &url, const QByteArray &accept, int timeout, qint64 limit, const Fetched &done);
    void fetchFirst(const QList<QUrl> &urls, qint64 limit, const std::function<void(bool, const QByteArray &)> &done);
    bool signatureAccepted(Signature result, const QString &detail, QString *problem) const;
    QList<QUrl> githubLinks(const QJsonArray &assets, const QString &name) const;
    QUrl latestReleaseUrl() const;
    void downloadFrom(int index);

    QString m_repository;
    QString m_current;
    QString m_latest;
    QString m_notes;
    QString m_releaseUrl;
    QString m_state = "idle";
    QString m_message;
    double m_progress = 0;
    QString m_file;
    QVariantMap m_asset;
    QString m_downloadUrl;
    QString m_mirror;
    QString m_mirrorProblem;
    QString m_source;
    QString m_sha256;
    qint64 m_size = 0;
    bool m_checking = false;
    QString m_publicKey;
    QString m_api = "https://api.github.com/";
    QString m_kind;
    QString m_keygen;
    bool m_keygenSet = false;
    QString m_directory;
    QString m_lastDownloadError;
    QList<QUrl> m_sources;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_timer;
};
