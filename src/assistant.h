#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>
#include <QPointer>
#include <QVariant>

class QNetworkReply;
class QJsonObject;

// Turns a plain-English request into a shell command with Claude, for the
// Ask bar, by one of two routes: the Anthropic API with the user's key (from
// ANTHROPIC_API_KEY, or saved in the system keyring where there is one and
// otherwise in a file only they can read), or the user's own Claude Code,
// which answers on their Claude subscription.
class Assistant : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString keySource READ keySource NOTIFY keyChanged)
    // Where a saved key is kept: "keyring" or "file".
    Q_PROPERTY(QString keyStore READ keyStore NOTIFY keyChanged)
    Q_PROPERTY(QString claudeCode READ claudeCode CONSTANT)
    // "auto", "api", or "claude-code"; auto prefers a key, then Claude Code.
    Q_PROPERTY(QString provider READ provider WRITE setProvider NOTIFY providerChanged)
    // The route ask() will take: "api", "claude-code", or empty when neither works.
    Q_PROPERTY(QString route READ route NOTIFY providerChanged)

public:
    // Without a key file (the app itself) the keyring is reached through
    // secret-tool from the PATH; with one (tests) only through the
    // secretTool given, and not at all when that is empty.
    explicit Assistant(const QString &keyFile = {}, QObject *parent = nullptr, const QString &secretTool = {});

    bool busy() const { return !m_reply.isNull() || m_process.state() != QProcess::NotRunning; }
    QString claudeCode() const { return m_claudeCode; }
    QString provider() const { return m_provider; }
    void setProvider(const QString &provider);
    QString route() const;
    // "environment", "saved", or empty when there's no key.
    QString keySource() const;
    QString keyStore() const;

    Q_INVOKABLE QString saveKey(const QString &key);
    // context: kind (local, ssh, serial), host, user, and recent screen text.
    Q_INVOKABLE void ask(const QString &request, const QVariantMap &context);
    Q_INVOKABLE void cancel();

    static QByteArray requestBody(const QString &request, const QVariantMap &context);
    static QString systemPrompt();
    static QString userMessage(const QString &request, const QVariantMap &context);
    static QJsonObject answerSchema();
    // The arguments for Claude Code's one-shot print mode, with no tools.
    // The request itself isn't among them: other users of the computer can
    // read a program's arguments, so it goes in on standard input.
    static QStringList claudeCodeArguments();
    // command, explanation, and risky from Claude Code's JSON output; or error.
    static QVariantMap parseClaudeCode(int exitCode, const QByteArray &stdoutData, const QByteArray &stderrData);
    static QString findClaudeCode();
    // command, explanation, and risky from a Messages API response; or error.
    static QVariantMap parseResponse(int status, const QByteArray &body);

    static constexpr const char *Model = "claude-opus-5-5";

signals:
    void busyChanged();
    void keyChanged();
    void providerChanged();
    void answered(const QString &command, const QString &explanation, bool risky);
    void failed(const QString &message);

private:
    QString key() const;
    void loadSavedKey();
    void keepInKeyring(const QString &value);
    QString writeKeyFile(const QString &value);
    bool hasKeyring() const;

    QString m_keyFile;
    QString m_secretTool;
    QString m_keyringKey; // as read from, or stored in, the keyring
    QString m_fileKey;    // from the file, where there's no keyring
    QString m_unsavedKey; // on its way into the keyring
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QProcess m_process;
    QString m_claudeCode;
    QString m_provider = "auto";
    void askClaudeCode(const QString &request, const QVariantMap &context);
};
