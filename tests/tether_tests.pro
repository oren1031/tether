QT += core gui quick quickcontrols2 network testlib
CONFIG += c++17 testcase
TARGET = tether_tests
TEMPLATE = app

INCLUDEPATH += ../src

include(../third_party/libvterm/libvterm.pri)
unix:!android:!macx: LIBS += -lutil
HEADERS += ../src/credentials.h ../src/pseudoterminal.h ../src/terminalview.h ../src/updater.h ../src/assistant.h ../src/sftp.h ../src/backend.h ../src/serialports.h ../src/theme.h ../src/translator.h
SOURCES += tether_tests.cpp ../src/credentials.cpp ../src/pseudoterminal.cpp ../src/terminalview.cpp ../src/updater.cpp ../src/assistant.cpp ../src/sftp.cpp ../src/backend.cpp ../src/serialports.cpp ../src/theme.cpp ../src/translator.cpp

# The libssh connection is tested against a throwaway sshd where libssh is installed.
packagesExist(libssh) {
    DEFINES += TETHER_LIBSSH
    HEADERS += ../src/sshsession.h
    SOURCES += ../src/sshsession.cpp
    LIBS += -lssh
}
