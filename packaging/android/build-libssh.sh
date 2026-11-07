#!/bin/sh
# Builds libssh for Android, with OpenSSL from KDAB's android_openssl (the
# builds Qt recommends), both as static libraries so nothing extra ships in
# the APK:  build-libssh.sh <abi> <install dir>
# Needs ANDROID_NDK_ROOT, cmake, and ninja.
set -eu
abi="$1"; out="$2"
libssh_version=0.12.2
libssh_sha256=49560f677d96e3706a904ac2de1116e25f3680937d51e5c92198fcba4a1c1e9f
openssl_commit=b71f1470962019bd89534a2919f5925f93bc5779

if [ -f "$out/lib/libssh.a" ] && [ -f "$out/lib/libcrypto.a" ]; then
    echo "libssh for $abi is already in $out"
    exit 0
fi
work="${RUNNER_TEMP:-/tmp}/tether-libssh-$abi"
mkdir -p "$work"
cd "$work"

if [ ! -d android_openssl/ssl_3 ]; then
    git init -q android_openssl
    git -C android_openssl remote add origin https://github.com/KDAB/android_openssl.git
    git -C android_openssl fetch -q --depth 1 origin "$openssl_commit"
    git -C android_openssl checkout -q FETCH_HEAD
fi
ssl="$work/android_openssl/ssl_3/$abi"

curl -fsSLO "https://www.libssh.org/files/${libssh_version%.*}/libssh-$libssh_version.tar.xz"
echo "$libssh_sha256  libssh-$libssh_version.tar.xz" | sha256sum -c -
tar xf "libssh-$libssh_version.tar.xz"

cmake -S "libssh-$libssh_version" -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-28 \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$out" \
    -DBUILD_SHARED_LIBS=OFF -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF -DCLIENT_TESTING=OFF \
    -DWITH_GSSAPI=OFF -DWITH_SERVER=OFF -DWITH_ZLIB=OFF -DWITH_PCAP=OFF -DWITH_NACL=OFF \
    -DOPENSSL_ROOT_DIR="$ssl" -DOPENSSL_INCLUDE_DIR="$ssl/include" \
    -DOPENSSL_CRYPTO_LIBRARY="$ssl/libcrypto.a" -DOPENSSL_SSL_LIBRARY="$ssl/libssl.a" \
    -DOPENSSL_USE_STATIC_LIBS=ON
cmake --build build -j"$(nproc)"
cmake --install build
cp "$ssl/libcrypto.a" "$out/lib/"
ls -la "$out/lib"
