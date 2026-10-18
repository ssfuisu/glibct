# How glibct works

This page is for developers. The [README](../README.md) explains how to use
glibct.

## The problem

Termux runs on Android's C library, Bionic. Termux also has a glibc package
repository (`pkg install glibc-repo`) with glibc 2.44 in `$PREFIX/glibc`.
Programs from that repository start fine, because they ask for the loader
`$PREFIX/glibc/lib/ld-linux-aarch64.so.1`.

Programs built for regular Linux distributions ask for
`/lib/ld-linux-aarch64.so.1`, and scripts ask for `/bin/sh` or
`/usr/bin/env`. These paths do not exist on Android (`/bin` is missing or
points to `/system/bin`), so the kernel reports `ENOENT`.

The two C libraries also cannot share `LD_PRELOAD`, which was checked on the
device:

- Android's linker stops with `CANNOT LINK EXECUTABLE` if any `LD_PRELOAD`
  entry cannot be loaded. A missing file or a glibc library is enough.
- The glibc loader skips a missing preload with a warning, but stops if a
  preload needs a Bionic library: termux-exec needs `libc.so`, and the glibc
  `libc.so` in `$PREFIX/glibc/lib` is a linker script.
- termux-exec 2.5 keeps `LD_PRELOAD` when it starts a glibc program.

## The parts

| File | Built for | Job |
| :--- | :--- | :--- |
| `bin/glibct` | Termux bash script | Starts the glibc bash with the glibct environment, `--doctor`, `--update`, `--uninstall`. |
| `share/bashrc` | glibc bash | Startup file of the glibct shell: reads `~/.bashrc`, sets `PS1='glibct\$ '`, reads `~/.glibctrc`. |
| `src/glibc_hook.c` + `src/common.c` | glibc (`libglibct.so`) | The hook inside every glibc program. |
| `src/termux_hook.c` + `src/common.c` | Bionic (`libglibct-termux.so`) | The hook inside Termux programs started from glibct. |

### Loading the hooks

`libglibct.so` is listed in `$PREFIX/glibc/etc/ld.so.preload`, the file
that Termux's glibc loader reads (it is compiled in as the loader's
`/etc/ld.so.preload`). Android's linker never reads it, so every glibc
program gets the hook and no Termux program does. The glibc world runs with
`LD_PRELOAD` unset. `glibct` adds the line the first time it runs and
`glibct --uninstall` removes it.

`libglibct-termux.so` is loaded with `LD_PRELOAD`, in front of termux-exec,
into Termux programs that a glibc program starts inside glibct.

### The glibct environment

`glibct` saves the Termux values and then changes them:

| Variable | In glibct | Saved as |
| :--- | :--- | :--- |
| `PATH` | `$PREFIX/glibc/bin` right before `$PREFIX/bin` | (computed back) |
| `LD_PRELOAD` | unset | `GLIBCT_TERMUX_LD_PRELOAD` |
| `LD_LIBRARY_PATH` | unset | `GLIBCT_TERMUX_LD_LIBRARY_PATH` |
| `SHELL` | `$PREFIX/glibc/bin/bash` | `GLIBCT_TERMUX_SHELL` |
| `GLIBCT` | `1` | |

## Starting a program

Both hooks wrap `execve` and `posix_spawn`, and build `execv`, `execvp`,
`execvpe`, `execl`, `execlp`, `execle` and `posix_spawnp` on top of them
(glibc calls its own internal functions, so these must be replaced too).
For every start, `route_exec()` in `src/common.c` opens the file and reads
the first bytes:

| File | Started as |
| :--- | :--- |
| ELF, interpreter `$PREFIX/glibc/lib/ld-linux-aarch64.so.1` | itself (glibc) |
| ELF, interpreter under `/system/` | itself (Bionic) |
| ELF, a missing `ld-linux*` interpreter | `$PREFIX/glibc/lib/ld-linux-aarch64.so.1 --argv0 ARGV0 FILE ARGS` |
| ELF for x86_64 | `$PREFIX/glibc/bin/box64 FILE ARGS`, if box64 is installed |
| `#!` script | the same rules for the interpreter, up to 4 levels, then `ELOOP` |
| anything else | itself |

The glibc hook also maps paths, the way termux-exec does: `/bin/X` and
`/usr/bin/X` always, and `/usr/local/bin/X`, `/sbin/X`, `/usr/sbin/X` when
missing, become `$PREFIX/glibc/bin/X` or else `$PREFIX/bin/X`. For
`#!/usr/bin/env NAME` (also with `-S`), it looks up `NAME` in `PATH` itself;
lines with quotes or `$` go to a real `env` program.

When nothing has to change, the original file and argv are passed on, so the
kernel runs scripts exactly as usual. The exec path keeps its state on the
stack and calls `malloc` only for very long lists, so it can run in a
`vfork()` child.

### Changing the environment

The environment follows the kind of program that runs in the end.

The glibc hook:

- A Termux program inside glibct gets `LD_PRELOAD`, `LD_LIBRARY_PATH` and
  `SHELL` back from the saved values, and a `PATH` without
  `$PREFIX/glibc`. `LD_PRELOAD` gets `libglibct-termux.so` in front.
- A Termux program outside glibct only loses glibc entries in `LD_PRELOAD`.
- A glibc program loses Android entries in `LD_PRELOAD`.

The Termux hook only acts when a glibc, Linux or x86_64 program is started.
That program gets the glibct environment back: Android entries removed from
`LD_PRELOAD` and `LD_LIBRARY_PATH`, glibc programs first in `PATH`, and the
glibc bash as `SHELL`. It is started with the `execve` system call directly,
because termux-exec only knows Termux programs. Everything else goes to
termux-exec unchanged.

## Programs started through the loader

The kernel sees the loader as the program, so the glibc hook fixes what the
program sees about itself. The program path is found in
`/proc/self/cmdline` (the first argument after the loader options), because
the loader leaves `l_name` of the main program empty.

- `readlink` and `readlinkat` of `/proc/self/exe`, `/proc/thread-self/exe`
  and `/proc/PID/exe`, and `realpath` and `canonicalize_file_name` of them,
  return the real program. `execve("/proc/self/exe")` starts the real
  program again.
- `argv[0]` is kept with the loader's `--argv0`.
- The process name (`/proc/self/comm`, shown by `top`, used by `pkill`) is
  set to the name of the started path, like the kernel does.

## System calls blocked by Android

Android's seccomp filter stops some newer system calls with `SIGSYS`. On the
test phone (Android 12) these were: `openat2`, `mount_setattr`,
`memfd_secret`, `process_mrelease`, `cachestat`, `map_shadow_stack` and
`pkey_alloc`. The glibc hook installs a `SIGSYS` handler that answers
`ENOSYS` for seccomp traps, which is what a kernel without these calls does,
so programs use their fallback instead of dying with `Bad system call`.
Other `SIGSYS` signals keep their default action. A program that installs
its own `SIGSYS` handler replaces this one.

## Limits

- aarch64 only.
- Static programs and programs that start others with the raw `execve`
  system call (most Go programs) bypass the hooks. Termux programs they
  start work, Linux programs they start do not.
- Opening `/proc/self/exe` as a file (instead of `readlink`) still gives the
  loader.
- glibc 2.44 from Termux is the newest glibc a program can need.

## Debugging

`GLIBCT_DEBUG=1` makes both hooks print one line for each program they start
and each blocked system call:

```
glibct: ./node -> /data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1 (linux)
glibct: system call 437 is blocked by Android, returning ENOSYS
```

## Building and testing

On the phone:

```
pkg install git make clang gcc-glibc
git clone https://github.com/ssfuisu/glibct
cd glibct
make install
bash tests/run-tests.sh
```

`make install` builds `libglibct.so` with the glibc gcc and
`libglibct-termux.so` with Termux's clang, and installs them with the other
files. The tests (`tests/run-tests.sh`) build small helper programs with the
glibc gcc and Termux's clang and run 60 checks inside glibct.

CI (`.github/workflows/build.yml`) builds both libraries with
`aarch64-linux-gnu-gcc` and the Android NDK, checks the shell scripts with
shellcheck and, for a `v*` tag, publishes `glibct-aarch64.tar.gz` and
`SHA256SUMS` as a GitHub release. `install.sh` downloads the newest release.
