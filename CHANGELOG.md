# Changes

## 1.1.0

- Termux programs inside glibct can now start glibc and Linux programs. For
  example, Termux's `make` can run the glibc `gcc`, and a Termux Python can
  start a downloaded Linux program.
- Programs no longer die with `Bad system call` when they use a newer system
  call that Android blocks (like `openat2`). They get a "not supported" answer
  and use their fallback, as on an older Linux kernel.
- Programs made for normal PCs (x86_64) run with box64, if it is installed
  (`pkg install box64-glibc`).
- New: `glibct --doctor` checks the installation and says how to fix
  problems.
- New: `glibct --update` installs the newest version.
- The README is written for everyone; the technical details moved to
  `docs/how-it-works.md`.

## 1.0.0

- First release: the `glibct` shell, where Linux programs (arm64) run in
  Termux, scripts with `#!/bin/sh` or `#!/usr/bin/env` work, and Termux
  programs keep working.
