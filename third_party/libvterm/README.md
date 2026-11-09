libvterm 0.3.3 by Paul Evans, MIT licensed (see LICENSE), from
https://launchpad.net/libvterm/trunk/v0.3/+download/libvterm-0.3.3.tar.gz
(sha256 09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0).
Only `src/` and `include/` are kept; the generated encoding tables come with the release.

Local changes (a fuzz test found the first; `terminalSurvivesRandomOutput`):
- `src/parser.c`: more than 16 `;`-separated arguments in one CSI sequence
  wrote past the end of the `args` array; the argument index now stops at the
  last slot.
