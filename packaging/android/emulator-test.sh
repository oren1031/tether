#!/bin/sh
# Installs Tether in a running emulator, runs its self-test, and saves a
# screenshot of it running:  emulator-test.sh <apk> <screenshot.png>
set -eu
apk="$1"; shot="$2"
pkg=io.github.oren1031.tether
activity=org.qtproject.qt.android.bindings.QtActivity

adb install -r "$apk"
adb logcat -c
adb shell am start -W -n "$pkg/$activity" --es applicationArguments "--self-test"
i=0
while [ $i -lt 60 ] && ! adb logcat -d | grep -q "SELF-TEST \(PASSED\|FAILED\)"; do
    sleep 2
    i=$((i + 1))
done
echo "── self-test ──"
adb logcat -d | grep "tether-self-test:" | sed 's/.*tether-self-test: //' || true
echo "── problems in the log ──"
adb logcat -d | grep -E "qrc:/|FATAL EXCEPTION|AndroidRuntime" | head -30 || true

# Then as people use it, for a screenshot.
adb shell am force-stop "$pkg"
adb shell am start -W -n "$pkg/$activity"
sleep 15
adb exec-out screencap -p > "$shot"
if ! adb logcat -d | grep -q "SELF-TEST PASSED"; then
    echo "── the app's log ──"
    adb logcat -d | grep -iE "tether|qt\.|libtether|QtActivity|DEBUG|crash" | tail -120
    exit 1
fi
