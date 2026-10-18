# glibct

A Termux shell where every command runs with glibc.

Termux runs on Android's own C library, Bionic. Most Linux programs are built
for glibc, the C library of regular Linux systems, so in Termux they fail with
errors like `No such file or directory`, even though the file is right there.

`glibct` opens a shell where these programs just work. It uses your normal
Termux home directory, files and settings. Nothing is copied or changed, and
there is no container, no proot and no root access involved.

```
~ $ glibct
glibct$ ./node-v22.11.0-linux-arm64/bin/node --version
v22.11.0
glibct$ exit
~ $
```

## Install

Run this in Termux:

```
curl -fsSL https://raw.githubusercontent.com/ssfuisu/glibct/main/install.sh | bash
```

You need Termux on a 64-bit ARM (aarch64) device. If they are missing, the
installer adds the Termux glibc package repository and installs `glibc`,
`bash-glibc` and `coreutils-glibc`. Run the same command again to update.

## Use

Type `glibct` to open the glibc shell. The prompt changes from `$` to
`glibct$`. Type `exit` to go back to the normal Termux shell.

To run a single command with glibc and return:

```
glibct node --version
glibct -c 'cd my-project && npm install'
```

## What happens inside glibct

- Commands are looked up in `$PREFIX/glibc/bin` first, then in the usual
  Termux places. Termux has glibc builds of many tools; install them with
  `pkg`, for example `pkg install git-glibc python-glibc make-glibc`.
  `pkg search glibc` lists all of them.
- Programs downloaded for Linux arm64 run as they are: official Node.js
  builds, Claude Code, and other programs from GitHub releases. The files are
  not patched or copied.
- Scripts that start with `#!/bin/sh`, `#!/bin/bash` or
  `#!/usr/bin/env node` work.
- Programs that exist only for Termux, like `pkg` and the `termux-*` tools,
  still work. They get the normal Termux environment.
- Your home directory, files, shell history and `~/.bashrc` are the same as
  in Termux.

## Settings

- `~/.glibctrc` is read by the glibct shell only, after `~/.bashrc`. Use it
  for glibct-only settings, for example your own prompt:
  `PS1='glibct \w\$ '`.
- To open glibct every time Termux starts, add this line to the end of
  `~/.bashrc`:

  ```
  [ -z "$GLIBCT" ] && glibct
  ```

- `GLIBCT_DEBUG=1` prints one line for each program that starts and how it
  was started.

## How it works

1. `glibct` starts the glibc build of bash. It puts `$PREFIX/glibc/bin` in
   front of `$PREFIX/bin` in `PATH`, removes Termux's `LD_PRELOAD` (glibc
   cannot load Android libraries) and sets the prompt.
2. A small library, `libglibct.so`, is listed in
   `$PREFIX/glibc/etc/ld.so.preload`. Only the glibc loader reads that file,
   so every glibc program loads the library and Termux programs never do.
3. The library checks each program before it starts (`execve`, `execvp`,
   `posix_spawn` and the other exec functions):
   - A program built for regular Linux asks for
     `/lib/ld-linux-aarch64.so.1`, which does not exist on Android. It is
     started through Termux's `$PREFIX/glibc/lib/ld-linux-aarch64.so.1`
     instead.
   - `/bin/...` and `/usr/bin/...` paths point to the glibc or Termux program
     of the same name.
   - For `#!` scripts the same rules apply to the interpreter.
   - A Termux program gets the Termux values of `LD_PRELOAD`, `PATH` and
     `SHELL` back.
4. A program started through the loader would see the loader as its own
   executable. The library makes `readlink("/proc/self/exe")` and `realpath`
   return the real program, and keeps `argv[0]` and the process name, so
   programs that restart themselves or look for files next to themselves
   keep working.

## Limits

- Only 64-bit ARM (aarch64) is supported.
- Programs that start other programs without the C library, like most Go
  programs, bypass glibct. Linux programs that they start fail as they do in
  plain Termux. Termux programs that they start still work.
- A Termux program that starts a glibc program by its full path fails, for
  example Termux's `make` running `$PREFIX/glibc/bin/gcc`. Install the glibc
  build of the tool instead (`make-glibc` in this example).
- Programs built for x86 or 32-bit ARM do not run.
- Android blocks some system calls. The few Linux programs that need them
  stop with `Bad system call`.
- Programs that need a newer glibc than the one in Termux do not start.
- A program that opens `/proc/self/exe` as a file, instead of reading the
  link, still gets the loader.

## Uninstall

```
glibct --uninstall
```

This removes glibct. The glibc packages stay installed; remove them with
`pkg uninstall` if you do not need them.

## Build from source

```
pkg install git make gcc-glibc
git clone https://github.com/ssfuisu/glibct
cd glibct
make install
```

`bash tests/run-tests.sh` runs the tests on the device.

Tested on Android 12 with Termux 0.119.0-beta.3, glibc 2.44, Node.js 22
(npm and npx) and Claude Code 2.1.

## License

MIT
