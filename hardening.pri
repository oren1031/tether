# Compiler and linker protections for the app and its tests. The terminal
# parses bytes sent by servers in C (libvterm), so these matter most there.
# The Arch package gets the same from makepkg; the AppImage and Windows
# builds get only what is set here.
#
# CONFIG+=tether_sanitize builds with AddressSanitizer and UBSan instead (the
# CI job .github/workflows/ci.yml runs the tests that way); the two don't mix.
tether_sanitize {
    QMAKE_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
    QMAKE_CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
    QMAKE_LFLAGS += -fsanitize=address,undefined
} else:android|macx {
    # Left to their toolchains' defaults.
} else:win32-msvc* {
    # Control Flow Guard: indirect calls may only reach real functions.
    QMAKE_CFLAGS += /guard:cf
    QMAKE_CXXFLAGS += /guard:cf
    QMAKE_LFLAGS += /guard:cf
} else:unix {
    # -U first: makepkg already defines it, and a second definition warns.
    QMAKE_CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection
    QMAKE_CXXFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection
    QMAKE_LFLAGS += -Wl,-z,relro,-z,now
}
