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

for size in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
    dir="$out/res/mipmap-${size%%:*}"
    mkdir -p "$dir"
    rsvg-convert -w "${size##*:}" -h "${size##*:}" "$svg" -o "$dir/ic_launcher.png"
done
echo "Android package sources in $out:"
grep -E 'package=|android:label|android:icon|minSdk|targetSdk' "$manifest" || true
