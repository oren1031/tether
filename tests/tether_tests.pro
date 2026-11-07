QT += core gui quick quickcontrols2 network testlib
CONFIG += c++17 testcase
TARGET = tether_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/updater.h ../src/assistant.h ../src/sftp.h ../src/backend.h ../src/serialports.h ../src/theme.h ../src/translator.h
SOURCES += tether_tests.cpp ../src/updater.cpp ../src/assistant.cpp ../src/sftp.cpp ../src/backend.cpp ../src/serialports.cpp ../src/theme.cpp ../src/translator.cpp
