QT += core gui qml quick quickcontrols2 network

CONFIG += c++17 release
VERSION = 0.2.0
# Where updates are published: owner/repository on GitHub. Release builds
# set it from the TETHER_REPO environment variable.
TETHER_REPO = $$(TETHER_REPO)
DEFINES += APP_VERSION=\\\"$$VERSION\\\" TETHER_REPO=\\\"$$TETHER_REPO\\\"
TARGET = tether
TEMPLATE = app
LIBS += -ldl

HEADERS += \
    src/assistant.h \
    src/backend.h \
    src/sftp.h \
    src/updater.h \
    src/serialports.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/assistant.cpp \
    src/backend.cpp \
    src/sftp.cpp \
    src/updater.cpp \
    src/serialports.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
