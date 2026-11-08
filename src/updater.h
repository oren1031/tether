#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariant>

class QNetworkReply;

// Checks GitHub Releases for a newer Tether, downloads the build for this
// system, and hands it to the installer: pacman on Arch (run in a terminal
// tab, so sudo can ask for the password there), the setup program on
// Windows, or the release page anywhere else.
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
    // AppImage: swaps the downloaded file in for the running one and restarts.
    Q_INVOKABLE bool replaceAppImage();

    // -1, 0, or 1, comparing "1.2.10" with "v1.2.9" and the like.
    static int compareVersions(const QString &a, const QString &b);
    // The asset for this kind of install from a release's assets.
    static QVariantMap pickAsset(const QVariantList &assets, const QString &kind);

signals:
    void changed();

private:
    void setState(const QString &state, const QString &message = {});

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
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_timer;
};
