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

    // Exit codes: the remote program's own, or 255 when the connection
    // fails or drops, as the ssh program does.
    static constexpr int ConnectionFailed = 255;

signals:
    void dataReceived(const QByteArray &data);
    void finished(int exitCode);

protected:
    void run() override;

private:
    void say(const QString &text);
    // Asks in the terminal and waits for a line; empty when stopped.
    QString ask(const QString &prompt, bool echo);
    QByteArray takeInput();

    QVariantMap m_settings;
    QMutex m_mutex;
    QByteArray m_input;
    int m_columns;
    int m_rows;
    std::atomic<bool> m_resized{false};
    std::atomic<bool> m_stopping{false};
};
