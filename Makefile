PREFIX ?= /data/data/com.termux/files/usr
GLIBC_PREFIX = $(PREFIX)/glibc
# libglibct.so is built for glibc, libglibct-termux.so for Android (Bionic).
CC = $(GLIBC_PREFIX)/bin/gcc
TERMUX_CC = $(PREFIX)/bin/clang
# Build with the glibc tools, also from a normal Termux shell. Termux's
# LD_PRELOAD holds Android libraries that glibc programs cannot load.
export PATH := $(GLIBC_PREFIX)/bin:$(PATH)
unexport LD_PRELOAD
CFLAGS = -O2 -Wall -Wextra
HOOK_FLAGS = -shared -fPIC -fvisibility=hidden -U_FORTIFY_SOURCE
HOOKS = build/libglibct.so build/libglibct-termux.so
DIST = dist/glibct

all: $(HOOKS)

build/libglibct.so: src/glibc_hook.c src/common.c src/common.h
	mkdir -p build
	$(CC) $(CFLAGS) $(HOOK_FLAGS) -o $@ src/glibc_hook.c src/common.c

build/libglibct-termux.so: src/termux_hook.c src/common.c src/common.h
	mkdir -p build
	$(TERMUX_CC) $(CFLAGS) $(HOOK_FLAGS) -o $@ src/termux_hook.c src/common.c

# Files are replaced with mv: running programs keep the old file.
# glibct turns the hook on itself the first time it runs.
install: $(HOOKS)
	install -Dm755 bin/glibct $(PREFIX)/bin/glibct
	install -Dm644 share/bashrc $(PREFIX)/share/glibct/bashrc
	install -Dm755 build/libglibct-termux.so $(PREFIX)/lib/libglibct-termux.so.new
	mv -f $(PREFIX)/lib/libglibct-termux.so.new $(PREFIX)/lib/libglibct-termux.so
	install -Dm755 build/libglibct.so $(GLIBC_PREFIX)/lib/libglibct.so.new
	mv -f $(GLIBC_PREFIX)/lib/libglibct.so.new $(GLIBC_PREFIX)/lib/libglibct.so

dist: $(HOOKS)
	rm -rf dist
	install -Dm755 bin/glibct $(DIST)/bin/glibct
	install -Dm644 share/bashrc $(DIST)/share/bashrc
	install -Dm755 build/libglibct.so $(DIST)/lib/libglibct.so
	install -Dm755 build/libglibct-termux.so $(DIST)/lib/libglibct-termux.so
	install -Dm644 README.md $(DIST)/README.md
	install -Dm644 CHANGELOG.md $(DIST)/CHANGELOG.md
	install -Dm644 LICENSE $(DIST)/LICENSE
	tar -C dist -czf dist/glibct-aarch64.tar.gz --owner=0 --group=0 glibct
	cd dist && sha256sum glibct-aarch64.tar.gz > SHA256SUMS

clean:
	rm -rf build dist

.PHONY: all install dist clean
