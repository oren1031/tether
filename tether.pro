QT += core gui qml quick quickcontrols2 network svg

CONFIG += c++17 release
VERSION = 0.5.2
# Where updates are published: owner/repository on GitHub. Forks can set
# the TETHER_REPO environment variable to publish their own.
TETHER_REPO = $$(TETHER_REPO)
isEmpty(TETHER_REPO): TETHER_REPO = oren1031/tether
# A server of your own with the releases, checked before GitHub (see
# bin/mirror-release); TETHER_MIRROR in the environment overrides it.
TETHER_MIRROR = $$(TETHER_MIRROR)
isEmpty(TETHER_MIRROR): TETHER_MIRROR = https://tether.orenamir.com
DEFINES += APP_VERSION=\\\"$$VERSION\\\" TETHER_REPO=\\\"$$TETHER_REPO\\\" TETHER_MIRROR=\\\"$$TETHER_MIRROR\\\"
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
# ssh without an ssh program, on Android, through libssh (LIBSSH_DIR points at
# a build of it there). CONFIG+=tether_libssh tries it on other systems.
android|tether_libssh {
    DEFINES += TETHER_LIBSSH
    HEADERS += src/sshsession.h
    SOURCES += src/sshsession.cpp
    isEmpty(LIBSSH_DIR) {
        LIBS += -lssh
    } else {
        # Static libssh with KDAB's OpenSSL (packaging/android/build-libssh.sh).
        INCLUDEPATH += $$LIBSSH_DIR/include
        DEFINES += LIBSSH_STATIC
        LIBS += $$LIBSSH_DIR/lib/libssh.a -L$$LIBSSH_DIR/lib -lcrypto_3
        android: ANDROID_EXTRA_LIBS += $$LIBSSH_DIR/lib/libcrypto_3.so $$LIBSSH_DIR/lib/libssl_3.so
    }
}
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
