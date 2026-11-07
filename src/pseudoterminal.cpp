#include "pseudoterminal.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTimer>

#ifdef Q_OS_WIN
// ── Windows: ConPTY ────────────────────────────────────────────────────────
// ConPTY arrived in Windows 10 1809.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <QThread>
#include <QWinEventNotifier>
#include <windows.h>

namespace {

// Reads the program's output on its own thread, since pipe reads block.
class Reader : public QThread {
public:
    Reader(HANDLE pipe, Pty *pty) : m_pipe(pipe), m_pty(pty) {}
    void run() override {
        char buffer[16384];
        DWORD got = 0;
        while (ReadFile(m_pipe, buffer, sizeof buffer, &got, nullptr) && got > 0) {
            const QByteArray data(buffer, int(got));
            QMetaObject::invokeMethod(m_pty, [pty = m_pty, data] { emit pty->dataReceived(data); },
                                      Qt::QueuedConnection);
        }
    }

private:
    HANDLE m_pipe;
    Pty *m_pty;
};

// Quotes one argument the way CommandLineToArgvW reads it back.
QString quoteArgument(const QString &arg) {
    if (!arg.isEmpty() && !arg.contains(QRegularExpression(R"([\s"])")))
        return arg;
    QString out = "\"";
    int backslashes = 0;
    for (const QChar c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out += QString(backslashes * 2 + 1, '\\') + '"';
            backslashes = 0;
        } else {
            out += QString(backslashes, '\\') + c;
            backslashes = 0;
        }
    }
    out += QString(backslashes * 2, '\\') + '"';
    return out;
}

} // namespace

Pty::Pty(QObject *parent) : QObject(parent) {}

Pty::~Pty() {
    terminate();
}

bool Pty::start(const QString &program, const QStringList &args, const QString &workingDirectory,
                const QStringList &environment, int columns, int rows) {
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, nullptr, 0) || !CreatePipe(&outRead, &outWrite, nullptr, 0)) {
        m_error = tr("Couldn't create pipes for the terminal.");
        return false;
    }
    HPCON console = nullptr;
    const COORD size{SHORT(qMax(columns, 2)), SHORT(qMax(rows, 2))};
    if (FAILED(CreatePseudoConsole(size, inRead, outWrite, 0, &console))) {
        m_error = tr("This version of Windows has no pseudo console (ConPTY); Windows 10 1809 or newer is needed.");
        CloseHandle(inRead); CloseHandle(inWrite); CloseHandle(outRead); CloseHandle(outWrite);
        return false;
    }
    // The console holds its own copies of these ends now.
    CloseHandle(inRead);
    CloseHandle(outWrite);

    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    QByteArray attributes(int(attributeSize), 0);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    // Without this, a Tether whose own output is redirected (started from a
    // script, say) passes those handles on, and the program writes there
    // instead of to the terminal.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullptr;
    startup.StartupInfo.hStdOutput = nullptr;
    startup.StartupInfo.hStdError = nullptr;
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeSize);
    UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console,
                              sizeof(HPCON), nullptr, nullptr);

    QString commandLine = quoteArgument(QDir::toNativeSeparators(program));
    for (const QString &arg : args)
        commandLine += ' ' + quoteArgument(arg);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &pair : environment)
        env.insert(pair.section('=', 0, 0), pair.section('=', 1));
    QString block;
    for (const QString &key : env.keys())
        block += key + '=' + env.value(key) + QChar(0);
    block += QChar(0);

    std::wstring line = commandLine.toStdWString();
    const std::wstring directory = QDir::toNativeSeparators(
        workingDirectory.isEmpty() ? QDir::homePath() : workingDirectory).toStdWString();
    PROCESS_INFORMATION info{};
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                                   (void *)block.utf16(), directory.c_str(), &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!ok) {
        m_error = tr("Couldn't start %1 (error %2).").arg(program).arg(GetLastError());
        ClosePseudoConsole(console);
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return false;
    }
    CloseHandle(info.hThread);

    m_console = console;
    m_input = inWrite;
    m_output = outRead;
    m_process = info.hProcess;
    m_running = true;

    m_reader = new Reader(outRead, this);
    m_reader->start();
    m_exitNotifier = new QWinEventNotifier(info.hProcess, this);
    connect(m_exitNotifier, &QWinEventNotifier::activated, this, [this] {
        DWORD code = 0;
        GetExitCodeProcess(m_process, &code);
        // Closing the console ends the reader once the last output is read.
        finish(int(code));
    });
    return true;
}

void Pty::write(const QByteArray &data) {
    if (!m_running || data.isEmpty())
        return;
    DWORD written = 0;
    WriteFile(m_input, data.constData(), DWORD(data.size()), &written, nullptr);
}

void Pty::resize(int columns, int rows) {
    if (m_console)
        ResizePseudoConsole(m_console, COORD{SHORT(qMax(columns, 2)), SHORT(qMax(rows, 2))});
}

void Pty::finish(int exitCode) {
    if (!m_running)
        return;
    m_running = false;
    if (m_exitNotifier)
        m_exitNotifier->setEnabled(false);
    if (m_console) {
        ClosePseudoConsole(m_console);
        m_console = nullptr;
    }
    if (m_reader) {
        m_reader->wait(2000);
        delete m_reader;
        m_reader = nullptr;
    }
    CloseHandle(m_input);
    CloseHandle(m_output);
    CloseHandle(m_process);
    m_input = m_output = m_process = nullptr;
    emit finished(exitCode);
}

void Pty::terminate() {
    if (!m_running)
        return;
    TerminateProcess(m_process, 1);
    finish(1);
}

#else
// ── Linux, Android, macOS: forkpty ─────────────────────────────────────────
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(Q_OS_MACOS)
#include <util.h>
#else
#include <pty.h>
#endif

#include <vector>

extern char **environ;

Pty::Pty(QObject *parent) : QObject(parent) {}

Pty::~Pty() {
    terminate();
}

bool Pty::start(const QString &program, const QStringList &args, const QString &workingDirectory,
                const QStringList &environment, int columns, int rows) {
    // Everything the child needs is built before fork, which is only safe
    // to follow with async-signal-safe calls.
    std::vector<QByteArray> argStorage{QFile::encodeName(program)};
    for (const QString &arg : args)
        argStorage.push_back(arg.toLocal8Bit());
    std::vector<char *> argv;
    for (QByteArray &a : argStorage)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("TERM", "xterm-256color");
    env.insert("COLORTERM", "truecolor");
    env.insert("TERM_PROGRAM", "Tether");
    env.remove("COLUMNS");
    env.remove("LINES");
    for (const QString &pair : environment)
        env.insert(pair.section('=', 0, 0), pair.section('=', 1));
    std::vector<QByteArray> envStorage;
    for (const QString &key : env.keys())
        envStorage.push_back((key + '=' + env.value(key)).toLocal8Bit());
    std::vector<char *> envp;
    for (QByteArray &e : envStorage)
        envp.push_back(e.data());
    envp.push_back(nullptr);

    const QByteArray directory = QFile::encodeName(workingDirectory.isEmpty() ? QDir::homePath() : workingDirectory);
    const QByteArray executable = QFile::encodeName(program);

    struct winsize size{};
    size.ws_col = (unsigned short)qMax(columns, 2);
    size.ws_row = (unsigned short)qMax(rows, 2);

    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &size);
    if (pid < 0) {
        m_error = tr("Couldn't open a terminal: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    if (pid == 0) {
        if (chdir(directory.constData()) != 0)
            (void)!chdir("/");
        // Signals Tether ignores must not stay ignored in the shell.
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        environ = envp.data();
        execvp(executable.constData(), argv.data());
        const char message[] = "tether: couldn't run the program\r\n";
        (void)!::write(STDERR_FILENO, message, sizeof message - 1);
        _exit(127);
    }

    m_pid = pid;
    m_master = master;
    m_running = true;
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    fcntl(master, F_SETFD, FD_CLOEXEC);

    m_readNotifier = new QSocketNotifier(master, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, [this] {
        char buffer[65536];
        for (int rounds = 0; rounds < 16; ++rounds) {
            const ssize_t got = ::read(m_master, buffer, sizeof buffer);
            if (got > 0) {
                emit dataReceived(QByteArray(buffer, int(got)));
                continue;
            }
            if (got < 0 && (errno == EAGAIN || errno == EINTR))
                return;
            // EOF or EIO: the program and everything it started are gone.
            int status = 0;
            pid_t done = waitpid(pid_t(m_pid), &status, WNOHANG);
            if (done == 0) {
                // The terminal closed before the process exited; give it a moment.
                ::usleep(20000);
                done = waitpid(pid_t(m_pid), &status, WNOHANG);
            }
            const int code = done > 0 ? (WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status)) : 0;
            if (done > 0)
                m_pid = -1;
            finish(code);
            return;
        }
    });
    m_writeNotifier = new QSocketNotifier(master, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &Pty::flushWrites);
    return true;
}

void Pty::write(const QByteArray &data) {
    if (!m_running || data.isEmpty())
        return;
    m_pending += data;
    flushWrites();
}

void Pty::flushWrites() {
    while (!m_pending.isEmpty() && m_master >= 0) {
        const ssize_t sent = ::write(m_master, m_pending.constData(), size_t(m_pending.size()));
        if (sent > 0) {
            m_pending.remove(0, int(sent));
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        break; // EAGAIN: wait until the terminal can take more
    }
    if (m_writeNotifier)
        m_writeNotifier->setEnabled(!m_pending.isEmpty());
}

void Pty::resize(int columns, int rows) {
    if (m_master < 0)
        return;
    struct winsize size{};
    size.ws_col = (unsigned short)qMax(columns, 2);
    size.ws_row = (unsigned short)qMax(rows, 2);
    ioctl(m_master, TIOCSWINSZ, &size);
}

void Pty::finish(int exitCode) {
    if (!m_running)
        return;
    m_running = false;
    delete m_readNotifier;
    delete m_writeNotifier;
    m_readNotifier = m_writeNotifier = nullptr;
    if (m_master >= 0) {
        ::close(m_master);
        m_master = -1;
    }
    m_pending.clear();
    emit finished(exitCode);
}

void Pty::terminate() {
    const qint64 pid = m_pid;
    const bool wasRunning = m_running;
    if (pid > 0 && wasRunning)
        ::kill(pid_t(pid), SIGHUP);
    if (wasRunning) {
        m_running = false;
        delete m_readNotifier;
        delete m_writeNotifier;
        m_readNotifier = m_writeNotifier = nullptr;
        if (m_master >= 0) {
            ::close(m_master);
            m_master = -1;
        }
    }
    // Reap it without blocking, now or a little later, so no zombie is left.
    if (pid > 0) {
        m_pid = -1;
        if (waitpid(pid_t(pid), nullptr, WNOHANG) == 0 && QCoreApplication::instance()) {
            QTimer::singleShot(2000, QCoreApplication::instance(), [pid] {
                if (waitpid(pid_t(pid), nullptr, WNOHANG) == 0) {
                    ::kill(pid_t(pid), SIGKILL);
                    waitpid(pid_t(pid), nullptr, 0);
                }
            });
        }
    }
}
#endif
