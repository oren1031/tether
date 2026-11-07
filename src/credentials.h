#pragma once

#include <QString>

// Passwords in Windows Credential Manager, under "Tether/<id>", for the
// vault on Windows; Linux keeps them in the Secret Service keyring instead.
namespace WinCred {
QString store(const QString &id, const QString &label, const QString &secret); // error or empty
QString lookup(const QString &id);
void remove(const QString &id);
// ssh's askpass, answered by tether.exe itself: prints the vault's secret
// for password and passphrase prompts. Returns the exit code.
int askpass(const QString &prompt);
}
