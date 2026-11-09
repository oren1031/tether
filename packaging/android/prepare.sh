#!/bin/sh
# Makes the Android package sources from Qt's own templates, with Tether's
# name, id, and icon:  prepare.sh <Qt for Android dir> <output dir> <icon.svg>
set -eu
qt="$1"; out="$2"; svg="$3"
templates="$qt/src/android/templates"
[ -f "$templates/AndroidManifest.xml" ] || { echo "no Android templates in $templates" >&2; exit 1; }
mkdir -p "$out"
cp "$templates/AndroidManifest.xml" "$out/"
manifest="$out/AndroidManifest.xml"

# The id phones know the app by, its name, and its icon.
if grep -q 'package="' "$manifest"; then
    sed -i 's/package="[^"]*"/package="io.github.oren1031.tether"/' "$manifest"
else
    sed -i 's/<manifest /<manifest package="io.github.oren1031.tether" /' "$manifest"
fi
sed -i 's/android:label="[^"]*"/android:label="Tether"/g' "$manifest"
if grep -q 'android:icon="' "$manifest"; then
    sed -i 's#android:icon="[^"]*"#android:icon="@mipmap/ic_launcher"#' "$manifest"
else
    sed -i 's#<application #<application android:icon="@mipmap/ic_launcher" #' "$manifest"
fi

# Updates download inside the app and go to Android's installer.
# (Qt's template has "<application" alone on its line.)
grep -q REQUEST_INSTALL_PACKAGES "$manifest" || \
    sed -i 's#^\([[:space:]]*\)<application\b#\1<uses-permission android:name="android.permission.REQUEST_INSTALL_PACKAGES"/>\n\1<application#' "$manifest"
grep -q REQUEST_INSTALL_PACKAGES "$manifest" || { echo "couldn't add the install permission" >&2; exit 1; }

# The window always makes room for the on-screen keyboard. Left to itself Qt
# picks between that and sliding the window up, by the size of the field.
grep -q windowSoftInputMode "$manifest" || \
    sed -i 's#android:launchMode="singleTop"#android:launchMode="singleTop"\n            android:windowSoftInputMode="adjustResize"#' "$manifest"
grep -q 'windowSoftInputMode="adjustResize"' "$manifest" || { echo "couldn't set the keyboard mode" >&2; exit 1; }

# No backups: keys picked with "Choose..." are copied into the app's files,
# and Google's backup and phone-to-phone transfer would carry them off.
if grep -q 'android:allowBackup="' "$manifest"; then
    sed -i 's/android:allowBackup="[^"]*"/android:allowBackup="false"/' "$manifest"
else
    sed -i 's#<application\b#<application android:allowBackup="false"#' "$manifest"
fi
grep -q 'android:allowBackup="false"' "$manifest" || { echo "couldn't turn backups off" >&2; exit 1; }

for size in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
    dir="$out/res/mipmap-${size%%:*}"
    mkdir -p "$dir"
    rsvg-convert -w "${size##*:}" -h "${size##*:}" "$svg" -o "$dir/ic_launcher.png"
done
echo "Android package sources in $out:"
grep -E 'package=|android:label|android:icon|allowBackup|minSdk|targetSdk' "$manifest" || true
