# Security

## Reporting a problem

Please report security problems privately, not in a public issue.

- On GitHub: **Security → Report a vulnerability** on
  <https://github.com/oren1031/tether> (private vulnerability reporting).
  The repository owner needs to have it switched on under
  Settings → Code security.
- Or write to the maintainer, whose address is on the GitHub profile
  <https://github.com/oren1031>.

Say which version you used (Settings shows it, or `tether --version`), what
system, and what you did. Expect an answer within a few days; a fix is
released as a new version, and the notes say what was wrong once people have
had time to update.

## What is protected, and how

- **Updates are signed.** Every release publishes `latest.json` (each
  file's size and SHA-256) and `latest.json.sig`, made with an ed25519 key
  that is not on any web server. Tether checks the signature with
  `ssh-keygen -Y verify` against the public key built into the app, and
  installs a file only when it matches the signed list. The list must be for
  the version offered and newer than the running one. A release without a
  valid signature, or without a checksum for the file, is refused. The same
  check is made for your own update server and for GitHub. On Android the
  system itself refuses an APK that is not signed with Tether's release key.
- **`install.sh`** checks `SHA256SUMS.sig` with the same key when
  `ssh-keygen` is installed, and always checks the file's SHA-256; it refuses
  to install without one. Without `ssh-keygen` it says plainly that only the
  checksum was checked.
- **Releases** are built by GitHub Actions from a tag, with third-party
  actions pinned to a commit, a read-only token except in the publishing
  step, and a build attestation for the files
  (`gh attestation verify <file> --repo oren1031/tether`).
- **Hosts and options:** a host's address, user, jump host, forwards and
  devices are matched against strict patterns, and `--` ends ssh's options
  before the host, so a saved host cannot become an ssh option.
- **Passwords** are never written to Tether's files: system keyring on Linux,
  Credential Manager on Windows.
- **Host keys:** the built-in ssh (Android) checks them with SHA-256
  fingerprints in its own known-hosts file.

## What is not done yet

- **The Windows installer and program are not code-signed**, so Windows shows
  an "unknown publisher" warning. Signing needs an outside account (SignPath
  is free for open source, or Azure Trusted Signing); until then the update
  signature above is what tells a real update from a fake one.
- Private keys picked on Android are stored unencrypted inside the app's
  private storage (backups of them are switched off); there is no app lock.

## Signing keys (for the maintainer)

| What | Where it lives | If it is lost |
|---|---|---|
| Android release key | repository secrets `ANDROID_KEYSTORE`, `ANDROID_KEYSTORE_PASSWORD`, and a backup of the `.p12` | every phone must uninstall Tether once before the next update installs |
| Update-signing key | repository secret `UPDATE_SIGNING_KEY`, and a backup of the private key file | installed copies cannot take updates: ship a new public key in a release signed with the old one, or users reinstall |

Never regenerate either key without a plan for the above. The update key's
public half is built into `src/updater.cpp`, `install.sh` and
`bin/mirror-release`; `bin/sign-release` refuses to sign unless all three
match the key it is given. A fork that publishes its own releases builds with
its own key (`TETHER_UPDATE_KEY`) and changes the other two copies.
