#include "serialports.h"

#include <QRegularExpression>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <vector>

SerialPorts::SerialPorts(const QString &deviceDirectory, int interval, QObject *parent)
    : QObject(parent), m_directory(deviceDirectory) {
    // Polling a directory listing is cheap and, unlike inotify on /dev, also
    // notices the stable by-id links udev adds a moment after the device.
    m_timer.setInterval(interval);
    connect(&m_timer, &QTimer::timeout, this, &SerialPorts::refresh);
    if (interval > 0)
        m_timer.start();
    refresh();
}

QString SerialPorts::labelFor(const QString &byIdName) {
    QString label = byIdName;
    label.remove(QRegularExpression("^(usb|pci|platform)-"));
    label.remove(QRegularExpression("-if\\d+(-port\\d+)?$"));
    label.replace('_', ' ');
    return label.trimmed();
}

void SerialPorts::refresh() {
    // Names under serial/by-id survive replugging and say what the device is.
    QHash<QString, QString> stable;
    const QDir byId(m_directory + "/serial/by-id");
    for (const QFileInfo &link : byId.entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot))
        stable.insert(QFileInfo(link.canonicalFilePath()).fileName(), link.absoluteFilePath());

    const QDir dir(m_directory);
    QStringList names = dir.entryList({"ttyUSB*", "ttyACM*", "rfcomm*"}, QDir::Files | QDir::System);
    // ttyUSB2 before ttyUSB10: by the name, then the number at its end. Done
    // by hand because QCollator's numeric mode depends on the system locale.
    static const QRegularExpression trailing("^(.*?)(\\d*)$");
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        const auto x = trailing.match(a), y = trailing.match(b);
        if (x.captured(1) != y.captured(1))
            return x.captured(1) < y.captured(1);
        return x.captured(2).toLongLong() < y.captured(2).toLongLong();
    });

    QVariantList devices;
    for (const QString &name : names) {
        const QString path = dir.absoluteFilePath(name);
        const QString byIdPath = stable.value(name);
        const QString issue = problem(path);
        devices << QVariantMap{
            {"path", path},
            {"stablePath", byIdPath.isEmpty() ? path : byIdPath},
            {"label", byIdPath.isEmpty() ? name : labelFor(QFileInfo(byIdPath).fileName())},
            {"accessible", issue.isEmpty()},
            {"problem", issue},
        };
    }
    if (devices != m_devices) {
        m_devices = devices;
        emit devicesChanged();
    }
}

QString SerialPorts::problem(const QString &path) {
    const QByteArray native = QFile::encodeName(path);
    if (::access(native.constData(), F_OK) != 0)
        return tr("%1 is not connected.").arg(path);
    if (::access(native.constData(), R_OK | W_OK) == 0)
        return {};

    struct stat info {};
    if (::stat(native.constData(), &info) != 0)
        return tr("No permission to open %1.").arg(path);
    const group *owner = getgrgid(info.st_gid);
    const QString groupName = owner ? QString::fromLocal8Bit(owner->gr_name) : QString::number(info.st_gid);

    // Group membership is read at login, so someone just added to the group
    // (uucp on Arch, dialout on Debian and Ubuntu) still can't open the port.
    bool listed = false;
    if (const passwd *user = getpwuid(getuid())) {
        int count = 32;
        std::vector<gid_t> groups(count);
        if (getgrouplist(user->pw_name, user->pw_gid, groups.data(), &count) == -1) {
            groups.resize(count);
            getgrouplist(user->pw_name, user->pw_gid, groups.data(), &count);
        }
        groups.resize(count);
        listed = std::find(groups.begin(), groups.end(), info.st_gid) != groups.end();
    }
    std::vector<gid_t> active(getgroups(0, nullptr));
    getgroups(int(active.size()), active.data());
    const bool current = std::find(active.begin(), active.end(), info.st_gid) != active.end();

    if (listed && !current)
        return tr("You were added to the %1 group after logging in. Log out and back in, then open %2 again.")
            .arg(groupName, path);
    if (listed || info.st_gid == 0)
        return tr("No permission to open %1.").arg(path);
    return tr("No permission to open %1. Add yourself to the %2 group with “sudo usermod -aG %2 $USER”, then log out and back in.")
        .arg(path, groupName);
}
