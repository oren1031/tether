#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QVariant>

class QNetworkReply;

// Turns a plain-English request into a shell command with Claude, for the
// Ask bar. The API key comes from ANTHROPIC_API_KEY, or from a key saved in
// a file only the user can read.
class Assistant : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString keySource READ keySource NOTIFY keyChanged)

public:
    explicit Assistant(const QString &keyFile = {}, QObject *parent = nullptr);

    bool busy() const { return !m_reply.isNull(); }
    // "environment", "saved", or empty when there's no key.
    QString keySource() const;

    Q_INVOKABLE QString saveKey(const QString &key);
    // context: kind (local, ssh, serial), host, user, and recent screen text.
    Q_INVOKABLE void ask(const QString &request, const QVariantMap &context);
    Q_INVOKABLE void cancel();

    static QByteArray requestBody(const QString &request, const QVariantMap &context);
    // command, explanation, and risky from a Messages API response; or error.
    static QVariantMap parseResponse(int status, const QByteArray &body);

    static constexpr const char *Model = "claude-opus-5-5";

signals:
    void busyChanged();
    void keyChanged();
    void answered(const QString &command, const QString &explanation, bool risky);
    void failed(const QString &message);

private:
    QString key() const;

    QString m_keyFile;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
};
