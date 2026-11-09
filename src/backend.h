#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QUrl>
#include <QVariant>

#include <functional>

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
    // What the sync folder wants to change that waits for a look: see syncReview().
    Q_PROPERTY(QVariantList syncReview READ syncReview NOTIFY syncReviewChanged)
    // A serial device name as this system writes them, for hints.
    Q_PROPERTY(QString exampleDevice READ exampleDevice CONSTANT)
    // Phones and tablets: bigger targets, no keyboard shortcuts. TETHER_TOUCH=1
    // turns it on elsewhere, to try the phone layout.
    Q_PROPERTY(bool touchUi READ touchUi CONSTANT)
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
    // Returns what's wrong with the login at once; the keyring is asked in
    // the background (it can sit behind an unlock prompt), and identitySaved
    // says how that went, with an empty error when the login is saved.
    Q_INVOKABLE QString saveIdentity(const QVariantMap &identity, const QString &password, bool clearPassword = false);
    Q_INVOKABLE QString removeIdentity(const QString &id);
    // A private key picked in a file dialog, as a path ssh can use: path or error.
    Q_INVOKABLE QVariantMap importKey(const QUrl &url);
    // Removes a host's saved key, after the server's key changed. The answer
    // comes through hostKeyForgotten with the same token: empty or an error.
    Q_INVOKABLE void forgetHostKey(const QVariantMap &profile, const QString &token = {});
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

    // Asks whether an ssh host's shared connection is up, meaning the login
    // is done and it's safe to type startup snippets. The answer comes
    // through connectionChecked with the same token.
    Q_INVOKABLE void checkConnection(const QVariantMap &profile, const QString &token);

    // How to run sftp against a host: program, args, and environment; or an error.
    Q_INVOKABLE QVariantMap sftpCommand(const QVariantMap &profile) const;

    // Sync: hosts, snippets, and the vault's logins (never passwords) are
    // mirrored to tether-hosts.json in the syncFolder setting, for Syncthing,
    // Dropbox, or a git repository to carry between computers.
    // Sync is another computer, a phone, or a storage service writing to
    // this one, so what it can do alone is limited. A synced host that
    // carries options which run programs or weaken host checks, and a synced
    // change to where an existing host connects or what is typed into it,
    // wait here until approved; the saved copy stays in use meanwhile.
    QString syncStatus() const { return m_syncStatus; }
    // One entry per waiting item: id, type (host, snippet, login), name,
    // isNew, risky (option names), and changes ({label, before, after}).
    QVariantList syncReview() const;
    // An empty id approves or rejects everything that waits.
    Q_INVOKABLE QString approveSync(const QString &id = {});
    Q_INVOKABLE QString rejectSync(const QString &id = {});
    // The ssh options in a host that run a program on this computer or turn
    // off checks of who the server is.
    static QStringList riskyOptions(const QVariantMap &profile);
    static bool touchUi() {
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
        return true;
#else
        return qEnvironmentVariable("TETHER_TOUCH") == "1";
#endif
    }
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
    void syncReviewChanged();
    void connectionChecked(const QString &token, bool ready);
    void hostKeyForgotten(const QString &token, const QString &error);
    void identitySaved(const QString &error);

private:
    void load();
    QString save();
    void reloadSshConfig();
    void readSyncFile();
    void writeSyncFile();
    void watchSyncFile();
    QString syncFile() const;
    bool mergeSynced(const QString &type, QVariantList &mine, const QVariantList &theirs);
    void hold(const QString &type, const QVariantMap &item, const QVariantMap &before);
    bool release(const QString &type, const QString &id, bool forget = false);
    QString storeIdentity(const QVariantMap &login);
    // Runs a helper program without waiting for it; done hears whether it
    // exited with 0 and what it wrote to stderr.
    void runTool(const QString &program, const QStringList &args, const QByteArray &input, int timeoutMs,
                 const std::function<void(bool, const QString &)> &done);
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
    // From the sync folder, not applied: {type, id, item, before, state}.
    // "pending" waits for the user; "rejected" was turned down and is only
    // remembered so the same copy isn't offered again.
    QVariantList m_held;
    // The last copy of what a sync removed, so the same id coming back with
    // a different address is seen as a change, not as a new host.
    QVariantMap m_tombstones;
    bool m_heldChanged = false; // while reading the sync file
    bool m_syncStale = false;   // the sync file has an older copy than ours
    QStringList m_checking;
    bool m_vaultBusy = false;
    QFileSystemWatcher m_syncWatcher;
    QProcess m_tmuxProcess;
    QHash<QString, QString> m_tools;
    QFileSystemWatcher m_sshWatcher;
};
