#include "assistant.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

Assistant::Assistant(const QString &keyFile, QObject *parent)
    : QObject(parent),
      m_keyFile(!keyFile.isEmpty() ? keyFile
                : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/tether/anthropic-key") {}

QString Assistant::key() const {
    const QString env = qEnvironmentVariable("ANTHROPIC_API_KEY").trimmed();
    if (!env.isEmpty())
        return env;
    QFile file(m_keyFile);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

QString Assistant::keySource() const {
    if (!qEnvironmentVariable("ANTHROPIC_API_KEY").trimmed().isEmpty())
        return "environment";
    return key().isEmpty() ? QString() : "saved";
}

QString Assistant::saveKey(const QString &input) {
    const QString value = input.trimmed();
    if (value.isEmpty()) {
        QFile::remove(m_keyFile);
        emit keyChanged();
        return {};
    }
    if (value.contains(QRegularExpression("\\s")))
        return tr("That doesn't look like an API key.");
    QDir().mkpath(QFileInfo(m_keyFile).absolutePath());
    QSaveFile file(m_keyFile);
    if (!file.open(QIODevice::WriteOnly))
        return tr("Couldn't save the key: %1").arg(file.errorString());
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    file.write(value.toUtf8());
    if (!file.commit())
        return tr("Couldn't save the key: %1").arg(file.errorString());
    emit keyChanged();
    return {};
}

QByteArray Assistant::requestBody(const QString &request, const QVariantMap &context) {
    const QString system =
        "You turn a request into one shell command for the terminal session described in the user's message. "
        "Prefer common, portable tools; use the platform hints in the context when they are given. "
        "When the request can't be done in one command, chain steps with && or a short pipeline. "
        "Never invent hostnames, paths, or values the user didn't give; use an obvious placeholder like <file> "
        "instead. Set risky to true when the command deletes or overwrites data, changes system configuration, "
        "restarts services, or needs root. The explanation is one or two plain sentences.";

    QString message = "Request: " + request.trimmed() + "\n\nSession: ";
    const QString kind = context.value("kind").toString();
    if (kind == "ssh")
        message += QString("ssh to %1%2").arg(context.value("user").toString().isEmpty() ? QString()
                                                  : context.value("user").toString() + "@",
                                              context.value("host").toString());
    else if (kind == "serial")
        message += "a serial console (picocom) on a device, likely an embedded board";
    else
        message += "a local shell on Linux";
    const QString screen = context.value("screen").toString().right(3000).trimmed();
    if (!screen.isEmpty())
        message += "\n\nRecent terminal output:\n<terminal>\n" + screen + "\n</terminal>";

    const QJsonObject schema{
        {"type", "object"},
        {"properties", QJsonObject{{"command", QJsonObject{{"type", "string"}}},
                                   {"explanation", QJsonObject{{"type", "string"}}},
                                   {"risky", QJsonObject{{"type", "boolean"}}}}},
        {"required", QJsonArray{"command", "explanation", "risky"}},
        {"additionalProperties", false}};
    const QJsonObject body{
        {"model", Model},
        {"max_tokens", 16000},
        {"system", system},
        {"fallbacks", "default"},
        {"output_config", QJsonObject{{"effort", "low"},
                                      {"format", QJsonObject{{"type", "json_schema"}, {"schema", schema}}}}},
        {"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", message}}}}};
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QVariantMap Assistant::parseResponse(int status, const QByteArray &body) {
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    if (status != 200) {
        const QString message = root.value("error").toObject().value("message").toString();
        if (status == 401)
            return {{"error", tr("The API key was refused. Check it in Settings → AI.")}};
        if (status == 429)
            return {{"error", tr("Too many requests right now; try again in a moment.")}};
        return {{"error", message.isEmpty() ? tr("The request failed (HTTP %1).").arg(status) : message}};
    }
    const QString stop = root.value("stop_reason").toString();
    if (stop == "refusal")
        return {{"error", tr("Claude declined this request.")}};
    if (stop == "max_tokens")
        return {{"error", tr("The answer was cut off; try a shorter request.")}};
    QString text;
    for (const QJsonValue &block : root.value("content").toArray())
        if (block.toObject().value("type") == "text")
            text += block.toObject().value("text").toString();
    const QJsonObject answer = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString command = answer.value("command").toString().trimmed();
    if (command.isEmpty())
        return {{"error", tr("No command came back.")}};
    return {{"command", command}, {"explanation", answer.value("explanation").toString()},
            {"risky", answer.value("risky").toBool()}};
}

void Assistant::ask(const QString &request, const QVariantMap &context) {
    if (request.trimmed().isEmpty())
        return;
    const QString apiKey = key();
    if (apiKey.isEmpty()) {
        emit failed(tr("Add an Anthropic API key in Settings → AI, or set ANTHROPIC_API_KEY."));
        return;
    }
    cancel();
    QNetworkRequest req(QUrl("https://api.anthropic.com/v1/messages"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("x-api-key", apiKey.toUtf8());
    req.setRawHeader("anthropic-version", "2026-10-21");
    req.setRawHeader("anthropic-beta", "server-side-fallback-2026-07-01");
    req.setTransferTimeout(90000);
    m_reply = m_network.post(req, requestBody(request, context));
    emit busyChanged();
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const bool aborted = reply->error() == QNetworkReply::OperationCanceledError;
        m_reply = nullptr;
        emit busyChanged();
        if (aborted)
            return;
        if (status == 0) {
            emit failed(tr("Couldn't reach the Anthropic API: %1").arg(reply->errorString()));
            return;
        }
        const QVariantMap result = parseResponse(status, body);
        if (result.contains("error"))
            emit failed(result.value("error").toString());
        else
            emit answered(result["command"].toString(), result["explanation"].toString(), result["risky"].toBool());
    });
}

void Assistant::cancel() {
    if (m_reply)
        m_reply->abort();
}
