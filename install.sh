#!/bin/sh
# Installs the latest Tether.
#
#   curl -fsSL https://raw.githubusercontent.com/oren1031/tether/main/install.sh | sh
#
# On Arch, Omarchy, and Manjaro it installs the pacman package (sudo asks for
# your password). Everywhere else, Ubuntu included, it puts the AppImage in
# ~/.local/bin and adds Tether to the app launcher; nothing needs root.
# Either way, Tether updates itself after that.
set -eu

REPO="oren1031/tether"
API="${TETHER_API:-https://api.github.com/repos/$REPO/releases/latest}"
# The public half of the key releases are signed with: the same one that is
# built into Tether itself (src/updater.cpp).
KEY="${TETHER_UPDATE_KEY:-AAAAC3NzaC1lZDI1NTE5AAAAIGlNzf+gXq5FiE2CBcdgvlgMfKFqLNDo4wnYITdFhF2Q}"

say() { printf '\033[1m%s\033[0m\n' "$*"; }
fail() { printf 'tether install: %s\n' "$*" >&2; exit 1; }

command -v curl >/dev/null 2>&1 || fail "curl is needed."
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is needed, to check the download."
[ "$(uname -s)" = Linux ] || fail "this script is for Linux. On Windows, get the setup program from https://github.com/$REPO/releases/latest"
[ "$(uname -m)" = x86_64 ] || fail "only x86_64 builds are published so far."

# The update server asked before GitHub; TETHER_MIRROR= (empty) skips it.
MIRROR="${TETHER_MIRROR-https://tether.orenamir.com}"
progress=-sS

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# One "key": value per line, whether the JSON came pretty or on one line.
fields() {
    printf '%s\n' "$1" | tr ',{}[]' '\n\n\n\n\n'
}
# A release's file by name, to $2: from the update server's folder for the
# version, or through GitHub's API, whose links work even when its download
# links don't.
get() {
    if [ "$source" = GitHub ]; then
        url=$(printf '%s\n' "$files" | awk -F'\t' -v n="$1" '$1 == n { print $2; exit }')
        [ -n "$url" ] || return 1
        curl -fL $progress --max-time 900 -H "Accept: application/octet-stream" -o "$2" "$url"
    else
        curl -fL $progress --max-time 900 -o "$2" "$MIRROR/$version/$1"
    fi
}
# The release's checksum list and its signature, when the source has them.
get_sums() {
    rm -f "$tmp/SHA256SUMS" "$tmp/SHA256SUMS.sig"
    get SHA256SUMS "$tmp/SHA256SUMS" 2>/dev/null && get SHA256SUMS.sig "$tmp/SHA256SUMS.sig" 2>/dev/null \
        && [ -s "$tmp/SHA256SUMS" ] && [ -s "$tmp/SHA256SUMS.sig" ]
}
github() {
    source=GitHub
    release=$(curl -fsSL -H "Accept: application/vnd.github+json" "$API") || fail "couldn't reach GitHub."
    version=$(fields "$release" | awk -F'"' '/"tag_name": *"/ { v = $4; sub(/^[vV]/, "", v); print v; exit }')
    # name<TAB>API link<TAB>the SHA-256 GitHub recorded, for each file.
    files=$(fields "$release" | awk -F'"' '
        /"url": *"https:\/\/[^"]*\/releases\/assets\// { url = $4 }
        /"name": *"/   { name = $4 }
        /"digest": *"/ { sha = $4; sub(/^sha256:/, "", sha) }
        /"browser_download_url": *"/ { print name "\t" url "\t" sha; url = ""; name = ""; sha = "" }')
}

say "Finding the latest Tether…"
source=
if [ -n "$MIRROR" ] && manifest=$(curl -fsSL --max-time 10 "$MIRROR/latest.json" 2>/dev/null); then
    version=$(fields "$manifest" | awk -F'"' '/"version": *"/ { print $4; exit }')
    case "$version" in
        ''|*[!0-9.]*) ;;
        *) source="$MIRROR"; get_sums || source= ;;
    esac
fi
if [ -z "$source" ]; then
    github
    get_sums || rm -f "$tmp/SHA256SUMS" "$tmp/SHA256SUMS.sig"
fi
case "$version" in ''|*[!0-9.]*) fail "couldn't read the latest version." ;; esac

# Releases are signed: SHA256SUMS lists each file's checksum, and
# SHA256SUMS.sig is its signature, which ssh-keygen checks against the key
# above. So a file is only installed when it is the one that was published.
if [ -s "$tmp/SHA256SUMS.sig" ]; then
    if command -v ssh-keygen >/dev/null 2>&1; then
        printf 'tether-update namespaces="tether-update" ssh-ed25519 %s\n' "$KEY" > "$tmp/signers"
        ssh-keygen -Y verify -f "$tmp/signers" -I tether-update -n tether-update \
            -s "$tmp/SHA256SUMS.sig" < "$tmp/SHA256SUMS" >/dev/null 2>&1 \
            || fail "the signature on Tether $version's checksum list isn't Tether's (or ssh-keygen here is older than OpenSSH 8.1). Nothing was installed."
        say "Tether $version: the signature is good."
    else
        say "Tether $version: ssh-keygen isn't installed, so the signature was NOT checked; only the checksum will be."
    fi
elif [ "$(printf '%s\n' 0.5.4 "$version" | sort -V | tail -n 1)" != 0.5.4 ]; then
    # Everything after 0.5.4 is signed; a release without its signature isn't one.
    fail "Tether $version has no signed checksum list. Nothing was installed."
else
    # 0.5.4 and older were published before signing: GitHub's own record of
    # each file's checksum is all there is.
    say "Tether $version was published before releases were signed; checking GitHub's checksums only."
    printf '%s\n' "$files" | awk -F'\t' '$3 != "" { print $3 "  " $1 }' > "$tmp/SHA256SUMS"
fi

# The first file in the checksum list whose name matches.
pick() {
    awk '{ print $2 }' "$tmp/SHA256SUMS" | grep -E -- "$1\$" | grep -v -- '-debug-' | grep -E '^[A-Za-z0-9][A-Za-z0-9._+-]*$' | head -n 1
}
# Downloads a picked file to $2; it stays only when its checksum is the list's.
fetch() {
    sum=$(awk -v n="$1" '$2 == n { print $1; exit }' "$tmp/SHA256SUMS")
    [ -n "$sum" ] || fail "there is no checksum for $1, so it won't be installed."
    say "Downloading $1 from $source…"
    progress=--progress-bar
    get "$1" "$2" || fail "the download failed."
    echo "$sum  $2" | sha256sum -c --quiet - >/dev/null 2>&1 \
        || { rm -f "$2"; fail "the downloaded file's checksum doesn't match; nothing was installed. Try again."; }
}

if command -v pacman >/dev/null 2>&1 && [ -d /var/lib/pacman/local ]; then
    file=$(pick '-x86_64\.pkg\.tar\.zst') || true
    [ -n "$file" ] || fail "the latest release has no Arch package."
    package="$tmp/$file"
    fetch "$file" "$package"
    say "Installing with pacman (sudo asks for your password)…"
    sudo pacman -U --needed "$package"
    say "Done. Open Tether from your app launcher, or run: tether"
    exit 0
fi

file=$(pick '-x86_64\.AppImage') || true
[ -n "$file" ] || fail "the latest release has no AppImage yet."
bin="$HOME/.local/bin"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/scalable/apps"
mkdir -p "$bin" "$apps" "$icons"

fetch "$file" "$tmp/Tether.AppImage"
chmod +x "$tmp/Tether.AppImage"
mv "$tmp/Tether.AppImage" "$bin/Tether.AppImage"
ln -sf "$bin/Tether.AppImage" "$bin/tether"

curl -fsSL -o "$icons/tether.svg" "https://raw.githubusercontent.com/$REPO/main/pkgbuild/tether.svg" || true
cat > "$apps/tether.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Tether
Comment=SSH hosts and serial ports in tabs and split panes
Exec=$bin/Tether.AppImage
Icon=tether
Terminal=false
Categories=Network;RemoteAccess;System;TerminalEmulator;
Keywords=ssh;serial;com;uart;terminal;tmux;
StartupWMClass=tether
EOF
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" >/dev/null 2>&1 || true

# AppImages need FUSE 2 to run directly; Ubuntu 22.04 and later name it libfuse2(t64).
if ! ldconfig -p 2>/dev/null | grep -q 'libfuse\.so\.2'; then
    say "Note: AppImages need FUSE 2. On Ubuntu, install it once with:"
    echo "  sudo apt install libfuse2t64    # Ubuntu 24.04 and later"
    echo "  sudo apt install libfuse2       # Ubuntu 22.04"
fi
for tool in ssh picocom tmux; do
    command -v "$tool" >/dev/null 2>&1 || missing="${missing:-} $tool"
done
if [ -n "${missing:-}" ]; then
    say "Tether uses these, which aren't installed:$missing"
    echo "  sudo apt install openssh-client picocom tmux"
fi

case ":$PATH:" in *":$bin:"*) ;; *) say "Add $bin to your PATH to run tether from a terminal." ;; esac
say "Done. Open Tether from your app launcher, or run: $bin/tether"
