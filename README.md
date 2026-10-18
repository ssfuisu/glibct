# glibct

**Run Linux programs on your Android phone, inside Termux.**

Without glibct, a program made for Linux does not start:

```
~ $ ./node-v22.11.0-linux-arm64/bin/node --version
bash: ./node-v22.11.0-linux-arm64/bin/node: cannot execute: required file not found
```

With glibct, the same program just works:

```
~ $ glibct
glibct$ ./node-v22.11.0-linux-arm64/bin/node --version
v22.11.0
glibct$ exit
~ $
```

## What is this?

[Termux](https://termux.dev) is an app that gives your Android phone a
command line, like the terminal on a Linux computer.

Most programs on the internet are made for normal Linux computers. Android is
built a little differently on the inside, so these programs usually refuse to
start in Termux. The error message is confusing, often just
`No such file or directory`, even though the file is right there.

glibct fixes that. Think of it as a travel adapter for programs: the program
stays exactly the same, and glibct makes it fit your phone.

- Your files stay where they are. Nothing is copied.
- There is no second Linux system inside your phone and no virtual machine,
  so programs run at the full speed of your phone.
- You do not need root.

## What can I run with it?

Everything in this list was tested on a real phone:

- **Programs you download for Linux.** On a download page, pick the version
  for "Linux" and "arm64" (sometimes called "aarch64"). For example the
  official Node.js, Claude Code, or tools from GitHub.
- **Node.js and Python packages with built-in Linux parts.** For example
  `npm install` and `npx` with the official Node.js, or `pip install numpy`
  with the glibc version of Python.
- **The glibc versions of Termux tools.** Termux offers many tools in a
  version made for this, for example `git-glibc` or `python-glibc`.
- **Programs for normal PCs (x86_64)**, if you install box64. They run more
  slowly, and not all of them work.

## Install

You need Termux on a 64-bit phone. Almost every phone from the last few years
is 64-bit.

1. Install Termux from [F-Droid](https://f-droid.org/packages/com.termux/) or
   from [GitHub](https://github.com/termux/termux-app/releases).
2. Open Termux, copy this line, paste it and press Enter:

   ```
   curl -fsSL https://raw.githubusercontent.com/ssfuisu/glibct/main/install.sh | bash
   ```

3. Wait until it says `glibct ... is installed`. This takes a few minutes the
   first time, because it also installs the parts glibct needs.

## How to use it

1. Type `glibct` and press Enter. The start of the line changes from `$` to
   `glibct$`. Now everything you type runs the Linux way.
2. Use your programs as usual.
3. Type `exit` to go back to normal Termux.

You can also run a single program without opening glibct first:

```
glibct node --version
```

## Commands

| Type this | What happens |
| :--- | :--- |
| `glibct` | Starts glibct. |
| `exit` | Leaves glibct, back to normal Termux. |
| `glibct PROGRAM` | Runs one program with glibct, then returns. |
| `glibct --doctor` | Checks that everything is in place and tells you how to fix problems. |
| `glibct --update` | Updates glibct to the newest version. |
| `glibct --uninstall` | Removes glibct. |
| `pkg search glibc` | Lists the Termux tools that have a glibc version. |
| `pkg install git-glibc` | Installs the glibc version of a tool, here git. |
| `pkg install box64-glibc` | Lets glibct run programs made for normal PCs (x86_64). |

## Questions

**Will glibct change or break my normal Termux?**
No. Outside of glibct, Termux works exactly as before. Inside glibct, the
normal Termux programs like `pkg` keep working too.

**Where are my files?**
In the same place as always. glibct uses your normal Termux home folder.

**Is this a virtual machine, a container or proot?**
No. Programs run directly on your phone, without anything in between that
slows them down.

**Can I start glibct automatically when Termux opens?**
Yes. Add this line at the end of the file `~/.bashrc`:

```
[ -z "$GLIBCT" ] && glibct
```

**Can I change the `glibct$` prompt?**
Yes. Put your own prompt in the file `~/.glibctrc`, for example
`PS1='glibct \w\$ '` to also show the current folder.

**A program does not work. What can I do?**

1. Run `glibct --doctor` and follow the "Fix" lines.
2. Make sure you downloaded the Linux **arm64** version of the program.
3. Run the program again with `GLIBCT_DEBUG=1` in front, for example
   `GLIBCT_DEBUG=1 ./my-program`, and
   [open an issue](https://github.com/ssfuisu/glibct/issues) with what it
   prints.

**How do I remove glibct?**
Type `glibct --uninstall`. The glibc packages from Termux stay installed; you
can remove them with `pkg uninstall` if you do not need them.

## What does not work

- Phones with 32-bit Android.
- Programs that need special rights or hardware access, like Docker, systemd
  or anything that needs root.
- Programs that need a newer glibc than Termux has (glibc 2.44 at the moment).
- Some programs written in Go start other programs in a way glibct cannot
  see. The programs they start do not get glibct's help.
- Programs for normal PCs (x86_64) run through an emulator, box64. They are
  slower, and some of them crash.

## For developers

[docs/how-it-works.md](docs/how-it-works.md) explains how glibct works inside
and how to build and test it.

## License

MIT. You can use, change and share glibct freely.
