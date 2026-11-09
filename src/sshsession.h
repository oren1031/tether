#pragma once

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QVariantMap>

#include <atomic>

// An ssh connection made by Tether itself with libssh, for systems without an
// ssh program (Android). It runs on its own thread. Questions ssh would ask
// (a new host key, a password, a key's passphrase) are asked in the terminal:
// the prompt arrives through dataReceived, and what's typed comes back
// through write(), just as with the ssh program.
class SshSession : public QThread {
    Q_OBJECT

public:
    // settings: host, port, user, identityFile, password, passphrase,
    // command (run instead of a shell), knownHosts (file), columns, rows.
    explicit SshSession(const QVariantMap &settings, QObject *parent = nullptr);
    ~SshSession() override;

    void write(const QByteArray &data);
    void resize(int columns, int rows);
    void stop();
    // Ends a session nobody is watching anymore (its tab closed), without
    // waiting: the thread is told to stop and the object deletes itself once
    // the thread is done, which can take a call's timeout while it's still
    // connecting or logging in.
    void retire();
    // The answer to hostKeyQuestion, from a dialog; typing it works too.
    void answerHostKey(bool trust);
    // Takes a host's entries out of a known_hosts file, for a key that changed.
    static bool forgetHost(const QString &knownHosts, const QString &host, int port);
    // The keys a known_hosts file has for a host, as "TYPE SHA256:…", to
    // show beside a new key that doesn't match them.
    static QStringList savedFingerprints(const QString &knownHosts, const QString &host, int port);

    // Exit codes: the remote program's own; 255 when the connection fails
    // or drops, which the pane reconnects from; 254 when logging in was
    // refused (a host key turned down, a wrong password), which it doesn't,
    // so the same question isn't asked again and again.
    static constexpr int ConnectionFailed = 255;
    static constexpr int LoginRefused = 254;

signals:
    void dataReceived(const QByteArray &data);
    // A host key to trust or not: new, or changed since last time, and then
    // with the saved keys it would replace.
    void hostKeyQuestion(const QString &host, const QString &keyType, const QString &fingerprint, bool changed,
                         const QStringList &oldFingerprints);
    // A password or passphrase is being typed (true) or done (false).
    void secretInput(bool on);
    void finished(int exitCode);

protected:
    void run() override;

private:
    void say(const QString &text);
    // Asks in the terminal and waits for a line; empty when stopped.
    QString ask(const QString &prompt, bool echo);
    // Asks in the terminal and through hostKeyQuestion. A new key takes a
    // typed "yes"; a changed one only the text in mustType (the host's
    // name), so it can't be agreed to by reflex.
    bool askTrust(const QString &prompt, const QString &mustType = {});
    // The thread sleeps until there is something to do: typed input, a new
    // size, a stop (wake), or, when asked, bytes from the server.
    void wake();
    void waitForWork(int timeoutMs, bool watchSocket);
    QByteArray takeInput();
    // A question can wait a long time for its answer, and the connection can
    // drop meanwhile (a phone changing networks). True once that's seen, so
    // the answer isn't sent into a dead connection and called a wrong password.
    bool linkLost();

    QVariantMap m_settings;
    QMutex m_mutex;
    QByteArray m_input;
    int m_columns;
    int m_rows;
    std::atomic<bool> m_resized{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<int> m_trust{-1};
    int m_socket = -1;   // the connection's socket, once there is one
    int m_wake[2] = {-1, -1}; // a pipe written to wake the thread
    bool m_lost = false;
};
