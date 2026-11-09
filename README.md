# Tether

SSH hosts and serial ports in tabs and split panes, for Omarchy.

![Tether with a tab split into three panes](docs/screenshot.png)

- **Saved hosts** with user, port, key file, jump host, extra ssh options, and
  port forwards (local, remote, and SOCKS). Hosts in `~/.ssh/config` show up too.
- **Serial and COM ports** through picocom: baud rate, data bits, parity, stop
  bits, flow control, what Enter sends, and local echo. Plugged-in USB serial
  devices appear on their own, by name.
- **Tabs and split panes**, moved between and resized from the keyboard, with
  zoom, like tmux.
- **Snippets** typed into the focused pane, or every pane in the tab at once.
- **Session logs** of everything a session prints, per host or port.
- **Quick connect**: type `user@host:port` or `/dev/ttyUSB0 9600` in the search.
- **Sessions that survive drops**: keep a host's shell running on the server in
  tmux, reconnect on its own when the link drops, or connect with mosh. Local
  shells can run in tmux too and are listed under Detached sessions after Tether
  closes.
- **Files**: browse a host over sftp beside its terminal (Ctrl+Shift+F), drop
  files in to upload, download, rename, and delete. It reuses the terminal's
  login, so it never asks for a password again.
- **Vault**: logins with a user, key file, and password or passphrase. Passwords
  live in the system keyring (GNOME Keyring, KeePassXC) and ssh gets them through
  an askpass helper; they never touch Tether's files.
- **Snippet automation**: `{{placeholders}}` asked for on send, `{{host}}`,
  `{{user}}`, `{{date}}`, `{{time}}` filled in, and snippets that run when a host
  connects.
- **Sync**: hosts, snippets, and vault logins (not passwords) mirrored to a
  folder that Syncthing, Dropbox, or git keeps in step.
- **Ask for a command** (Ctrl+Shift+A): describe it, and Claude answers with a
  command to insert or run, using your own Anthropic API key.
- **Settings** (Ctrl+,): colors, fonts, terminal schemes, and everything above.

Passwords are stored only if you save them in the vault, and then only in your
system keyring. Otherwise ssh asks for them in the terminal, or uses your keys
and ssh-agent. Hosts are kept in `~/.config/tether/tether.json`, readable only by
you, and logs in `~/.local/share/tether/logs`.

## Shortcuts

Terminal programs need plain keys and most Ctrl keys, so Tether's own shortcuts
use Ctrl+Shift, or Alt for moving between panes.

| Keys | Action |
| --- | --- |
| Ctrl+Shift+K | Search hosts and serial ports |
| Ctrl+Shift+T | New tab with a local shell |
| Ctrl+Shift+N | New host |
| Ctrl+Shift+E | Split right |
| Ctrl+Shift+O | Split down |
| Alt+Arrow | Move to the pane in that direction |
| Alt+Shift+Arrow | Move the divider |
| Ctrl+Shift+Z | Zoom the pane, or show all panes again |
| Ctrl+Shift+W | Close the pane |
| Ctrl+PgUp / Ctrl+PgDn | Previous or next tab |
| Ctrl+Shift+S | Snippets |
| Ctrl+Shift+F | Files on this host, beside the pane |
| Ctrl+Shift+A | Ask Claude for a command |
| Ctrl+Shift+C / Ctrl+Shift+V | Copy or paste |
| Ctrl+= / Ctrl+- / Ctrl+0 | Bigger, smaller, or default text |
| Ctrl+Shift+B | Show or hide the sidebar |
| Ctrl+, | Settings: colors, fonts, terminal, hosts, and snippets |
| F1 | All shortcuts |
| Ctrl+Shift+Q | Quit |
| Ctrl+A, Ctrl+X | Leave a serial session (picocom) |

In the sidebar, Enter connects, E edits, and Delete removes a saved host.

## Serial port permissions

Serial devices belong to a group: `uucp` on Arch, `dialout` on Debian and
Ubuntu. Add yourself once and log in again:

```sh
sudo usermod -aG uucp $USER
```

Tether says which group a device needs when it can't open it.

## Install

**Linux** (Arch, Omarchy, Ubuntu, Fedora, and others), in a terminal:

```sh
curl -fsSL https://raw.githubusercontent.com/oren1031/tether/main/install.sh | sh
```

On Arch-based systems that installs the pacman package; everywhere else it
installs the AppImage into `~/.local/bin` and adds Tether to the app launcher.
Tether then tells you when a new version is out and installs it for you.

Or download a file from the [latest release](https://github.com/oren1031/tether/releases/latest):

| System | File | Install |
| --- | --- | --- |
| Arch, Omarchy | `tether-*-x86_64.pkg.tar.zst` | `sudo pacman -U tether-*.pkg.tar.zst` |
| Ubuntu 22.04+, other Linux | `Tether-*-x86_64.AppImage` | `chmod +x Tether-*.AppImage` and run it (Ubuntu needs `libfuse2t64`, or `libfuse2` on 22.04) |
| Windows 10 (1809+) and 11 | `Tether-*-setup.exe` | Run it; it installs for your user, no admin needed |
| Android 9+ (64-bit ARM phones and tablets) | `Tether-*-android-arm64.apk` | Open it on the phone and allow installing from your browser |

Tether uses `ssh`, `picocom`, and optionally `tmux` and `mosh` from the system.
On Ubuntu: `sudo apt install openssh-client picocom tmux`. Without picocom (as on
Windows) Tether opens serial ports itself.

On Windows, Tether uses the OpenSSH client built into Windows (Settings → System →
Optional features → OpenSSH Client, on by default in current versions), opens COM
ports itself, and keeps vault passwords in Windows Credential Manager. Local tabs run
PowerShell. Connection sharing, local tmux, and mosh are Linux-only.

On Android, Tether has ssh built in (libssh), since Android has none: host keys,
key files (pick one with Choose… and Tether keeps a private copy), passwords, and
keeping sessions alive with tmux on the server all work. A row of extra keys (Esc,
Tab, Ctrl, Alt, arrows) sits above the keyboard. Not yet on Android: jump hosts,
port forwarding, the file browser, serial ports, and saving passwords in the vault.

To build it yourself:

```sh
git clone https://github.com/oren1031/tether.git
cd tether && bin/install
```

## What Tether stores and sends

**On your computer**

- `~/.config/tether/tether.json` (mode 600): saved hosts, snippets, settings,
  and vault entries without their passwords. On Android it is in the app's
  private storage, which backups leave out.
- Passwords you save in the vault: only in the system keyring (libsecret on
  Linux, Credential Manager on Windows), never in a file of Tether's.
- The Anthropic API key, if you enter one for "Ask for a command": in the
  system keyring when there is one, otherwise `~/.config/tether/anthropic-key`
  (mode 600).
- Session logs, for hosts or ports where you switched "Log" on:
  `~/.local/share/tether/logs` (folder 700, files 600). They hold everything
  the session printed, so a secret that appears on screen is in the log.
- Host keys the built-in ssh (Android) has accepted: `known_hosts` in the
  app's data folder. Desktop ssh uses your own `~/.ssh/known_hosts`.
- The update download, in the cache folder's `updates`, until the next one.
- A sync folder, if you choose one in Settings: `tether-hosts.json` with hosts,
  snippets and vault logins (not passwords). Whoever can read that folder can
  read it, and whoever can write there can change your hosts, so keep it
  somewhere only you can reach.

**Over the network**

- **Update checks** (every few hours, and when you press Check): your update
  server if one is set (default `https://tether.orenamir.com`, see below),
  then `api.github.com` and `github.com`. What they see is your IP address
  and "Tether/&lt;version&gt;" as the user agent; nothing else about you or
  your hosts is sent. Turn automatic checks off in Settings → General.
- **Ask for a command**, only when you use it: the text you type, and the
  session's kind with the host and user name of an ssh session, go to
  Anthropic (`api.anthropic.com`) with your API key, or to the `claude`
  program on your computer if you use Claude Code instead (that is then
  Claude Code's own connection). The terminal's contents are not sent.
  Nothing is sent unless you press Ask, and nothing it suggests runs without
  a click.
- **ssh, sftp, mosh** connect only to the hosts you saved or typed.

Tether has no analytics and no account.

## Your own update server

Tether looks for updates on an update server of your own before GitHub, and
falls back to GitHub when it can't be reached (set it in Settings → General →
Updates; it must be an `https://` address). The server only holds copies:
every release carries a signed list of its files (`latest.json` and
`latest.json.sig`), Tether checks that signature with the key built into it,
and installs a file only when it matches the list, so a server can't change
what gets installed. The default name `tether.orenamir.com` is the owner's
server; a fork or a build with another default sets `TETHER_MIRROR`.

On the server, with the domain's DNS pointing at it:

```sh
git clone https://github.com/oren1031/tether.git
sudo mkdir -p /srv/tether && sudo chown $USER /srv/tether
tether/bin/mirror-release /srv/tether          # copies and checks the newest release
crontab -e    # */15 * * * * ~/tether/bin/mirror-release /srv/tether
```

and serve `/srv/tether` over HTTPS, for example with Caddy using
[packaging/mirror/Caddyfile](packaging/mirror/Caddyfile). The installer uses
it too: `TETHER_MIRROR=https://tether.example.com sh install.sh`.

## Build

Needs Qt 6.9 or newer, picocom, and openssh. The terminal is built in, on [libvterm](third_party/libvterm).
Optional: tmux and mosh for sessions that survive drops, and libsecret with a
keyring for saved passwords.

```sh
bin/build      # build/tether
bin/test       # tests, offscreen
bin/install    # Arch package, so Tether is in the app launcher
```
