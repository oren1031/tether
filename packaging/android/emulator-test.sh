#!/bin/sh
# Installs Tether in a running emulator, runs its self-test, and saves a
# screenshot of it running:  emulator-test.sh <apk> <screenshot.png>
# With SSH_TEST=user@host:port and SSH_TEST_KEY=<private key>, the self-test
# also logs in there with the built-in ssh (the emulator sees its host as 10.0.2.2).
set -eu
apk="$1"; shot="$2"
pkg=io.github.oren1031.tether
activity=org.qtproject.qt.android.bindings.QtActivity

adb install -r "$apk"
arguments="--self-test"
if [ -n "${SSH_TEST:-}" ] && [ -f "${SSH_TEST_KEY:-}" ]; then
    adb push "$SSH_TEST_KEY" /data/local/tmp/tether_test_key >/dev/null
    adb shell chmod 644 /data/local/tmp/tether_test_key
    arguments="$arguments --ssh-test $SSH_TEST /data/local/tmp/tether_test_key"
fi
# Twice at most: a cold emulator sometimes drops the first launch's log.
for attempt in 1 2; do
    adb logcat -c
    adb shell am force-stop "$pkg"
    adb shell am start -W -n "$pkg/$activity" --es applicationArguments "'$arguments'"
    i=0
    while [ $i -lt 60 ] && ! adb logcat -d | grep -q "SELF-TEST \(PASSED\|FAILED\)"; do
        sleep 2
        i=$((i + 1))
    done
    adb logcat -d | grep -q "SELF-TEST \(PASSED\|FAILED\)" && break
done
echo "── self-test ──"
adb logcat -d | grep "tether-self-test:" | sed 's/.*tether-self-test: //' || true
adb logcat -d | grep -q "SELF-TEST PASSED" && self_test_passed=1
echo "── problems in the log ──"
adb logcat -d | grep -E "qrc:/|FATAL EXCEPTION|AndroidRuntime" | head -30 || true

# An update handed to Android's installer: the installer must come up.
adb logcat -c
adb shell am force-stop "$pkg"
adb shell am start -W -n "$pkg/$activity" --es applicationArguments "'--self-test --try-install'"
installer_seen=
i=0
while [ $i -lt 45 ]; do
    if adb shell dumpsys activity activities | grep -iE "ResumedActivity" | grep -qi "packageinstaller"; then
        installer_seen=1
        sleep 2
        adb exec-out screencap -p > "${shot%.png}-installer.png"
        break
    fi
    sleep 1
    i=$((i + 1))
done
adb logcat -d | grep "tether-self-test:" | grep -i "install" | sed 's/.*tether-self-test: //' || true
if [ -n "$installer_seen" ]; then
    echo "ok   Android's installer opened on the update"
else
    echo "FAIL Android's installer didn't open"
    adb shell dumpsys activity activities | grep -iE "ResumedActivity" | head -3
    installer_failed=1
fi
adb shell input keyevent 4
adb shell am force-stop "$pkg"

# Then as people use it, for a screenshot.
adb shell am force-stop "$pkg"
adb shell am start -W -n "$pkg/$activity"
sleep 15
adb exec-out screencap -p > "$shot"

# The phone layout of the big screens.
for screen in settings new-host; do
    adb shell am force-stop "$pkg"
    adb shell am start -W -n "$pkg/$activity" --es applicationArguments "'--open $screen'"
    sleep 8
    adb exec-out screencap -p > "${shot%.png}-$screen.png"
done

# Typing through Android's own input into a live ssh session: connect, tap the
# terminal, answer the host key question, and run a command that leaves a
# file on the server for the workflow to find.
if [ -n "${SSH_TEST:-}" ] && [ -f "${SSH_TEST_KEY:-}" ]; then
    adb shell am force-stop "$pkg"
    adb shell pm clear "$pkg" >/dev/null   # forget the host key, so the question comes
    adb shell am start -W -n "$pkg/$activity" --es applicationArguments \
        "'--connect $SSH_TEST --identity /data/local/tmp/tether_test_key'"
    sleep 12
    size=$(adb shell wm size | sed -n 's/.*: //p' | tail -n 1)
    w=${size%x*}; h=${size#*x}
    adb shell input tap $((w / 2)) $((h / 3))
    sleep 2
    adb exec-out screencap -p > "${shot%.png}-question.png"
    adb shell input text yes
    adb shell input keyevent 66
    sleep 6
    adb shell input text "touch%s/tmp/tether-typed-on-android"
    adb shell input keyevent 66
    sleep 4
    adb exec-out screencap -p > "${shot%.png}-session.png"
fi
if [ -n "${installer_failed:-}" ]; then
    exit 1
fi
if [ -z "${self_test_passed:-}" ]; then
    echo "── the app's log ──"
    adb logcat -d | grep -E "oren1031|libtether|QtActivity|Qt[A-Z]|qt\.|UnsatisfiedLink|dlopen|linker|AndroidRuntime|F DEBUG|E DEBUG|crash_dump|libcrypto|libssh" \
        | grep -v "NamespaceManager\|netd\|MagicTether" | tail -150
    exit 1
fi
