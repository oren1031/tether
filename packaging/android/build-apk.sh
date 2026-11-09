#!/bin/sh
# Builds a signed Tether APK:  build-apk.sh <abi> <output.apk>
# Needs QT_ROOT_DIR (Qt for Android), ANDROID_SDK_ROOT, and ANDROID_NDK_ROOT.
# Signs with KEYSTORE (base64 PKCS#12, alias "tether") and KEYSTORE_PASSWORD
# when set, so each version installs over the last; else with a throwaway key,
# unless TETHER_REQUIRE_KEY is set (tagged releases), which makes that an error.
# TETHER_TEST_HOOKS=1 builds the CI test version, the only one with the
# self-test and the test switches (--self-test, --ssh-test, --try-install).
set -eu
abi="$1"; out="$2"
root=$(cd "$(dirname "$0")/../.." && pwd)
version=$(sed -n 's/^VERSION = //p' "$root/tether.pro")
set -- $(echo "$version" | tr . ' ')
code=$(( $1 * 10000 + $2 * 100 + ${3:-0} ))

build="$root/build-android-$abi"
mkdir -p "$build"
cd "$build"
"$root/packaging/android/prepare.sh" "$QT_ROOT_DIR" "$build/package" "$root/pkgbuild/tether.svg"
# libssh for ssh connections, built once per ABI and cached.
libssh="${LIBSSH_ROOT:-$HOME/.cache/tether-libssh}/$abi"
"$root/packaging/android/build-libssh.sh" "$abi" "$libssh"
"$QT_ROOT_DIR/bin/qmake" "$root/tether.pro" ANDROID_ABIS="$abi" ANDROID_PACKAGE_SOURCE_DIR="$build/package" \
    ANDROID_VERSION_NAME="$version" ANDROID_VERSION_CODE="$code" LIBSSH_DIR="$libssh" \
    ${TETHER_TEST_HOOKS:+CONFIG+=tether_test}
make -j"$(nproc)"
make INSTALL_ROOT="$build/android-build" install

if [ -n "${KEYSTORE:-}" ]; then
    echo "$KEYSTORE" | base64 -d > release.p12
    password="$KEYSTORE_PASSWORD"
else
    if [ -n "${TETHER_REQUIRE_KEY:-}" ]; then
        echo "A release needs the signing key: KEYSTORE and KEYSTORE_PASSWORD aren't set." >&2
        exit 1
    fi
    echo "No release key given; signing with a throwaway key."
    password=throwaway
    rm -f release.p12
    keytool -genkeypair -keystore release.p12 -storetype pkcs12 -alias tether -keyalg RSA -keysize 2048 \
        -validity 3650 -dname "CN=Tether" -storepass "$password" -keypass "$password"
fi
deploy=$(find "$(dirname "$QT_ROOT_DIR")" "$RUNNER_WORKSPACE/Qt" -name androiddeployqt -type f 2>/dev/null | head -n 1)
"$deploy" --input android-tether-deployment-settings.json --output android-build \
    --android-platform android-35 --gradle --release \
    --sign release.p12 tether --storetype pkcs12 --storepass "$password" --keypass "$password"
rm -f release.p12
apk=$(find android-build -name '*-release-signed.apk' | head -n 1)
[ -n "$apk" ] || apk=$(find android-build -name '*.apk' | head -n 1)
cp "$apk" "$out"
echo "Built $out ($version, code $code, $abi)"
