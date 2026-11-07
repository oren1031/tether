# libvterm, built into Tether so every platform has the same terminal.
LIBVTERM = $$PWD
INCLUDEPATH += $$LIBVTERM/include
QMAKE_CFLAGS += -std=c99
win32-msvc*: QMAKE_CFLAGS -= -std=c99
win32-msvc*: QMAKE_CFLAGS += /std:c11
SOURCES += \
    $$LIBVTERM/src/encoding.c \
    $$LIBVTERM/src/keyboard.c \
    $$LIBVTERM/src/mouse.c \
    $$LIBVTERM/src/parser.c \
    $$LIBVTERM/src/pen.c \
    $$LIBVTERM/src/screen.c \
    $$LIBVTERM/src/state.c \
    $$LIBVTERM/src/unicode.c \
    $$LIBVTERM/src/vterm.c
HEADERS += $$LIBVTERM/include/vterm.h $$LIBVTERM/include/vterm_keycodes.h
# Third-party code: its warnings are upstream's to fix.
QMAKE_CFLAGS_WARN_ON =
