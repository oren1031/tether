# Tests that drive the interface itself: tst_*.qml here, run by bin/test
# after the unit tests, once as a desktop and once as a touch screen.
QT += core gui qml quick quickcontrols2 network svg qmltest
CONFIG += c++17 qmltestcase
TARGET = tether_ui_tests
TEMPLATE = app

INCLUDEPATH += ../../src

include(../../third_party/libvterm/libvterm.pri)
unix:!android:!macx: LIBS += -lutil
HEADERS += ../../src/credentials.h ../../src/pseudoterminal.h ../../src/terminalview.h ../../src/updater.h \
    ../../src/assistant.h ../../src/sftp.h ../../src/backend.h ../../src/serialports.h ../../src/theme.h \
    ../../src/translator.h ../../src/iconprovider.h
SOURCES += main.cpp ../../src/credentials.cpp ../../src/pseudoterminal.cpp ../../src/terminalview.cpp \
    ../../src/updater.cpp ../../src/assistant.cpp ../../src/sftp.cpp ../../src/backend.cpp \
    ../../src/serialports.cpp ../../src/theme.cpp ../../src/translator.cpp
RESOURCES += ../../src/resources.qrc
