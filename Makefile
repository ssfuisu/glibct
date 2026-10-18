PREFIX ?= /data/data/com.termux/files/usr
GLIBC_PREFIX = $(PREFIX)/glibc
CC = $(GLIBC_PREFIX)/bin/gcc
# Build with the glibc tools, also from a normal Termux shell. Termux's
# LD_PRELOAD holds Android libraries that glibc programs cannot load.
export PATH := $(GLIBC_PREFIX)/bin:$(PATH)
unexport LD_PRELOAD
CFLAGS = -O2 -Wall -Wextra
HOOK_FLAGS = -shared -fPIC -U_FORTIFY_SOURCE
DIST = dist/glibct

all: build/libglibct.so

build/libglibct.so: src/libglibct.c
	mkdir -p build
	$(CC) $(CFLAGS) $(HOOK_FLAGS) -o $@ src/libglibct.c

# glibct turns the hook on itself the first time it runs.
install: build/libglibct.so
	install -Dm755 bin/glibct $(PREFIX)/bin/glibct
	install -Dm644 share/bashrc $(PREFIX)/share/glibct/bashrc
	install -Dm755 build/libglibct.so $(GLIBC_PREFIX)/lib/libglibct.so.new
	mv -f $(GLIBC_PREFIX)/lib/libglibct.so.new $(GLIBC_PREFIX)/lib/libglibct.so

dist: build/libglibct.so
	rm -rf dist
	install -Dm755 bin/glibct $(DIST)/bin/glibct
	install -Dm644 share/bashrc $(DIST)/share/bashrc
	install -Dm755 build/libglibct.so $(DIST)/lib/libglibct.so
	install -Dm644 README.md $(DIST)/README.md
	install -Dm644 LICENSE $(DIST)/LICENSE
	tar -C dist -czf dist/glibct-aarch64.tar.gz --owner=0 --group=0 glibct
	cd dist && sha256sum glibct-aarch64.tar.gz > SHA256SUMS

clean:
	rm -rf build dist

.PHONY: all install dist clean
