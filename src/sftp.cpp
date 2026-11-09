#include "sftp.h"

#include <QDir>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

SftpBrowser::SftpBrowser(QObject *parent)
    : QObject(parent),
      m_downloadFolder(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)) {
    if (m_downloadFolder.isEmpty())
        m_downloadFolder = QDir::homePath();
    connect(&m_process, &QProcess::finished, this, &SftpBrowser::finished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            setError(tr("sftp couldn't start."));
            emit busyChanged();
        }
    });
}

SftpBrowser::~SftpBrowser() {
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

void SftpBrowser::setDownloadFolder(const QString &folder) {
    if (folder == m_downloadFolder)
        return;
    m_downloadFolder = folder;
    emit downloadFolderChanged();
}

QString SftpBrowser::quote(const QString &argument) {
    QString escaped;
    for (const QChar c : argument) {
        if (c == '\\' || c == '"' || c == '*' || c == '?' || c == '[' || c == ']')
            escaped += '\\';
        escaped += c;
    }
    return '"' + escaped + '"';
}

QString SftpBrowser::parsePwd(const QString &output) {
    static const QRegularExpression pwd(R"(^Remote working directory: (.+)$)", QRegularExpression::MultilineOption);
    const auto match = pwd.match(output);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

QVariantList SftpBrowser::parseListing(const QString &output) {
    static const QRegularExpression line(
        R"(^([dlcbps-][rwxsStT-]{9}\S*)\s+\S+\s+\S+\s+\S+\s+(\d+)\s+(\w{3}\s+\d+\s+[\d:]+)\s+(.+)$)");
    QVariantList entries;
    for (const QString &raw : output.split('\n')) {
        const QString text = raw.trimmed();
        if (text.startsWith("sftp>"))
            continue;
        const auto match = line.match(text);
        if (!match.hasMatch())
            continue;
        const QString perms = match.captured(1);
        QString name = match.captured(4);
        const bool link = perms.startsWith('l');
        if (link && name.contains(" -> "))
            name = name.section(" -> ", 0, 0);
        name = name.section('/', -1);
        if (name == "." || name == ".." || name.isEmpty())
            continue;
        entries << QVariantMap{{"name", name}, {"folder", perms.startsWith('d')}, {"link", link},
                               {"size", match.captured(2).toLongLong()}, {"modified", match.captured(3)},
                               {"permissions", perms.left(10)}};
    }
    std::sort(entries.begin(), entries.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap(), y = b.toMap();
        if (x["folder"].toBool() != y["folder"].toBool())
            return x["folder"].toBool();
        return x["name"].toString().compare(y["name"].toString(), Qt::CaseInsensitive) < 0;
    });
    return entries;
}

void SftpBrowser::start(const QVariantMap &command) {
    if (command.contains("error")) {
        setError(command.value("error").toString());
        return;
    }
    m_command = command;
    m_path.clear();
    emit pathChanged();
    run({}, tr("Connecting…"));
}

void SftpBrowser::refresh() { run({}, tr("Refreshing…")); }

void SftpBrowser::open(const QString &name) {
    go(m_path.endsWith('/') ? m_path + name : m_path + "/" + name);
}

void SftpBrowser::go(const QString &path) {
    if (busy() || m_command.isEmpty())
        return;
    const QString previous = m_path;
    m_path = path;
    run({}, tr("Opening %1…").arg(path));
    m_path = previous;
    m_process.setProperty("target", path);
}

void SftpBrowser::up() {
    if (m_path.isEmpty() || m_path == "/")
        return;
    const QString parent = m_path.section('/', 0, -2);
    go(parent.isEmpty() ? "/" : parent);
}

void SftpBrowser::download(const QStringList &names) {
    if (names.isEmpty())
        return;
    QDir().mkpath(m_downloadFolder);
    QStringList lines{"lcd " + quote(m_downloadFolder)};
    for (const QString &name : names)
        lines << "get -r " + quote(name);
    run(lines, names.size() == 1 ? tr("Downloading %1…").arg(names.first()) : tr("Downloading %n items…", "", names.size()),
        names);
}

void SftpBrowser::upload(const QVariantList &localFiles) {
    QStringList lines;
    for (const QVariant &value : localFiles) {
        const QUrl url = value.toUrl();
        const QString local = url.isLocalFile() ? url.toLocalFile() : value.toString();
        if (!local.isEmpty())
            lines << "put -r " + quote(local);
    }
    if (!lines.isEmpty())
        run(lines, lines.size() == 1 ? tr("Uploading…") : tr("Uploading %n items…", "", lines.size()));
}

void SftpBrowser::makeFolder(const QString &name) {
    if (!name.trimmed().isEmpty())
        run({"mkdir " + quote(name.trimmed())}, tr("Creating %1…").arg(name.trimmed()));
}

void SftpBrowser::remove(const QString &name, bool folder) {
    // sftp only removes empty folders, which keeps a slip from deleting a tree.
    run({(folder ? "rmdir " : "rm ") + quote(name)}, tr("Deleting %1…").arg(name));
}

void SftpBrowser::rename(const QString &from, const QString &to) {
    if (!to.trimmed().isEmpty() && to != from)
        run({"rename " + quote(from) + " " + quote(to.trimmed())}, tr("Renaming…"));
}

void SftpBrowser::cancel() {
    if (busy())
        m_process.kill();
}

void SftpBrowser::run(const QStringList &lines, const QString &status, const QStringList &downloadedNames) {
    if (busy() || m_command.isEmpty())
        return;
    // sftp reads its commands a line at a time, and quoting can't carry a
    // line break: the rest of such a name would be read as the next command,
    // and a line starting with "!" runs a program on this computer.
    QStringList checked = lines;
    checked << m_path;
    for (const QString &line : std::as_const(checked))
        if (line.contains('\n') || line.contains('\r')) {
            setError(tr("Names with a line break in them can't be used here."));
            return;
        }
    QStringList batch;
    if (!m_path.isEmpty())
        batch << "cd " + quote(m_path);
    batch << lines << "pwd" << "ls -lan";

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &pair : m_command.value("environment").toStringList())
        env.insert(pair.section('=', 0, 0), pair.section('=', 1));
    m_process.setProcessEnvironment(env);
    m_process.setProperty("target", QVariant());
    m_pendingDownloads = downloadedNames;
    setError({});
    setStatus(status);
    m_process.start(m_command.value("program").toString(), m_command.value("args").toStringList());
    m_process.write((batch.join('\n') + '\n').toUtf8());
    m_process.closeWriteChannel();
    emit busyChanged();
}

void SftpBrowser::finished(int code, QProcess::ExitStatus exitStatus) {
    const QString out = QString::fromUtf8(m_process.readAllStandardOutput());
    const QString err = QString::fromUtf8(m_process.readAllStandardError()).trimmed();
    emit busyChanged();
    setStatus({});
    if (exitStatus != QProcess::NormalExit) {
        setError(tr("Stopped."));
        return;
    }
    const QString pwd = parsePwd(out);
    if (!pwd.isEmpty() && pwd != m_path) {
        m_path = pwd;
        emit pathChanged();
    }
    if (code != 0 || pwd.isEmpty()) {
        QString message = err.section('\n', -1).trimmed();
        if (message.contains("Permission denied") && m_path.isEmpty())
            message = tr("The server wants a password. Connect to this host in a terminal first, or save the password in the vault.");
        else if (message.contains("Host key verification failed"))
            message = tr("This host's key isn't known yet. Connect to it once in a terminal to accept it.");
        setError(message.isEmpty() ? tr("sftp stopped with code %1.").arg(code) : message);
        if (pwd.isEmpty())
            return;
    }
    m_entries = parseListing(out);
    emit entriesChanged();
    if (code == 0 && !m_pendingDownloads.isEmpty())
        emit downloaded(m_downloadFolder, m_pendingDownloads);
    m_pendingDownloads.clear();
}

void SftpBrowser::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}

void SftpBrowser::setStatus(const QString &status) {
    if (status == m_status)
        return;
    m_status = status;
    emit statusChanged();
}
