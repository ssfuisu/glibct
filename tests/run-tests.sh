#!/usr/bin/env bash
# Tests glibct on a Termux device (aarch64).
#
# Needs an installed glibct and a C compiler for glibc:
#   pkg install gcc-glibc
# Run it from a normal Termux shell or from glibct:
#   bash tests/run-tests.sh

# Many tests pass $VARS in single quotes on purpose: the shell under test
# expands them.
# shellcheck disable=SC2016

set -u

if [ "${GLIBCT-}" != 1 ]; then
	exec glibct bash "$0" "$@"
fi

TERMUX_PREFIX=/data/data/com.termux/files/usr
G=$TERMUX_PREFIX/glibc
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d "$TERMUX_PREFIX/tmp/glibct-tests.XXXXXX")
trap 'rm -rf "$T"' EXIT
pass=0
fail=0

# check NAME PATTERN... -- COMMAND...
# Passes when the output (stdout and stderr) contains every pattern.
check() {
	local name=$1 out p ok=1 patterns=()
	shift
	while [ "$1" != -- ]; do
		patterns+=("$1")
		shift
	done
	shift
	out=$("$@" 2>&1)
	for p in "${patterns[@]}"; do
		[[ $out == *"$p"* ]] || ok=
	done
	if [ -n "$ok" ]; then
		pass=$((pass + 1))
		printf 'ok    %s\n' "$name"
	else
		fail=$((fail + 1))
		printf 'FAIL  %s\n      wanted: %s\n' "$name" "${patterns[*]}"
		printf '      got:    %s\n' "${out//$'\n'/$'\n'              }"
	fi
}

# check_exit NAME CODE COMMAND...
check_exit() {
	local name=$1 want=$2 got
	shift 2
	"$@" >/dev/null 2>&1
	got=$?
	if [ "$got" = "$want" ]; then
		pass=$((pass + 1))
		printf 'ok    %s\n' "$name"
	else
		fail=$((fail + 1))
		printf 'FAIL  %s\n      wanted exit %s, got %s\n' "$name" "$want" "$got"
	fi
}

if [ ! -x "$G/bin/gcc" ]; then
	echo "gcc for glibc is missing, install it with: pkg install gcc-glibc" >&2
	exit 1
fi

# probe and spawn are built twice: for Termux glibc and, with the loader path
# of a regular Linux system, as stand-ins for downloaded Linux programs.
for prog in probe spawn syscalls; do
	"$G/bin/gcc" -O2 -o "$T/$prog" "$HERE/$prog.c" || exit 1
	"$G/bin/gcc" -O2 -Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1 \
		-o "$T/$prog-linux" "$HERE/$prog.c" || exit 1
done
"$G/bin/gcc" -O2 -static -o "$T/spawn-static" "$HERE/spawn.c" 2>/dev/null
# The same spawn program built for Termux (Bionic), if clang is installed.
"$TERMUX_PREFIX/bin/clang" -O2 --target=aarch64-linux-android28 \
	-o "$T/spawn-termux" "$HERE/spawn.c" 2>/dev/null
# A tiny program for x86_64 PCs that prints "x86 ok".
base64 -d >"$T/x86hello" <<'EOF64'
f0VMRgIBAQAAAAAAAAAAAAIAPgABAAAAeABAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAEAAOAAB
AAAAAAAAAAEAAAAFAAAAAAAAAAAAAAAAAEAAAAAAAAAAQAAAAAAAoAAAAAAAAACgAAAAAAAAAAAQ
AAAAAAAAuAEAAAC/AQAAAEiNNRAAAAC6BwAAAA8FuDwAAAAx/w8FeDg2IG9rCg==
EOF64
chmod +x "$T/x86hello"
HELPER=$TERMUX_PREFIX/lib/libglibct-termux.so

echo "== environment"
check "GLIBCT is set" "GLIBCT=1" -- env
check "LD_PRELOAD is empty" "[]" -- bash -c 'echo "[${LD_PRELOAD-}]"'
check "SHELL is the glibc bash" "$G/bin/bash" -- bash -c 'echo "$SHELL"'
check "glibc programs come first in PATH" "$G/bin:$TERMUX_PREFIX/bin" -- \
	bash -c 'echo "$PATH"'
check "the hook is loaded" "libglibct.so" -- cat /proc/self/maps

echo "== Linux programs"
check "run by path" "exe=$T/probe-linux" "realpath=$T/probe-linux" \
	"argv0=$T/probe-linux" "arg1=a b" -- "$T/probe-linux" "a b"
check "process name is the program" "comm=probe-linux" -- "$T/probe-linux"
check "run from PATH" "argv0=probe-linux" "exe=$T/probe-linux" -- \
	env PATH="$T:$PATH" probe-linux
check "restart through /proc/self/exe" "argv0=reexeced" "arg1=done" -- \
	"$T/probe-linux" reexec
check "Linux program starts Linux program" "exe=$T/probe-linux" -- \
	"$T/spawn-linux" execv "$T/probe-linux"
check "relative path" "exe=$T/probe-linux" -- \
	bash -c "cd '$T' && ./probe-linux"
check "symlink resolves like the kernel does" "argv0=$T/link" \
	"exe=$T/probe-linux" -- bash -c "ln -s probe-linux '$T/link' && '$T/link'"
check_exit "exit code is kept" 3 "$T/spawn-linux" system "exit 3"

echo "== every way to start a program"
for m in execve execv execl execle posix_spawn; do
	check "$m" "exe=$T/probe-linux" -- "$T/spawn" "$m" "$T/probe-linux"
done
for m in execvp execvpe execlp vfork posix_spawnp; do
	check "$m (PATH search)" "exe=$T/probe-linux" -- \
		env PATH="$T:$PATH" "$T/spawn" "$m" probe-linux
done
check "system()" "exe=$T/probe-linux" -- "$T/spawn" system "$T/probe-linux"
check "execvp of a file without #!" "no shebang ran" -- bash -c "
	printf 'echo no shebang ran\n' > '$T/noshebang'
	chmod +x '$T/noshebang'
	PATH='$T:$PATH' '$T/spawn' execvp noshebang"

echo "== scripts"
script() { # script NAME FIRST-LINE BODY
	printf '%s\n%s\n' "$2" "$3" >"$T/$1"
	chmod +x "$T/$1"
}
script sh '#!/bin/sh' 'readlink /proc/$$/exe'
check "#!/bin/sh uses the glibc sh" "$G/bin/bash" -- "$T/sh"
script envbash '#!/usr/bin/env bash' 'readlink /proc/$$/exe'
check "#!/usr/bin/env bash" "$G/bin/bash" -- "$T/envbash"
script envS '#!/usr/bin/env -S bash -e' 'echo "flags=$-"'
check "#!/usr/bin/env -S bash -e" "flags=" "e" -- "$T/envS"
script envquote '#!/usr/bin/env -S bash -c "echo quoted env works"' ''
check "#!/usr/bin/env -S with quotes uses an env program" \
	"quoted env works" -- "$T/envquote"
script envlinux '#!/usr/bin/env probe-linux' ''
check "#!/usr/bin/env with a Linux program" "exe=$T/probe-linux" \
	"arg1=$T/envlinux" -- env PATH="$T:$PATH" "$T/envlinux"
script direct "#!$T/probe-linux --flag" ''
check "Linux program as interpreter" "arg1=--flag" "arg2=$T/direct" \
	"arg3=x" -- "$T/direct" x
printf '#!/bin/sh\r\necho crlf works\r\n' >"$T/crlf"
chmod +x "$T/crlf"
check "#! line with CRLF" "crlf works" -- "$T/crlf"
script loop1 "#!$T/loop2" ''
script loop2 "#!$T/loop1" ''
check "#! loop stops" "Too many levels" -- "$T/spawn" execv "$T/loop1"
script termux "#!$TERMUX_PREFIX/bin/sh" 'echo "termux sh: ${LD_PRELOAD-none}"'
check "#! with a Termux shell" \
	"termux sh: $HELPER:$GLIBCT_TERMUX_LD_PRELOAD" -- "$T/termux"

echo "== Termux (Bionic) programs"
check "get the Termux LD_PRELOAD back, with the helper in front" \
	"LD_PRELOAD=$HELPER:$GLIBCT_TERMUX_LD_PRELOAD" -- \
	"$TERMUX_PREFIX/bin/env"
check "get a PATH without glibc" "path ok" -- "$TERMUX_PREFIX/bin/bash" -c \
	"case :\$PATH: in *:$G/bin:*) echo bad;; *) echo path ok;; esac"
check "get the Termux SHELL back" "SHELL=$GLIBCT_TERMUX_SHELL" -- \
	"$TERMUX_PREFIX/bin/env"
check "start Termux programs" "$TERMUX_PREFIX/bin/cat" -- \
	"$TERMUX_PREFIX/bin/bash" -c 'type -P cat'
check "pkg works" "pkg" "install" -- pkg help
if [ -x "$T/spawn-static" ]; then
	check_exit "static program starts a Termux program" 0 \
		"$T/spawn-static" execv "$TERMUX_PREFIX/bin/true"
fi

check "Termux scripts still use Termux programs" "$TERMUX_PREFIX/bin/bash" -- \
	"$TERMUX_PREFIX/bin/bash" -c "'$T/envbash'"

echo "== Termux programs that start glibc programs"
check "start a glibc program" "$G/bin/bash" -- "$TERMUX_PREFIX/bin/bash" -c \
	"$G/bin/bash -c 'readlink /proc/\$\$/exe; true'"
check "the glibc program gets the glibc environment back" \
	"$G/bin:$TERMUX_PREFIX/bin" "SHELL=$G/bin/bash" "LD_PRELOAD=[]" -- \
	"$TERMUX_PREFIX/bin/bash" -c "$G/bin/bash -c 'echo \"PATH=\$PATH\"; \
	echo SHELL=\$SHELL; echo \"LD_PRELOAD=[\${LD_PRELOAD-}]\"'"
check "start a Linux program" "exe=$T/probe-linux" -- \
	"$TERMUX_PREFIX/bin/bash" -c "'$T/probe-linux'"
if [ -x "$T/spawn-termux" ]; then
	for m in execv posix_spawn; do
		check "$m" "exe=$T/probe-linux" -- \
			"$T/spawn-termux" "$m" "$T/probe-linux"
	done
	for m in execvp vfork posix_spawnp; do
		check "$m (PATH search)" "exe=$T/probe-linux" -- \
			env PATH="$T:$PATH" "$T/spawn-termux" "$m" probe-linux
	done
fi
if [ -x "$TERMUX_PREFIX/bin/make" ]; then
	mkdir "$T/mk"
	printf 'int main(void) { return 42; }\n' >"$T/mk/hello.c"
	printf 'hello: hello.c\n\t%s -o hello hello.c\n' "$G/bin/gcc" \
		>"$T/mk/Makefile"
	check_exit "Termux make runs the glibc gcc" 42 "$TERMUX_PREFIX/bin/bash" -c \
		"PATH='$TERMUX_PREFIX/bin' '$TERMUX_PREFIX/bin/make' -s -C '$T/mk' && '$T/mk/hello'"
fi

echo "== system calls that Android blocks"
for prog in syscalls syscalls-linux; do
	check "$prog gets 'not supported' instead of being killed" \
		"syscall 437: ret=-1 Function not implemented" \
		"syscall 451: ret=-1 Function not implemented" \
		"syscall 289: ret=-1 Function not implemented" -- "$T/$prog"
done

echo "== programs for x86_64 PCs"
if [ -x "$G/bin/box64" ]; then
	check "run with box64" "x86 ok" -- "$T/x86hello"
	check "run with box64 from a Termux program" "x86 ok" -- \
		"$TERMUX_PREFIX/bin/bash" -c "'$T/x86hello'"
else
	check_exit "without box64 they do not run" 126 "$T/x86hello"
fi

echo "== the glibct command"
check "--doctor" "Everything glibct needs is in place" -- glibct --doctor
check "arguments are passed unchanged" "a b|c|" -- glibct printf '%s|' 'a b' c
check_exit "exit code" 7 glibct sh -c 'exit 7'
check_exit "unknown command" 127 glibct glibct-no-such-command
check "-c" "x=1" -- glibct -c 'echo "x=$1"' sh 1
check "nested glibct keeps the Termux values" \
	"nested $GLIBCT_TERMUX_LD_PRELOAD" -- \
	glibct -c 'glibct -c "echo nested \$GLIBCT_TERMUX_LD_PRELOAD"'

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
