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
API="https://api.github.com/repos/$REPO/releases/latest"

say() { printf '\033[1m%s\033[0m\n' "$*"; }
fail() { printf 'tether install: %s\n' "$*" >&2; exit 1; }

command -v curl >/dev/null 2>&1 || fail "curl is needed."
[ "$(uname -s)" = Linux ] || fail "this script is for Linux. On Windows, get the setup program from https://github.com/$REPO/releases/latest"
[ "$(uname -m)" = x86_64 ] || fail "only x86_64 builds are published so far."

MIRROR="${TETHER_MIRROR:-https://tether.orenamir.com}"

# Prints "name<TAB>url<TAB>sha256" for each file of the newest release: from
# the update server's latest.json when it answers, else from GitHub's API.
# GitHub's API links work even when its download links don't.
# One "key": value per line, whether the JSON came pretty or on one line.
fields() {
    printf '%s\n' "$1" | tr ',{}[]' '\n\n\n\n\n'
}
say "Finding the latest Tether…"
if manifest=$(curl -fsSL --max-time 10 "$MIRROR/latest.json" 2>/dev/null) && printf '%s' "$manifest" | grep -q '"version"'; then
    source="$MIRROR"
    files=$(fields "$manifest" | awk -v base="$MIRROR" -F'"' '
        /"name": *"/   { name = $4 }
        /"sha256": *"/ { sha = $4 }
        /"url": *"/    { print name "\t" base "/" $4 "\t" sha; name = ""; sha = "" }')
else
    source=GitHub
    release=$(curl -fsSL -H "Accept: application/vnd.github+json" "$API") || fail "couldn't reach GitHub."
    files=$(fields "$release" | awk -F'"' '
        /"url": *"https:\/\/api.github.com\/repos\/.*\/releases\/assets\// { url = $4 }
        /"name": *"/   { name = $4 }
        /"digest": *"/ { sha = $4; sub(/^sha256:/, "", sha) }
        /"browser_download_url": *"/ { print name "\t" url "\t" sha; url = ""; name = ""; sha = "" }')
fi
pick() {
    printf '%s\n' "$files" | grep -E "^[^	]*$1	" | grep -v -- '-debug-' | head -n 1
}
# Downloads a picked file to $2 and checks its checksum.
fetch() {
    name=$(printf '%s' "$1" | cut -f1); url=$(printf '%s' "$1" | cut -f2); sha=$(printf '%s' "$1" | cut -f3)
    say "Downloading $name from $source…"
    case "$url" in
        https://api.github.com/*) curl -fL --progress-bar -H "Accept: application/octet-stream" -o "$2" "$url" ;;
        *) curl -fL --progress-bar -o "$2" "$url" ;;
    esac || fail "the download failed."
    if [ -n "$sha" ] && command -v sha256sum >/dev/null 2>&1; then
        echo "$sha  $2" | sha256sum -c --quiet - || fail "the downloaded file's checksum doesn't match; try again."
    fi
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if command -v pacman >/dev/null 2>&1 && [ -d /var/lib/pacman/local ]; then
    file=$(pick '-x86_64\.pkg\.tar\.zst') || true
    [ -n "$file" ] || fail "the latest release has no Arch package."
    package="$tmp/$(printf '%s' "$file" | cut -f1)"
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
