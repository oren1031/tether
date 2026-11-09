# Changelog

What changed in each version. Newest first. The release workflow uses the
section named for the version being released (for example `## 0.5.5`) as that
release's notes.

## 0.5.5

Security, accessibility and quality fixes from the audit of 0.5.4, and Hebrew.

### Sync, host keys and secrets
- Synced changes to where a host connects, its ssh options, a snippet's text or
  a login are held in a review sheet until you accept them. Options that run a
  program or switch off server checks never apply on their own. The newer edit
  wins; the sync file is private to you (mode 600).
- A changed host key defaults to "Don't connect", shows the saved and the new
  fingerprint, and needs the host's name typed to replace it. On Windows the
  yes/no question is a dialog with No as the default.
- Pasting strips control characters and asks before several lines go into a
  program that would run each as it arrives (a setting turns the question off).
- The Claude API key is kept in the keyring (secret-tool / Credential Manager),
  and an old key file is moved there. The Ask dialog says what is sent.
- Names with a line break are refused in the file browser. Saving a login,
  checking a connection and forgetting a host key no longer freeze the window.
  An idle ssh session sleeps instead of polling every 20 ms.

### Look and feel
- Text on accent colours is chosen by contrast (it was always black).
- Fields, sheets and controls have names for screen readers, a focus ring and
  keyboard access (settings sections, accent swatches, steppers, segmented
  controls). Touch targets are 44 px on phones; a tap connects and a long press
  opens the menu. Sheets no longer close when you click outside them, and
  destructive actions ask first.
- Layouts mirror in right-to-left languages.

### Language
- Hebrew, and a Language setting (System or any shipped language).


### Updates and releases
- Updates are signed. Each release carries `latest.json` (the size and
  SHA-256 of every file) and `latest.json.sig`; Tether checks the signature
  against a key built into it before it installs anything, whether the
  update came from your own server or from GitHub. A release with no valid
  signature, no checksum or no size is refused, and so is a signed list for
  another version than the one offered.
- The update server setting only takes `https://` addresses.
- Updates are saved to disk while they download instead of being held in
  memory, and checked as they arrive.
- Links in release notes open only when they are `https://`.
- `install.sh` refuses to install without a checksum, checks the signed
  checksum list when `ssh-keygen` is installed, and says so when it isn't.
- Releases also publish `SHA256SUMS` with its signature, and a build
  attestation. `bin/mirror-release` copies the signed list byte for byte and
  checks it before putting anything in place.
- The release workflow stops a tagged build that lacks the Android key or the
  update key, checks the APK is signed with the expected certificate, can no
  longer publish an older build of a file after a re-run, pins its actions
  and tools, and only publishing has write access.
- Tests and a build run on every push, and the tests run again under
  AddressSanitizer and UBSan; a monthly check looks for newer libssh,
  OpenSSL build, libvterm and Qt.
- The program is built with fortify, stack protector and RELRO/BIND_NOW (and
  Control Flow Guard on Windows).

### Terminal
- Fixed a memory-safety bug in the bundled libvterm: a server sending more
  than 16 arguments in one escape sequence wrote past the end of an array.
  Found by a new test that feeds the terminal random bytes.

### Android
- Backups of the app's data (which includes imported private keys) are off.
- The self-test, `--ssh-test` and `--try-install` switches exist only in the
  build the CI emulator runs. In the released app, `--connect` from another
  app asks "Connect to X?" first and ignores `--identity`.
- CI tests on Android 15 (API 35).

## 0.5.4 — 2026-11-09
- Closing a session on Android no longer freezes the app for a few seconds
  and then crashes. Closing the last tab on a phone shows the host list again.

## 0.5.3 — 2026-11-09
- Typing works on Android phones: letters, stars while typing a password,
  Return and Backspace from the on-screen keyboard; a dropped connection at
  a prompt no longer loops.
- Sheets and menus keep clear of the status bar, the navigation buttons and
  the keyboard on Android 15 and later.

## 0.5.2 — 2026-11-09
- Update servers: your own server is checked before GitHub (`latest.json`,
  SHA-256 checked), and GitHub's API links are used when its download links
  fail. (0.5.1 was never published: its build failed on a test.)

## 0.5.0 — 2026-11-08
- Made for phones too: a layout for narrow touch screens, in-app updates on
  Android, passwords typed with stars.

## 0.4.2 — 2026-11-08
- Host keys that can be trusted on any keyboard, and fixed.

## 0.4.1 — 2026-11-08
- Typing on Android phones.

## 0.4.0 — 2026-11-08
- Android: ssh built in with libssh, a real login tested in an emulator in CI.

## 0.3.0 — 2026-11-07
- Windows (ConPTY, the built-in OpenSSH client, Credential Manager) and a
  terminal engine of its own, on libvterm. `tether --self-test` runs on every
  platform in CI.

## 0.2.2 — 2026-11-07
- An AppImage for Ubuntu and other Linux, and a one-line installer.

## 0.2.1 — 2026-11-07
- "Ask for a command" can use a Claude subscription through Claude Code.
  Serial port lists sort the same in every locale.

## 0.2.0 — 2026-11-07
- First published version: SSH and serial terminal for Omarchy, with tabs and
  split panes, a vault, snippets, session logs and sync.
