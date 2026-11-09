#include "credentials.h"

#include <QObject>

#ifdef Q_OS_WIN
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>
#include <windows.h>
#include <wincred.h>

namespace Credentials {

static std::wstring target(const QString &id) { return ("Tether/" + id).toStdWString(); }

QString store(const QString &id, const QString &label, const QString &secret) {
    const std::wstring name = target(id), comment = label.toStdWString();
    const QByteArray blob = secret.toUtf8();
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(name.c_str());
    credential.Comment = const_cast<LPWSTR>(comment.c_str());
    credential.CredentialBlobSize = DWORD(blob.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(blob.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&credential, 0))
        return QObject::tr("Windows Credential Manager didn't take the password (error %1).").arg(GetLastError());
    return {};
}

QString lookup(const QString &id) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target(id).c_str(), CRED_TYPE_GENERIC, 0, &credential))
        return {};
    const QString secret = QString::fromUtf8(reinterpret_cast<const char *>(credential->CredentialBlob),
                                             int(credential->CredentialBlobSize));
    CredFree(credential);
    return secret;
}

void remove(const QString &id) { CredDeleteW(target(id).c_str(), CRED_TYPE_GENERIC, 0); }

// ssh's "trust this host?" question, shown as a window with the fingerprint
// ssh printed; No is the button Enter presses. ssh takes "yes" or "no".
static int askTrust(const QString &prompt) {
    const std::wstring text = prompt.trimmed().toStdWString();
    const int choice = MessageBoxW(nullptr, text.c_str(), L"Tether",
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND | MB_TOPMOST);
    std::fputs(choice == IDYES ? "yes\n" : "no\n", stdout);
    std::fflush(stdout);
    return 0;
}

int askpass(const QString &prompt) {
    const QString id = qEnvironmentVariable("TETHER_IDENTITY");
    const QString lower = prompt.toLower();
    // A host key ssh hasn't seen: the user decides, with the fingerprint in
    // front of them. (A changed key isn't asked about; ssh refuses it.)
    if (lower.contains("(yes/no"))
        return askTrust(prompt);
    if (id.isEmpty() || !(lower.contains("password") || lower.contains("passphrase") || lower.contains("pin")))
        return 1;
    // Once per login: a wrong saved password mustn't be tried until the
    // server locks the account.
    const QString mark = QDir::tempPath() + "/tether-askpass-" + id;
    const QFileInfo info(mark);
    if (info.exists() && info.lastModified().secsTo(QDateTime::currentDateTime()) < 20)
        return 1;
    QFile touch(mark);
    if (touch.open(QIODevice::WriteOnly))
        touch.close();
    const QString secret = lookup(id);
    if (secret.isEmpty())
        return 1;
    std::fputs(secret.toUtf8().constData(), stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
    return 0;
}

} // namespace Credentials

#else
namespace Credentials {
QString store(const QString &, const QString &, const QString &) { return QStringLiteral("Not on Windows."); }
QString lookup(const QString &) { return {}; }
void remove(const QString &) {}
int askpass(const QString &) { return 1; }
}
#endif
