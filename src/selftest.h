#pragma once

#include <QStringList>

class QQmlApplicationEngine;

// tether --self-test: checks the system-specific parts (the terminal, the
// vault's password store, ssh's askpass) and that the interface loads without
// errors, prints a report, and optionally saves a screenshot.
void startSelfTest();
int finishSelfTest(QQmlApplicationEngine &engine, const QStringList &arguments);
