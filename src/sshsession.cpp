#include "sshsession.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSaveFile>

#include <libssh/libssh.h>

#ifndef Q_OS_WIN
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace {

QString expandHome(const QString &path) {
    if (path == "~")
        return QDir::homePath();
    return path.startsWith("~/") ? QDir::homePath() + path.mid(1) : path;
}

// Frees libssh objects however run() leaves.
struct Session {
    ssh_session session = ssh_new();
    ssh_channel channel = nullptr;
    ~Session() {
        if (channel) {
            if (ssh_channel_is_open(channel)) {
                ssh_channel_send_eof(channel);
                ssh_channel_close(channel);
            }
            ssh_channel_free(channel);
        }
        if (session) {
            if (ssh_is_connected(session))
                ssh_disconnect(session);
            ssh_free(session);
        }
    }
};

} // namespace

SshSession::SshSession(const QVariantMap &settings, QObject *parent)
    : QThread(parent), m_settings(settings),
      m_columns(qMax(2, settings.value("columns", 80).toInt())),
      m_rows(qMax(1, settings.value("rows", 24).toInt())) {}

SshSession::~SshSession() {
    stop();
    wait(5000);
}

void SshSession::write(const QByteArray &data) {
    QMutexLocker lock(&m_mutex);
    m_input += data;
}

void SshSession::resize(int columns, int rows) {
    QMutexLocker lock(&m_mutex);
    m_columns = qMax(2, columns);
    m_rows = qMax(1, rows);
    m_resized = true;
}

void SshSession::stop() { m_stopping = true; }

void SshSession::answerHostKey(bool trust) { m_trust = trust ? 1 : 0; }

bool SshSession::askTrust(const QString &prompt) {
    say(prompt);
    QByteArray line;
    int waited = 0;
    while (!m_stopping) {
        const int decided = m_trust.exchange(-1);
        if (decided >= 0) {
            say(decided ? "yes\n" : "no\n");
            return decided == 1;
        }
        for (const char c : takeInput()) {
            if (c == '\r' || c == '\n') {
                say("\n");
                // "yes", "Yes", "y", "yes." from a phone keyboard all mean yes.
                const QString answer = QString::fromUtf8(line).trimmed().toLower();
                return answer.startsWith('y');
            }
            if (c == 0x7f || c == 0x08) {
                if (!line.isEmpty()) {
                    line.chop(1);
                    emit dataReceived("\b \b");
                }
                continue;
            }
            if (c == 0x03) {
                say("^C\n");
                return false;
            }
            if (uchar(c) >= 0x20) {
                line += c;
                emit dataReceived(QByteArray(1, c));
            }
        }
        msleep(20);
        if (++waited % 25 == 0 && linkLost())
            break;
    }
    return false;
}

bool SshSession::linkLost() {
    if (m_lost || m_socket < 0)
        return m_lost;
#ifndef Q_OS_WIN
    struct pollfd state{m_socket, POLLIN, 0};
    if (poll(&state, 1, 0) > 0) {
        if (state.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            m_lost = true;
        } else if (state.revents & POLLIN) {
            // Readable with nothing to read is the other end closing.
            char byte;
            const ssize_t got = recv(m_socket, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
            m_lost = got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
        }
    }
#endif
    return m_lost;
}

bool SshSession::forgetHost(const QString &knownHosts, const QString &host, int port) {
    QFile file(knownHosts);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QString name = port == 22 ? host : QString("[%1]:%2").arg(host).arg(port);
    QByteArray kept;
    bool removed = false;
    for (const QByteArray &line : file.readAll().split('\n')) {
        const QList<QByteArray> hosts = line.trimmed().split(' ').value(0).split(',');
        if (hosts.contains(name.toUtf8())) {
            removed = true;
            continue;
        }
        if (!line.isEmpty())
            kept += line + '\n';
    }
    file.close();
    if (!removed)
        return false;
    QSaveFile out(knownHosts);
    return out.open(QIODevice::WriteOnly) && out.write(kept) == kept.size() && out.commit();
}

QByteArray SshSession::takeInput() {
    QMutexLocker lock(&m_mutex);
    QByteArray data;
    data.swap(m_input);
    return data;
}

void SshSession::say(const QString &text) {
    QString out = text;
    out.replace("\r\n", "\n").replace('\n', "\r\n");
    emit dataReceived(out.toUtf8());
}

QString SshSession::ask(const QString &prompt, bool echo) {
    say(prompt);
    // A secret: the keyboard switches to password mode (no suggestions,
    // capitals, or added spaces), and each character shows as a star.
    if (!echo) {
        emit secretInput(true);
        emit dataReceived("\x1b" "7"); // where the stars start
    }
    int stars = 0;
    auto done = [&](const QString &answer) {
        if (!echo)
            emit secretInput(false);
        return answer;
    };
    QByteArray line;
    int waited = 0;
    while (!m_stopping) {
        const QByteArray input = takeInput();
        for (const char c : input) {
            if (c == '\r' || c == '\n') {
                say("\n");
                return done(QString::fromUtf8(line));
            }
            if (c == 0x7f || c == 0x08) {
                if (!line.isEmpty()) {
                    // Drop a whole UTF-8 character.
                    int cut = int(line.size()) - 1;
                    while (cut > 0 && (uchar(line[cut]) & 0xC0) == 0x80)
                        --cut;
                    line.truncate(cut);
                    if (echo) {
                        emit dataReceived("\b \b");
                    } else {
                        // Redrawn from the start, which stays right across a line wrap.
                        --stars;
                        emit dataReceived("\x1b" "8" "\x1b[J" + QByteArray(stars, '*'));
                    }
                }
                continue;
            }
            if (c == 0x03) { // Ctrl+C gives up
                say("^C\n");
                m_stopping = true;
                return done({});
            }
            if (uchar(c) < 0x20)
                continue;
            line += c;
            if (echo)
                emit dataReceived(QByteArray(1, c));
            else if ((uchar(c) & 0xC0) != 0x80) { // one star per character
                ++stars;
                emit dataReceived("*");
            }
        }
        msleep(20);
        // About twice a second: a dropped connection ends the question.
        if (++waited % 25 == 0 && linkLost())
            break;
    }
    return done({});
}

void SshSession::run() {
    Session s;
    if (!s.session) {
        say("tether: libssh couldn't start.\n");
        emit finished(LoginRefused);
        return;
    }
    const QString host = m_settings.value("host").toString();
    QString user = m_settings.value("user").toString();
    const int port = m_settings.value("port", 22).toInt();
    if (user.isEmpty()) {
        user = ask("login as: ", true).trimmed();
        if (user.isEmpty() || m_stopping) {
            emit finished(LoginRefused);
            return;
        }
    }

    const QByteArray hostBytes = host.toUtf8(), userBytes = user.toUtf8();
    ssh_options_set(s.session, SSH_OPTIONS_HOST, hostBytes.constData());
    ssh_options_set(s.session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(s.session, SSH_OPTIONS_USER, userBytes.constData());
    long timeout = 15;
    ssh_options_set(s.session, SSH_OPTIONS_TIMEOUT, &timeout);
    const QString knownHosts = m_settings.value("knownHosts").toString();
    const QByteArray knownHostsBytes = QFile::encodeName(knownHosts);
    if (!knownHosts.isEmpty()) {
        QDir().mkpath(QFileInfo(knownHosts).absolutePath());
        ssh_options_set(s.session, SSH_OPTIONS_KNOWNHOSTS, knownHostsBytes.constData());
    }
    // Don't read the system's ssh config; the profile says everything.
    int noConfig = 0;
    ssh_options_set(s.session, SSH_OPTIONS_PROCESS_CONFIG, &noConfig);

    if (ssh_connect(s.session) != SSH_OK) {
        say(QString("ssh: connect to host %1 port %2: %3\n").arg(host).arg(port)
                .arg(QString::fromUtf8(ssh_get_error(s.session))));
        emit finished(ConnectionFailed);
        return;
    }
    m_socket = int(ssh_get_fd(s.session));
    // The connection dropping while a question waited: say so and end as a
    // dropped connection, which the pane reconnects from.
    auto lost = [&] {
        if (!m_lost)
            return false;
        say(QString("\nConnection to %1 lost.\n").arg(host));
        emit finished(ConnectionFailed);
        return true;
    };

    // The server's host key, checked against known_hosts as ssh does.
    {
        ssh_key key = nullptr;
        unsigned char *hash = nullptr;
        size_t hashLength = 0;
        QString fingerprint, type;
        if (ssh_get_server_publickey(s.session, &key) == SSH_OK) {
            type = QString::fromLatin1(ssh_key_type_to_char(ssh_key_type(key)));
            if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLength) == 0) {
                char *text = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLength);
                fingerprint = QString::fromLatin1(text);
                ssh_string_free_char(text);
                ssh_clean_pubkey_hash(&hash);
            }
            ssh_key_free(key);
        }
        switch (ssh_session_is_known_server(s.session)) {
        case SSH_KNOWN_HOSTS_OK:
            break;
        case SSH_KNOWN_HOSTS_CHANGED:
        case SSH_KNOWN_HOSTS_OTHER: {
            emit hostKeyQuestion(host, type.toUpper(), fingerprint, true);
            const bool trust = askTrust(QString(
                "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\n"
                "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                "The %1 host key for %2 is now %3.\n"
                "Someone could be intercepting the connection; only go on if you know\n"
                "the server was reinstalled or replaced.\n"
                "Forget the old key and trust the new one (yes/no)? ").arg(type.toUpper(), host, fingerprint));
            if (!trust && lost())
                return;
            if (!trust) {
                say("Host key verification failed.\n");
                emit finished(LoginRefused);
                return;
            }
            forgetHost(knownHosts, host, port);
            if (ssh_session_update_known_hosts(s.session) == SSH_OK)
                say(QString("Warning: Replaced the key for '%1' (%2) in the list of known hosts.\n").arg(host, type.toUpper()));
            break;
        }
        case SSH_KNOWN_HOSTS_NOT_FOUND:
        case SSH_KNOWN_HOSTS_UNKNOWN: {
            emit hostKeyQuestion(host, type.toUpper(), fingerprint, false);
            const bool trust = askTrust(QString("The authenticity of host '%1' can't be established.\n"
                                                "%2 key fingerprint is %3.\n"
                                                "Are you sure you want to continue connecting (yes/no)? ")
                                            .arg(host, type.toUpper(), fingerprint));
            if (!trust && lost())
                return;
            if (!trust) {
                say("Host key verification failed.\n");
                emit finished(LoginRefused);
                return;
            }
            if (ssh_session_update_known_hosts(s.session) == SSH_OK)
                say(QString("Warning: Permanently added '%1' (%2) to the list of known hosts.\n").arg(host, type.toUpper()));
            else
                say(QString("Warning: couldn't save the key to %1: %2\n").arg(knownHosts, QString::fromUtf8(ssh_get_error(s.session))));
            break;
        }
        default:
            say(QString("ssh: couldn't check the host key: %1\n").arg(QString::fromUtf8(ssh_get_error(s.session))));
            emit finished(LoginRefused);
            return;
        }
    }

    // Logging in: keys first, then the password, as ssh does.
    bool authenticated = ssh_userauth_none(s.session, nullptr) == SSH_AUTH_SUCCESS;
    const int methods = ssh_userauth_list(s.session, nullptr);
    const QString keyFile = expandHome(m_settings.value("identityFile").toString());
    QString passphrase = m_settings.value("passphrase").toString();
    if (!authenticated && (methods & SSH_AUTH_METHOD_PUBLICKEY) && !keyFile.isEmpty()) {
        ssh_key key = nullptr;
        const QByteArray path = QFile::encodeName(keyFile);
        int tries = 0;
        while (!m_stopping && tries++ < 3) {
            const QByteArray phrase = passphrase.toUtf8();
            if (ssh_pki_import_privkey_file(path.constData(), passphrase.isEmpty() ? nullptr : phrase.constData(),
                                            nullptr, nullptr, &key) == SSH_OK)
                break;
            key = nullptr;
            if (!QFileInfo::exists(keyFile)) {
                say(QString("Warning: Identity file %1 not accessible.\n").arg(keyFile));
                break;
            }
            passphrase = ask(QString("Enter passphrase for key '%1': ").arg(keyFile), false);
            if (lost())
                return;
        }
        if (key) {
            authenticated = ssh_userauth_publickey(s.session, nullptr, key) == SSH_AUTH_SUCCESS;
            ssh_key_free(key);
        }
    }
    if (!authenticated && (methods & SSH_AUTH_METHOD_PUBLICKEY) && keyFile.isEmpty()) {
        const QByteArray phrase = passphrase.toUtf8();
        authenticated = ssh_userauth_publickey_auto(s.session, nullptr,
                                                    passphrase.isEmpty() ? nullptr : phrase.constData()) == SSH_AUTH_SUCCESS;
    }
    QString password = m_settings.value("password").toString();
    for (int attempt = 0; !authenticated && !m_stopping && attempt < 3; ++attempt) {
        if (methods & SSH_AUTH_METHOD_PASSWORD) {
            if (password.isEmpty() || attempt > 0)
                password = ask(QString("%1@%2's password: ").arg(user, host), false);
            if (lost())
                return;
            if (m_stopping)
                break;
            const int result = ssh_userauth_password(s.session, nullptr, password.toUtf8().constData());
            // An error, unlike a refusal, is the connection failing: not a wrong password.
            if (result == SSH_AUTH_ERROR && !m_stopping) {
                say(QString("\nConnection to %1 lost: %2\n").arg(host, QString::fromUtf8(ssh_get_error(s.session))));
                emit finished(ConnectionFailed);
                return;
            }
            authenticated = result == SSH_AUTH_SUCCESS;
        } else if (methods & SSH_AUTH_METHOD_INTERACTIVE) {
            int result = ssh_userauth_kbdint(s.session, nullptr, nullptr);
            while (result == SSH_AUTH_INFO && !m_stopping) {
                const int prompts = ssh_userauth_kbdint_getnprompts(s.session);
                for (int i = 0; i < prompts; ++i) {
                    char echo = 0;
                    const QString prompt = QString::fromUtf8(ssh_userauth_kbdint_getprompt(s.session, i, &echo));
                    const QString answer = ask(prompt, echo);
                    if (lost())
                        return;
                    ssh_userauth_kbdint_setanswer(s.session, i, answer.toUtf8().constData());
                }
                result = ssh_userauth_kbdint(s.session, nullptr, nullptr);
            }
            authenticated = result == SSH_AUTH_SUCCESS;
        } else {
            break;
        }
        if (!authenticated && !m_stopping)
            say("Permission denied, please try again.\n");
    }
    if (!authenticated) {
        if (!m_stopping)
            say(QString("%1@%2: Permission denied.\n").arg(user, host));
        emit finished(LoginRefused);
        return;
    }

    // A terminal on the server, running a shell or the given command.
    s.channel = ssh_channel_new(s.session);
    int columns, rows;
    {
        QMutexLocker lock(&m_mutex);
        columns = m_columns;
        rows = m_rows;
        m_resized = false;
    }
    const QByteArray command = m_settings.value("command").toString().toUtf8();
    if (!s.channel || ssh_channel_open_session(s.channel) != SSH_OK
        || ssh_channel_request_pty_size(s.channel, "xterm-256color", columns, rows) != SSH_OK
        || (command.isEmpty() ? ssh_channel_request_shell(s.channel)
                              : ssh_channel_request_exec(s.channel, command.constData())) != SSH_OK) {
        say(QString("ssh: couldn't open a terminal on %1: %2\n").arg(host, QString::fromUtf8(ssh_get_error(s.session))));
        emit finished(ConnectionFailed);
        return;
    }

    QElapsedTimer sinceKeepalive;
    sinceKeepalive.start();
    char buffer[16384];
    while (!m_stopping) {
        bool active = false;
        for (int stream = 0; stream < 2; ++stream) {
            const int got = ssh_channel_read_timeout(s.channel, buffer, sizeof buffer, stream, stream ? 0 : 20);
            if (got > 0) {
                emit dataReceived(QByteArray(buffer, got));
                active = true;
            } else if (got == SSH_ERROR) {
                say("\nConnection to " + host + " lost.\n");
                emit finished(ConnectionFailed);
                return;
            }
        }
        const QByteArray input = takeInput();
        if (!input.isEmpty()) {
            ssh_channel_write(s.channel, input.constData(), uint32_t(input.size()));
            active = true;
        }
        if (m_resized.exchange(false)) {
            QMutexLocker lock(&m_mutex);
            ssh_channel_change_pty_size(s.channel, m_columns, m_rows);
        }
        if (ssh_channel_is_eof(s.channel) || !ssh_channel_is_open(s.channel))
            break;
        // Keepalives find a dead connection; ssh's ServerAliveInterval does the same.
        if (!active && sinceKeepalive.elapsed() > 15000) {
            if (ssh_send_ignore(s.session, "keepalive") != SSH_OK) {
                say("\nConnection to " + host + " lost.\n");
                emit finished(ConnectionFailed);
                return;
            }
            sinceKeepalive.restart();
        }
    }
    // Whatever arrived just before the end.
    int got;
    while ((got = ssh_channel_read_nonblocking(s.channel, buffer, sizeof buffer, 0)) > 0)
        emit dataReceived(QByteArray(buffer, got));
    uint32_t exitCode = 0;
    char *signal = nullptr;
    int coreDumped = 0;
    ssh_channel_get_exit_state(s.channel, &exitCode, &signal, &coreDumped);
    if (signal)
        ssh_string_free_char(signal);
    say(QString("Connection to %1 closed.\n").arg(host));
    emit finished(m_stopping ? 0 : int(exitCode));
}
