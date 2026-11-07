#pragma once

#include <QByteArray>
#include <QObject>
#include <QStringList>

class QSocketNotifier;

// A program running in a pseudo-terminal: forkpty on Linux and Android,
// ConPTY on Windows. Bytes written go to the program's input; what it
// prints arrives through dataReceived.
class Pty : public QObject {
    Q_OBJECT

public:
    explicit Pty(QObject *parent = nullptr);
    ~Pty() override;

    // environment holds NAME=value pairs added to Tether's own.
    bool start(const QString &program, const QStringList &args, const QString &workingDirectory,
               const QStringList &environment, int columns, int rows);
    void write(const QByteArray &data);
    void resize(int columns, int rows);
    void terminate();
    bool running() const { return m_running; }
    QString error() const { return m_error; }

signals:
    void dataReceived(const QByteArray &data);
    void finished(int exitCode);

private:
    void finish(int exitCode);

    bool m_running = false;
    QString m_error;

#ifdef Q_OS_WIN
    void *m_console = nullptr;   // HPCON
    void *m_input = nullptr;     // our end of the program's input pipe
    void *m_output = nullptr;    // our end of the program's output pipe
    void *m_process = nullptr;
    class QThread *m_reader = nullptr;
    class QWinEventNotifier *m_exitNotifier = nullptr;
#else
    void flushWrites();

    int m_master = -1;
    qint64 m_pid = -1;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    QByteArray m_pending;
#endif
};
