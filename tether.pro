QT += core gui qml quick quickcontrols2 network

CONFIG += c++17 release
VERSION = 0.2.2
# Where updates are published: owner/repository on GitHub. Forks can set
# the TETHER_REPO environment variable to publish their own.
TETHER_REPO = $$(TETHER_REPO)
isEmpty(TETHER_REPO): TETHER_REPO = oren1031/tether
DEFINES += APP_VERSION=\\\"$$VERSION\\\" TETHER_REPO=\\\"$$TETHER_REPO\\\"
TARGET = tether
TEMPLATE = app
include(third_party/libvterm/libvterm.pri)
# forkpty lives in libutil on glibc before 2.34; Android has it in libc.
unix:!android:!macx: LIBS += -lutil
win32 {
    LIBS += -ladvapi32
    DESTDIR = $$OUT_PWD/bin
    RC_ICONS = pkgbuild/tether.ico
    QMAKE_TARGET_PRODUCT = Tether
    QMAKE_TARGET_DESCRIPTION = SSH hosts and serial ports in tabs and split panes
}
# Opening serial ports without picocom, on Windows and Android, and on Linux
# when Qt Serial Port is installed.
qtHaveModule(serialport) {
    QT += serialport
    DEFINES += TETHER_QTSERIAL
}

HEADERS += \
    src/assistant.h \
    src/backend.h \
    src/pseudoterminal.h \
    src/sftp.h \
    src/terminalview.h \
    src/updater.h \
    src/credentials.h \
    src/selftest.h \
    src/serialports.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/assistant.cpp \
    src/backend.cpp \
    src/pseudoterminal.cpp \
    src/sftp.cpp \
    src/terminalview.cpp \
    src/updater.cpp \
    src/credentials.cpp \
    src/selftest.cpp \
    src/serialports.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
