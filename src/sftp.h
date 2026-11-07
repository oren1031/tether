#pragma once

#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QVariant>

// A remote folder seen through sftp, for the file browser pane. Each action
// runs sftp once in batch mode with the host's options; with ssh connection
// sharing it rides the login a terminal already made, so it never asks for a
// password itself.
class SftpBrowser : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path NOTIFY pathChanged)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString downloadFolder READ downloadFolder WRITE setDownloadFolder NOTIFY downloadFolderChanged)

public:
    explicit SftpBrowser(QObject *parent = nullptr);
    ~SftpBrowser() override;

    QString path() const { return m_path; }
    QVariantList entries() const { return m_entries; }
    bool busy() const { return m_process.state() != QProcess::NotRunning; }
    QString error() const { return m_error; }
    QString status() const { return m_status; }
    QString downloadFolder() const { return m_downloadFolder; }
    void setDownloadFolder(const QString &folder);

    // command is Backend::sftpCommand(): program, args, and environment.
    Q_INVOKABLE void start(const QVariantMap &command);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void open(const QString &name);
    Q_INVOKABLE void go(const QString &path);
    Q_INVOKABLE void up();
    Q_INVOKABLE void download(const QStringList &names);
    Q_INVOKABLE void upload(const QVariantList &localFiles);
    Q_INVOKABLE void makeFolder(const QString &name);
    Q_INVOKABLE void remove(const QString &name, bool folder);
    Q_INVOKABLE void rename(const QString &from, const QString &to);
    Q_INVOKABLE void cancel();

    // Quotes an argument for an sftp batch line, globs included.
    static QString quote(const QString &argument);
    // Entries from "ls -lan" output: name, folder, link, size, modified,
    // permissions; folders first, then by name.
    static QVariantList parseListing(const QString &output);
    // The folder sftp reports after "pwd".
    static QString parsePwd(const QString &output);

signals:
    void pathChanged();
    void entriesChanged();
    void busyChanged();
    void errorChanged();
    void statusChanged();
    void downloadFolderChanged();
    void downloaded(const QString &folder, const QStringList &names);

private:
    // Runs lines in the current folder, then lists it again.
    void run(const QStringList &lines, const QString &status, const QStringList &downloadedNames = {});
    void finished(int code, QProcess::ExitStatus exitStatus);
    void setError(const QString &error);
    void setStatus(const QString &status);

    QVariantMap m_command;
    QString m_path;
    QVariantList m_entries;
    QString m_error;
    QString m_status;
    QString m_downloadFolder;
    QStringList m_pendingDownloads;
    QProcess m_process;
};
