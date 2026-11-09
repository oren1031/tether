#include "assistant.h"

#include "credentials.h"

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
#include <QTimer>

#include <functional>

namespace {

// The keyring entry, beside the vault's: service "tether", this identity.
const QString keyringId = QStringLiteral("anthropic-api-key");

// secret-tool, without waiting for it: a locked keyring asks to be unlocked.
void secretTool(QObject *owner, const QString &program, const QStringList &args, const QByteArray &input,
                const std::function<void(bool, const QByteArray &)> &done) {
    auto *process = new QProcess(owner);
    auto *limit = new QTimer(process);
    limit->setSingleShot(true);
    QObject::connect(limit, &QTimer::timeout, process, &QProcess::kill);
    auto report = [process, done](bool ok) {
        if (process->property("reported").toBool())
            return;
        process->setProperty("reported", true);
        const QByteArray out = process->readAllStandardOutput();
        process->deleteLater();
        done(ok, out);
    };
    QObject::connect(process, &QProcess::finished, owner, [report](int code, QProcess::ExitStatus status) {
        report(status == QProcess::NormalExit && code == 0);
    });
    QObject::connect(process, &QProcess::errorOccurred, owner, [report](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            report(false);
    });
    process->start(program, args);
    if (!input.isEmpty())
        process->write(input);
    process->closeWriteChannel();
    limit->start(30000);
}

} // namespace

Assistant::Assistant(const QString &keyFile, QObject *parent, const QString &secretTool)
    : QObject(parent),
      m_keyFile(!keyFile.isEmpty() ? keyFile
                : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/tether/anthropic-key"),
      m_secretTool(keyFile.isEmpty() ? QStandardPaths::findExecutable("secret-tool") : secretTool),
      m_claudeCode(findClaudeCode()) {
    loadSavedKey();
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const QByteArray out = m_process.readAllStandardOutput(), err = m_process.readAllStandardError();
        emit busyChanged();
        if (status != QProcess::NormalExit)
            return; // cancelled
        const QVariantMap result = parseClaudeCode(code, out, err);
        if (result.contains("error"))
            emit failed(result.value("error").toString());
        else
            emit answered(result["command"].toString(), result["explanation"].toString(), result["risky"].toBool());
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            emit busyChanged();
            emit failed(tr("Claude Code couldn't start."));
        }
    });
    // Claude Code can take a while to start; give up after two minutes.
    connect(&m_process, &QProcess::started, this, [this] {
        QTimer::singleShot(120000, &m_process, [this] {
            if (m_process.state() != QProcess::NotRunning) {
                m_process.kill();
                emit failed(tr("Claude Code took too long to answer."));
            }
        });
    });
}

void Assistant::setProvider(const QString &provider) {
    const QString p = provider == "api" || provider == "claude-code" ? provider : QString("auto");
    if (p == m_provider)
        return;
    m_provider = p;
    emit providerChanged();
}

QString Assistant::route() const {
    const bool api = !key().isEmpty(), code = !m_claudeCode.isEmpty();
    if (m_provider == "api")
        return api ? "api" : QString();
    if (m_provider == "claude-code")
        return code ? "claude-code" : QString();
    return api ? "api" : code ? "claude-code" : QString();
}

bool Assistant::hasKeyring() const {
#ifdef Q_OS_WIN
    return true; // Windows Credential Manager
#else
    return !m_secretTool.isEmpty();
#endif
}

// The saved key lives in the keyring, like the vault's passwords. A key an
// older Tether left in its file moves there, and the file goes once the
// keyring has it; the file stays the place only where there's no keyring.
void Assistant::loadSavedKey() {
    QFile file(m_keyFile);
    if (file.open(QIODevice::ReadOnly))
        m_fileKey = QString::fromUtf8(file.readAll()).trimmed();
    file.close();
    if (!hasKeyring())
        return;
#ifdef Q_OS_WIN
    m_keyringKey = Credentials::lookup(keyringId);
    if (!m_fileKey.isEmpty())
        keepInKeyring(m_keyringKey.isEmpty() ? m_fileKey : m_keyringKey);
#else
    secretTool(this, m_secretTool, {"lookup", "service", "tether", "identity", keyringId}, {},
               [this](bool ok, const QByteArray &out) {
                   if (!m_unsavedKey.isEmpty() || !m_keyringKey.isEmpty())
                       return; // a key was saved meanwhile
                   const QString found = ok ? QString::fromUtf8(out).trimmed() : QString();
                   if (!found.isEmpty()) {
                       m_keyringKey = found;
                       m_fileKey.clear();
                       QFile::remove(m_keyFile);
                   } else if (!m_fileKey.isEmpty()) {
                       keepInKeyring(m_fileKey);
                   }
                   emit keyChanged();
                   emit providerChanged();
               });
#endif
}

// Stores the key in the keyring; if the keyring won't take it, in the file.
void Assistant::keepInKeyring(const QString &value) {
    auto stored = [this, value](bool ok) {
        if (m_unsavedKey != value)
            return; // replaced or removed while this one was being stored
        m_unsavedKey.clear();
        if (ok) {
            m_keyringKey = value;
            m_fileKey.clear();
            QFile::remove(m_keyFile);
        } else {
            writeKeyFile(value);
        }
        emit keyChanged();
        emit providerChanged();
    };
    m_unsavedKey = value;
#ifdef Q_OS_WIN
    stored(Credentials::store(keyringId, "Tether: Anthropic API key", value).isEmpty());
#else
    secretTool(this, m_secretTool, {"store", "--label=Tether: Anthropic API key", "service", "tether", "identity", keyringId},
               value.toUtf8(), [stored](bool ok, const QByteArray &) { stored(ok); });
#endif
}

QString Assistant::writeKeyFile(const QString &value) {
    QDir().mkpath(QFileInfo(m_keyFile).absolutePath());
    QSaveFile file(m_keyFile);
    if (!file.open(QIODevice::WriteOnly))
        return tr("Couldn't save the key: %1").arg(file.errorString());
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    file.write(value.toUtf8());
    if (!file.commit())
        return tr("Couldn't save the key: %1").arg(file.errorString());
    m_fileKey = value;
    return {};
}

QString Assistant::key() const {
    const QString env = qEnvironmentVariable("ANTHROPIC_API_KEY").trimmed();
    if (!env.isEmpty())
        return env;
    return !m_unsavedKey.isEmpty() ? m_unsavedKey : !m_keyringKey.isEmpty() ? m_keyringKey : m_fileKey;
}

QString Assistant::keySource() const {
    if (!qEnvironmentVariable("ANTHROPIC_API_KEY").trimmed().isEmpty())
        return "environment";
    return key().isEmpty() ? QString() : "saved";
}

QString Assistant::keyStore() const {
    if (keySource() != "saved")
        return {};
    return !m_unsavedKey.isEmpty() || !m_keyringKey.isEmpty() ? "keyring" : "file";
}

QString Assistant::saveKey(const QString &input) {
    const QString value = input.trimmed();
    if (value.isEmpty()) {
        const bool inKeyring = !m_keyringKey.isEmpty() || !m_unsavedKey.isEmpty();
        m_keyringKey.clear();
        m_unsavedKey.clear();
        m_fileKey.clear();
        QFile::remove(m_keyFile);
        if (inKeyring) {
#ifdef Q_OS_WIN
            Credentials::remove(keyringId);
#else
            secretTool(this, m_secretTool, {"clear", "service", "tether", "identity", keyringId}, {},
                       [](bool, const QByteArray &) {});
#endif
        }
        emit keyChanged();
        emit providerChanged();
        return {};
    }
    if (value.contains(QRegularExpression("\\s")))
        return tr("That doesn't look like an API key.");
    if (hasKeyring()) {
        keepInKeyring(value);
    } else if (const QString error = writeKeyFile(value); !error.isEmpty()) {
        return error;
    }
    emit keyChanged();
    emit providerChanged();
    return {};
}

QString Assistant::systemPrompt() {
    return "You turn a request into one shell command for the terminal session described in the user's message. "
           "Prefer common, portable tools; use the platform hints in the context when they are given. "
           "When the request can't be done in one command, chain steps with && or a short pipeline. "
           "Never invent hostnames, paths, or values the user didn't give; use an obvious placeholder like <file> "
           "instead. Set risky to true when the command deletes or overwrites data, changes system configuration, "
           "restarts services, or needs root. The explanation is one or two plain sentences.";
}

QString Assistant::userMessage(const QString &request, const QVariantMap &context) {
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
    return message;
}

QJsonObject Assistant::answerSchema() {
    return QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{{"command", QJsonObject{{"type", "string"}}},
                                   {"explanation", QJsonObject{{"type", "string"}}},
                                   {"risky", QJsonObject{{"type", "boolean"}}}}},
        {"required", QJsonArray{"command", "explanation", "risky"}},
        {"additionalProperties", false}};
}

QByteArray Assistant::requestBody(const QString &request, const QVariantMap &context) {
    const QJsonObject body{
        {"model", Model},
        {"max_tokens", 16000},
        {"system", systemPrompt()},
        {"fallbacks", "default"},
        {"output_config", QJsonObject{{"effort", "low"},
                                      {"format", QJsonObject{{"type", "json_schema"}, {"schema", answerSchema()}}}}},
        {"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", userMessage(request, context)}}}}};
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

// ── Claude Code ────────────────────────────────────────────────────────────

QString Assistant::findClaudeCode() {
    const QString found = QStandardPaths::findExecutable("claude");
    if (!found.isEmpty())
        return found;
    // Apps started from a launcher may not have the shell's PATH, so look
    // where Claude Code's installers put it.
    const QString home = QDir::homePath();
    for (const QString &path : {home + "/.local/bin/claude", home + "/.claude/local/claude",
                                home + "/.local/share/mise/command-wrappers/bin/claude",
                                home + "/.local/share/mise/shims/claude", QString("/usr/local/bin/claude"),
                                QString("/usr/bin/claude"), QString("/opt/homebrew/bin/claude")})
        if (QFileInfo(path).isExecutable())
            return path;
    return {};
}

QStringList Assistant::claudeCodeArguments() {
    // One turn, no tools, no MCP servers, nothing saved: just the answer.
    // With no prompt among the arguments, print mode reads it from standard
    // input, where the process list doesn't show it.
    return {"-p",
            "--output-format", "json",
            "--json-schema", QString::fromUtf8(QJsonDocument(answerSchema()).toJson(QJsonDocument::Compact)),
            "--system-prompt", systemPrompt(),
            "--tools", "",
            "--strict-mcp-config",
            "--no-session-persistence",
            "--effort", "low"};
}

QVariantMap Assistant::parseClaudeCode(int exitCode, const QByteArray &stdoutData, const QByteArray &stderrData) {
    const QJsonObject root = QJsonDocument::fromJson(stdoutData).object();
    if (root.isEmpty()) {
        QString why = QString::fromUtf8(stderrData).trimmed().section('\n', -1);
        if (why.isEmpty())
            why = tr("Claude Code stopped with code %1.").arg(exitCode);
        if (why.contains("login", Qt::CaseInsensitive) || why.contains("auth", Qt::CaseInsensitive))
            why = tr("Claude Code isn't signed in. Run claude in a terminal and log in with your Claude account.");
        return {{"error", why}};
    }
    if (root.value("is_error").toBool() || root.value("subtype").toString() != "success") {
        const QString result = root.value("result").toString().trimmed();
        return {{"error", result.isEmpty() ? tr("Claude Code couldn't answer.") : result}};
    }
    QJsonObject answer = root.value("structured_output").toObject();
    if (answer.isEmpty())
        answer = QJsonDocument::fromJson(root.value("result").toString().toUtf8()).object();
    const QString command = answer.value("command").toString().trimmed();
    if (command.isEmpty())
        return {{"error", tr("No command came back.")}};
    return {{"command", command}, {"explanation", answer.value("explanation").toString()},
            {"risky", answer.value("risky").toBool()}};
}

void Assistant::askClaudeCode(const QString &request, const QVariantMap &context) {
    if (m_process.state() != QProcess::NotRunning)
        return;
    m_process.setWorkingDirectory(QDir::tempPath());
    m_process.start(m_claudeCode, claudeCodeArguments());
    m_process.write(userMessage(request, context).toUtf8());
    m_process.closeWriteChannel();
    emit busyChanged();
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
    cancel();
    const QString how = route();
    if (how == "claude-code") {
        askClaudeCode(request, context);
        return;
    }
    const QString apiKey = key();
    if (how.isEmpty() || apiKey.isEmpty()) {
        emit failed(m_provider == "claude-code"
                        ? tr("Claude Code isn't installed. Install it from claude.com/code, then log in with your Claude account.")
                        : tr("Add an Anthropic API key in Settings → AI, or use your Claude subscription through Claude Code."));
        return;
    }
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
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}
