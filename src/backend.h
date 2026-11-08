#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QUrl>
#include <QVariant>

// Saved hosts, serial ports, and snippets, and the command line each session
// runs. Nothing here goes through a shell unless a session is logged, and
// then every argument is quoted.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(QVariantList configHosts READ configHosts NOTIFY configHostsChanged)
    Q_PROPERTY(QVariantList snippets READ snippets NOTIFY snippetsChanged)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged)
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList identities READ identities NOTIFY identitiesChanged)
    Q_PROPERTY(QVariantList tmuxSessions READ tmuxSessions NOTIFY tmuxSessionsChanged)
    Q_PROPERTY(bool keyringAvailable READ keyringAvailable CONSTANT)
    Q_PROPERTY(bool moshAvailable READ moshAvailable CONSTANT)
    Q_PROPERTY(bool tmuxAvailable READ tmuxAvailable CONSTANT)
    Q_PROPERTY(bool sftpAvailable READ sftpAvailable CONSTANT)
    Q_PROPERTY(QString syncStatus READ syncStatus NOTIFY syncStatusChanged)
    // A serial device name as this system writes them, for hints.
    Q_PROPERTY(QString exampleDevice READ exampleDevice CONSTANT)
    Q_PROPERTY(QString logDirectory READ logDirectory CONSTANT)
    Q_PROPERTY(QString loadError READ loadError CONSTANT)

public:
    explicit Backend(const QString &configFile = {}, const QString &sshConfig = {},
                     const QString &logDirectory = {}, QObject *parent = nullptr);

    QVariantList profiles() const { return m_profiles; }
    QVariantList configHosts() const { return m_configHosts; }
    QVariantList snippets() const { return m_snippets; }
    int fontSize() const { return m_fontSize; }
    void setFontSize(int size);
    QString logDirectory() const { return m_logDirectory; }

    // Preferences: every key in defaultSettings(), with what the user changed
    // laid over the defaults. Unknown keys and values of the wrong type are
    // refused, so the file can't grow junk.
    QVariantMap settings() const;
    static QVariantMap defaultSettings();
    Q_INVOKABLE QString setSetting(const QString &key, const QVariant &value);
    Q_INVOKABLE QString resetSettings();

    // The vault: logins (a user, a key file, and a password or passphrase)
    // that hosts can use. Passwords live in the system keyring through
    // secret-tool, never in Tether's files; ssh gets them from an askpass
    // helper. An empty password keeps the saved one; clearPassword drops it.
    QVariantList identities() const { return m_identities; }
    Q_INVOKABLE QString saveIdentity(const QVariantMap &identity, const QString &password, bool clearPassword = false);
    Q_INVOKABLE QString removeIdentity(const QString &id);
    // A private key picked in a file dialog, as a path ssh can use: path or error.
    Q_INVOKABLE QVariantMap importKey(const QUrl &url);
    // Removes a host's saved key, after the server's key changed: empty or an error.
    Q_INVOKABLE QString forgetHostKey(const QVariantMap &profile) const;
#ifdef Q_OS_WIN
    bool keyringAvailable() const { return true; } // Windows Credential Manager
#else
    bool keyringAvailable() const { return !tool("secret-tool").isEmpty(); }
#endif
    bool moshAvailable() const { return !tool("mosh").isEmpty(); }
    bool tmuxAvailable() const { return !tool("tmux").isEmpty(); }
    bool sftpAvailable() const { return !tool("sftp").isEmpty(); }

    // Local tmux sessions Tether started that nothing is attached to, so a
    // shell left running when Tether closed can be picked up again.
    QVariantList tmuxSessions() const { return m_tmuxSessions; }
    Q_INVOKABLE void refreshTmux();

    // Whether an ssh host's shared connection is up, meaning the login is
    // done and it's safe to type startup snippets.
    Q_INVOKABLE bool connectionReady(const QVariantMap &profile) const;

    // How to run sftp against a host: program, args, and environment; or an error.
    Q_INVOKABLE QVariantMap sftpCommand(const QVariantMap &profile) const;

    // Sync: hosts, snippets, and the vault's logins (never passwords) are
    // mirrored to tether-hosts.json in the syncFolder setting, for Syncthing,
    // Dropbox, or a git repository to carry between computers.
    QString syncStatus() const { return m_syncStatus; }
#ifdef Q_OS_WIN
    static QString exampleDevice() { return QStringLiteral("COM3"); }
#else
    static QString exampleDevice() { return QStringLiteral("/dev/ttyUSB0"); }
#endif
    QString loadError() const { return m_loadError; }

    // Each returns an error to show, or an empty string on success.
    Q_INVOKABLE QString saveProfile(const QVariantMap &profile);
    Q_INVOKABLE QString removeProfile(const QString &id);
    Q_INVOKABLE QString saveSnippet(const QVariantMap &snippet);
    Q_INVOKABLE QString removeSnippet(const QString &id);

    // What a terminal pane runs for a profile: program, args, title,
    // workingDirectory, logPath, splittable, and the profile itself; or just
    // an error.
    Q_INVOKABLE QVariantMap launch(const QVariantMap &profile) const;
    // "pi@10.0.0.2:2222", "router", or "/dev/ttyUSB0 9600" as a profile, or an error.
    Q_INVOKABLE QVariantMap quickConnect(const QString &text) const;
    // The text a snippet types: lines end in Enter, and a "run" snippet
    // presses Enter at the end.
    // {{name}} placeholders fill from values; date, time, host, and user
    // are filled in by Tether, the rest are asked for.
    Q_INVOKABLE static QString snippetPayload(const QVariantMap &snippet, const QVariantMap &values = {});
    Q_INVOKABLE static QStringList snippetVariables(const QString &text);
    static QString tmuxSessionName(const QString &name);

    static QString validate(const QVariantMap &profile);
    static QVariantMap clean(const QVariantMap &profile);
    static QString shellQuote(const QString &argument);
    static QVariantList parseSshConfig(const QString &path);

    // Tests point tools at stand-ins; an empty path means it isn't installed.
    void setTool(const QString &name, const QString &path) { m_tools.insert(name, path); }

signals:
    void profilesChanged();
    void configHostsChanged();
    void snippetsChanged();
    void fontSizeChanged();
    void settingsChanged();
    void identitiesChanged();
    void tmuxSessionsChanged();
    void syncStatusChanged();

private:
    void load();
    QString save();
    void reloadSshConfig();
    void readSyncFile();
    void writeSyncFile();
    void watchSyncFile();
    QString syncFile() const;
    QString askpass() const;
    QString controlPath() const;
    QString newLogPath(const QString &title) const;
    bool sharesConnections() const;
#ifdef TETHER_LIBSSH
    QVariantMap builtInSsh(const QVariantMap &profile, const QString &title, const QVariantMap &original) const;
#endif
    QVariantMap identity(const QString &id) const;
    QVariantMap resolved(const QVariantMap &profile) const;
    QStringList askpassEnvironment(const QVariantMap &profile, bool terminal) const;
    QString tool(const QString &name) const;

    QString m_configFile;
    QString m_sshConfig;
    QString m_logDirectory;
    QString m_loadError;
    QVariantList m_profiles;
    QVariantList m_configHosts;
    QVariantList m_snippets;
    int m_fontSize = 13;
    QVariantMap m_settings;
    QVariantList m_identities;
    QVariantList m_tmuxSessions;
    QString m_syncStatus;
    QByteArray m_lastSync;
    QStringList m_removed;
    QFileSystemWatcher m_syncWatcher;
    QProcess m_tmuxProcess;
    QHash<QString, QString> m_tools;
    QFileSystemWatcher m_sshWatcher;
};
