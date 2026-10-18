/*
 * libglibct.so - starts programs the right way inside Termux's glibc world.
 *
 * Termux keeps glibc in $PREFIX/glibc. Programs built for it start fine, but
 * programs built for regular Linux distributions ask for a loader such as
 * /lib/ld-linux-aarch64.so.1, and scripts ask for /bin/sh or /usr/bin/env.
 * None of these paths exist on Android.
 *
 * glibct lists this library in $PREFIX/glibc/etc/ld.so.preload. Only the
 * glibc loader reads that file, so every glibc program loads the library and
 * Android's own linker never sees it. The library wraps the exec and
 * posix_spawn functions. For every program that is started it:
 *
 *   - runs Linux glibc programs through Termux's glibc loader,
 *   - maps missing /bin, /usr/bin, /usr/local/bin and /sbin paths to
 *     $PREFIX/glibc/bin or $PREFIX/bin,
 *   - reads #! lines itself so the interpreter gets the same treatment,
 *   - gives Android (Bionic) programs the normal Termux environment back.
 *
 * A program started through the loader sees the loader in /proc/self/exe,
 * so readlink() and realpath() of that path return the real program instead.
 *
 * The exec path keeps its state on the stack and calls malloc only for very
 * long argument lists, so it can run in a vfork() child.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TERMUX_PREFIX
#define TERMUX_PREFIX "/data/data/com.termux/files/usr"
#endif
#define GLIBC_PREFIX TERMUX_PREFIX "/glibc"

#if !defined(__aarch64__)
#error "glibct supports aarch64 only"
#endif
#define LOADER GLIBC_PREFIX "/lib/ld-linux-aarch64.so.1"

#define MAX_DEPTH 4    /* nested #! interpreters, same limit as Linux */
#define MAX_WORDS 8    /* words after "#!/usr/bin/env -S" */
#define MAX_PRE (MAX_DEPTH * (MAX_WORDS + 2) + 8)
#define PATH_BUF 1024
#define LINE_BUF 256   /* Linux reads at most 256 bytes of a #! line */
#define STACK_ARGS 256 /* longer argv or envp arrays use malloc */

extern char **environ;

static int (*real_execve)(const char *, char *const[], char *const[]);
static int (*real_posix_spawn)(pid_t *, const char *,
			       const posix_spawn_file_actions_t *,
			       const posix_spawnattr_t *, char *const[],
			       char *const[]);
static ssize_t (*real_readlink)(const char *, char *, size_t);
static ssize_t (*real_readlinkat)(int, const char *, char *, size_t);
static char *(*real_realpath)(const char *, char *);

static int debug;
/* Real path of this program when it was started through the loader. */
static char self_exe[PATH_MAX];

static void resolve_real(void)
{
	if (!real_execve)
		real_execve = dlsym(RTLD_NEXT, "execve");
	if (!real_posix_spawn)
		real_posix_spawn = dlsym(RTLD_NEXT, "posix_spawn");
	if (!real_readlink)
		real_readlink = dlsym(RTLD_NEXT, "readlink");
	if (!real_readlinkat)
		real_readlinkat = dlsym(RTLD_NEXT, "readlinkat");
	if (!real_realpath)
		real_realpath = dlsym(RTLD_NEXT, "realpath");
}

/*
 * The headers mark file arguments as never NULL, so the compiler would drop
 * a plain NULL check. Programs still pass NULL sometimes; keep the check.
 */
static int is_null(const void *p)
{
	__asm__("" : "+r"(p));
	return p == NULL;
}

static int starts_with(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* GLIBCT_DEBUG=1 prints one line per started program to stderr. */
static void log_route(const char *from, const char *to, const char *kind)
{
	const char *parts[] = { "glibct: ", from, " -> ", to, " (", kind,
				")\n" };
	char line[PATH_BUF * 2 + 64];
	size_t n = 0;

	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		size_t len = strlen(parts[i]);
		if (n + len >= sizeof(line))
			len = sizeof(line) - n - 1;
		memcpy(line + n, parts[i], len);
		n += len;
	}
	if (write(2, line, n) < 0)
		return;
}

/* ---- /proc/self/exe ---------------------------------------------------- */

static int is_self_exe(const char *path)
{
	char *end;
	long pid;

	if (!self_exe[0] || !path || !starts_with(path, "/proc/"))
		return 0;
	path += 6;
	if (!strcmp(path, "self/exe") || !strcmp(path, "thread-self/exe"))
		return 1;
	pid = strtol(path, &end, 10);
	return end != path && !strcmp(end, "/exe") && pid == getpid();
}

/*
 * When the loader runs a program ("ld.so [options] program args"), the
 * program path is the first argument after the loader options.
 */
static void find_self_exe(void)
{
	static const char *const with_value[] = {
		"--argv0", "--library-path", "--preload", "--audit",
		"--inhibit-rpath", "--glibc-hwcaps-prepend",
		"--glibc-hwcaps-mask", NULL
	};
	char buf[8192], *arg, *end, *name;
	ssize_t len = 0, n;
	int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	while (len < (ssize_t)sizeof(buf) - 1 &&
	       (n = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0)
		len += n;
	close(fd);
	buf[len] = '\0';

	end = buf + len;
	arg = buf + strlen(buf) + 1; /* skip the loader itself */
	while (arg < end && starts_with(arg, "--")) {
		int takes_value = 0;
		for (int i = 0; with_value[i]; i++)
			if (!strcmp(arg, with_value[i]))
				takes_value = 1;
		arg += strlen(arg) + 1;
		if (takes_value && arg < end)
			arg += strlen(arg) + 1;
	}
	if (arg >= end || !*arg)
		return;
	if (!real_realpath || !real_realpath(arg, self_exe)) {
		self_exe[0] = '\0';
		return;
	}
	/* Like the kernel: the name of the path that was started, so top,
	 * ps and pkill show the program instead of the loader. */
	name = strrchr(arg, '/');
	prctl(PR_SET_NAME, name ? name + 1 : arg, 0, 0, 0);
}

__attribute__((constructor)) static void glibct_init(void)
{
	const char *d = getenv("GLIBCT_DEBUG");
	char exe[PATH_BUF];
	ssize_t n;

	resolve_real();
	debug = d && *d && strcmp(d, "0");
	if (!real_readlink)
		return;
	n = real_readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n != (ssize_t)strlen(LOADER) || memcmp(exe, LOADER, n))
		return;
	find_self_exe();
}

static ssize_t copy_self_exe(char *buf, size_t size)
{
	size_t len = strlen(self_exe);

	if (len > size)
		len = size;
	memcpy(buf, self_exe, len);
	return len;
}

ssize_t readlink(const char *path, char *buf, size_t size)
{
	if (is_self_exe(path))
		return copy_self_exe(buf, size);
	resolve_real();
	return real_readlink(path, buf, size);
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t size)
{
	if (is_self_exe(path))
		return copy_self_exe(buf, size);
	resolve_real();
	return real_readlinkat(dirfd, path, buf, size);
}

ssize_t __readlink_chk(const char *path, char *buf, size_t size,
		       size_t buflen)
{
	(void)buflen;
	return readlink(path, buf, size);
}

ssize_t __readlinkat_chk(int dirfd, const char *path, char *buf, size_t size,
			 size_t buflen)
{
	(void)buflen;
	return readlinkat(dirfd, path, buf, size);
}

char *realpath(const char *path, char *resolved)
{
	if (is_self_exe(path))
		return resolved ? strcpy(resolved, self_exe) : strdup(self_exe);
	resolve_real();
	return real_realpath(path, resolved);
}

char *__realpath_chk(const char *path, char *resolved, size_t resolvedlen)
{
	(void)resolvedlen;
	return realpath(path, resolved);
}

char *canonicalize_file_name(const char *path)
{
	return realpath(path, NULL);
}

/* ---- looking at the file that is about to run ------------------------- */

enum kind {
	KIND_OTHER,  /* run unchanged: static programs, unknown files */
	KIND_GLIBC,  /* built for Termux glibc */
	KIND_LINUX,  /* built for a regular Linux system, needs the loader */
	KIND_BIONIC, /* built for Android */
	KIND_SCRIPT, /* starts with #! */
};

static const char *const kind_names[] = {
	"other", "glibc", "linux", "bionic", "script"
};

struct probe {
	enum kind kind;
	char line[LINE_BUF];  /* script: interpreter, NUL, optional argument */
	char *arg;
	char words[LINE_BUF]; /* the argument split into words */
};

static void probe_elf(int fd, const Elf64_Ehdr *eh, struct probe *p)
{
	char interp[PATH_BUF];
	Elf64_Phdr ph = { .p_type = PT_NULL };
	const char *base;
	ssize_t n;

	if (eh->e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh->e_machine != EM_AARCH64 ||
	    eh->e_phentsize != sizeof(ph) || eh->e_phnum > 128)
		return;
	for (int i = 0; i < eh->e_phnum; i++) {
		off_t off = eh->e_phoff + (off_t)i * sizeof(ph);
		if (pread(fd, &ph, sizeof(ph), off) != sizeof(ph))
			return;
		/* PT_INTERP comes before the first PT_LOAD. */
		if (ph.p_type == PT_INTERP || ph.p_type == PT_LOAD)
			break;
	}
	if (ph.p_type != PT_INTERP || ph.p_filesz < 2 ||
	    ph.p_filesz > sizeof(interp))
		return; /* static program */
	n = pread(fd, interp, ph.p_filesz, ph.p_offset);
	if (n != (ssize_t)ph.p_filesz)
		return;
	interp[n - 1] = '\0';

	if (!strcmp(interp, LOADER)) {
		p->kind = KIND_GLIBC;
	} else if (starts_with(interp, "/system/")) {
		p->kind = KIND_BIONIC;
	} else if (access(interp, F_OK) != 0) {
		base = strrchr(interp, '/');
		base = base ? base + 1 : interp;
		if (starts_with(base, "ld-linux"))
			p->kind = KIND_LINUX;
	}
}

/* Splits "#! interpreter [argument]" the way Linux does. */
static void probe_script(char *s, char *end, struct probe *p)
{
	char *nl = memchr(s, '\n', end - s), *interp, *arg = NULL;
	size_t ilen, alen = 0;

	if (nl)
		end = nl;
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
			   end[-1] == '\r'))
		end--;
	*end = '\0';
	while (*s == ' ' || *s == '\t')
		s++;
	interp = s;
	while (*s && *s != ' ' && *s != '\t')
		s++;
	if (s == interp)
		return;
	if (*s) {
		*s++ = '\0';
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s)
			arg = s;
	}

	ilen = strlen(interp);
	if (arg)
		alen = strlen(arg);
	if (ilen + alen + 2 > sizeof(p->line))
		return;
	memcpy(p->line, interp, ilen + 1);
	p->arg = NULL;
	if (arg) {
		p->arg = p->line + ilen + 1;
		memcpy(p->arg, arg, alen + 1);
	}
	p->kind = KIND_SCRIPT;
}

static void probe_file(const char *path, struct probe *p)
{
	char buf[LINE_BUF + 1];
	ssize_t n;
	int fd;

	p->kind = KIND_OTHER;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	n = pread(fd, buf, LINE_BUF, 0);
	if (n >= (ssize_t)sizeof(Elf64_Ehdr) && !memcmp(buf, ELFMAG, SELFMAG)) {
		Elf64_Ehdr eh;
		memcpy(&eh, buf, sizeof(eh));
		probe_elf(fd, &eh, p);
	} else if (n > 2 && buf[0] == '#' && buf[1] == '!') {
		buf[n] = '\0';
		probe_script(buf + 2, buf + n, p);
	}
	close(fd);
}

/* ---- paths ----------------------------------------------------------- */

static int is_exec_file(const char *path)
{
	struct stat st;

	return access(path, X_OK) == 0 && stat(path, &st) == 0 &&
	       S_ISREG(st.st_mode);
}

static int join(char *out, size_t size, const char *dir, size_t dirlen,
		const char *name)
{
	size_t len = strlen(name);

	if (!dirlen) {
		dir = ".";
		dirlen = 1;
	}
	if (dirlen + 1 + len + 1 > size)
		return -1;
	memcpy(out, dir, dirlen);
	out[dirlen] = '/';
	memcpy(out + dirlen + 1, name, len + 1);
	return 0;
}

/*
 * /usr/bin/env and friends do not exist on Android, and /bin is either
 * missing or points to /system/bin. Use the glibc program of the same name,
 * or the Termux one. Like termux-exec, /bin and /usr/bin are always mapped;
 * the other directories only when the file is missing.
 */
static const char *map_missing(const char *path, char *out, size_t size)
{
	static const struct {
		const char *dir;
		int always;
	} dirs[] = {
		{ "/bin/", 1 }, { "/usr/bin/", 1 }, { "/usr/local/bin/", 0 },
		{ "/sbin/", 0 }, { "/usr/sbin/", 0 }, { "/usr/local/sbin/", 0 },
	};
	static const char *const targets[] = {
		GLIBC_PREFIX "/bin", TERMUX_PREFIX "/bin", NULL
	};
	const char *name = NULL;
	int always = 0;

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		if (starts_with(path, dirs[i].dir)) {
			name = path + strlen(dirs[i].dir);
			always = dirs[i].always;
		}
	if (!name || !*name || strchr(name, '/') ||
	    (!always && access(path, F_OK) == 0))
		return NULL;
	for (int i = 0; targets[i]; i++)
		if (join(out, size, targets[i], strlen(targets[i]), name) == 0 &&
		    is_exec_file(out))
			return out;
	return NULL;
}

static const char *env_get(char *const envp[], const char *key)
{
	size_t len = strlen(key);

	for (int i = 0; envp && envp[i]; i++)
		if (!strncmp(envp[i], key, len) && envp[i][len] == '=')
			return envp[i] + len + 1;
	return NULL;
}

static const char *default_path(const char *list)
{
	return list ? list : GLIBC_PREFIX "/bin:" TERMUX_PREFIX "/bin";
}

/* Finds name in a PATH list. Returns 0, ENOENT or EACCES. */
static int path_search(const char *name, const char *list, char *out,
		       size_t size)
{
	int err = ENOENT;

	for (const char *p = default_path(list);; p++) {
		const char *colon = strchr(p, ':');
		size_t len = colon ? (size_t)(colon - p) : strlen(p);
		if (join(out, size, p, len, name) == 0 &&
		    access(out, F_OK) == 0) {
			if (is_exec_file(out))
				return 0;
			err = EACCES;
		}
		if (!colon)
			return err;
		p = colon;
	}
}

/* ---- deciding how to start a program --------------------------------- */

struct route {
	const char *file;         /* what gets passed to the real exec */
	const char *pre[MAX_PRE]; /* new start of argv ... */
	int npre;
	char *const *tail;        /* ... followed by these */
	int rewritten;            /* file or argv changed */
	enum kind kind;           /* decides the environment */
	char mapped[MAX_DEPTH + 1][PATH_BUF];
	char found[MAX_DEPTH + 1][PATH_BUF];
	struct probe probes[MAX_DEPTH + 1];
};

/* Replaces argv[0] (if there is one) with the given items. */
static int replace_argv0(struct route *r, const char *const *items,
			 int count)
{
	int drop = r->npre ? 1 : 0;

	if (r->npre - drop + count > MAX_PRE)
		return -1;
	memmove(r->pre + count, r->pre + drop,
		(r->npre - drop) * sizeof(r->pre[0]));
	memcpy(r->pre, items, count * sizeof(items[0]));
	r->npre += count - drop;
	return 0;
}

/*
 * "#!/usr/bin/env name" when /usr/bin/env does not exist: look up name
 * ourselves. Returns 0 if handled, -1 to fall back to an env program.
 */
static int env_script(struct route *r, struct probe *p, const char *script,
		      char *const envp[], char *out)
{
	const char *words[MAX_WORDS + 1];
	int count = 0;
	char *s = p->words;

	if (!p->arg)
		return -1;
	/* Split a copy: p->arg is still needed if this falls back. */
	strcpy(p->words, p->arg);
	if (starts_with(s, "-S") && (s[2] == ' ' || s[2] == '\t')) {
		for (s += 3; *s;) {
			while (*s == ' ' || *s == '\t')
				*s++ = '\0';
			if (!*s)
				break;
			if (count == MAX_WORDS)
				return -1;
			words[count++] = s;
			while (*s && *s != ' ' && *s != '\t')
				s++;
		}
	} else {
		words[count++] = s;
	}
	if (!count || words[0][0] == '-')
		return -1;
	for (int i = 0; i < count; i++)
		if (strpbrk(words[i], " \t\"'\\$"))
			return -1;
	if (strchr(words[0], '/')) {
		if (strlen(words[0]) >= PATH_BUF)
			return -1;
		strcpy(out, words[0]);
	} else if (path_search(words[0], env_get(envp, "PATH"), out,
			       PATH_BUF) != 0) {
		return -1;
	}
	words[count++] = script;
	if (replace_argv0(r, words, count) != 0)
		return -1;
	r->file = out;
	return 0;
}

static int route_exec(struct route *r, const char *file, char *const argv[],
		      char *const envp[])
{
	r->file = file;
	r->npre = 0;
	r->tail = argv;
	r->rewritten = 0;
	r->kind = KIND_OTHER;
	if (argv && argv[0]) {
		r->pre[r->npre++] = argv[0];
		r->tail = argv + 1;
	}
	if (is_self_exe(file)) {
		r->file = self_exe;
		r->rewritten = 1;
	}

	for (int depth = 0;; depth++) {
		struct probe *p = &r->probes[depth];
		const char *mapped = map_missing(r->file, r->mapped[depth],
						 PATH_BUF);
		if (mapped) {
			if (r->npre && !strcmp(r->pre[0], r->file))
				r->pre[0] = mapped;
			r->file = mapped;
			r->rewritten = 1;
		}
		if (access(r->file, X_OK) != 0)
			return 0; /* the real exec reports the error */
		probe_file(r->file, p);
		r->kind = p->kind;

		if (p->kind == KIND_LINUX) {
			const char *items[] = {
				LOADER, "--argv0", r->npre ? r->pre[0] : r->file,
				r->file
			};
			if (replace_argv0(r, items, 4) != 0)
				return -1;
			r->file = LOADER;
			r->rewritten = 1;
			return 0;
		}
		if (p->kind != KIND_SCRIPT)
			return 0;
		if (depth == MAX_DEPTH) {
			errno = ELOOP;
			return -1;
		}

		/* argv becomes: interpreter [argument] script args... */
		const char *script = r->file;
		const char *interp = p->line;
		const char *base = strrchr(interp, '/');
		base = base ? base + 1 : interp;
		if (!strcmp(base, "env") && access(interp, F_OK) != 0 &&
		    env_script(r, p, script, envp, r->found[depth]) == 0) {
			r->rewritten = 1;
			continue;
		}
		const char *with_arg[] = { interp, p->arg, script };
		const char *without_arg[] = { interp, script };
		if (p->arg ? replace_argv0(r, with_arg, 3)
			   : replace_argv0(r, without_arg, 2))
			return -1;
		r->file = interp;
	}
}

/*
 * When nothing had to be changed, the original file and argv are used, so
 * the kernel runs scripts exactly as it always does.
 */
static const char *route_file(const struct route *r, const char *file)
{
	return r->rewritten ? r->file : file;
}

/* ---- environment ----------------------------------------------------- */

struct envbuf {
	char **list;
	char *stack[STACK_ARGS];
	char preload[1024];
	char path[4096];
	char ldpath[2048];
	char shell[PATH_BUF];
};

/*
 * Writes "KEY=" plus the colon or space separated entries of value that
 * keep() accepts. Returns -1 if out is too small, 0 if no entry is left.
 */
static int filter_list(char *out, size_t size, const char *key,
		       const char *value, int (*keep)(const char *, size_t))
{
	size_t n = strlen(key);
	int kept = 0;

	if (n + 2 > size)
		return -1;
	memcpy(out, key, n);
	out[n++] = '=';
	for (const char *p = value; *p;) {
		size_t len = strcspn(p, ": ");
		if (len && keep(p, len)) {
			if (n + len + 2 > size)
				return -1;
			if (kept++)
				out[n++] = ':';
			memcpy(out + n, p, len);
			n += len;
		}
		p += len;
		if (*p)
			p++;
	}
	out[n] = '\0';
	return kept ? 1 : 0;
}

static int not_bionic_lib(const char *s, size_t len)
{
	static const char *const dirs[] = {
		TERMUX_PREFIX "/lib/", "/system/", "/apex/", "/vendor/", NULL
	};
	(void)len;
	for (int i = 0; dirs[i]; i++)
		if (starts_with(s, dirs[i]))
			return 0;
	return 1;
}

static int not_glibc_dir(const char *s, size_t len)
{
	size_t plen = strlen(GLIBC_PREFIX);

	return !(len >= plen && !strncmp(s, GLIBC_PREFIX, plen) &&
		 (len == plen || s[plen] == '/'));
}

static int set_entry(char *out, size_t size, const char *key,
		     const char *value)
{
	size_t k = strlen(key), v = strlen(value);

	if (k + v + 2 > size)
		return -1;
	memcpy(out, key, k);
	out[k] = '=';
	memcpy(out + k + 1, value, v + 1);
	return 0;
}

static int is_key(const char *entry, const char *key)
{
	size_t len = strlen(key);

	return !strncmp(entry, key, len) && entry[len] == '=';
}

/*
 * Returns the environment for a program of the given kind. glibc programs
 * must not get Android libraries in LD_PRELOAD and Android programs must not
 * get glibc libraries there: both linkers stop with an error otherwise.
 * Inside glibct (GLIBCT=1) Android programs also get the Termux values of
 * LD_PRELOAD, LD_LIBRARY_PATH, PATH and SHELL back.
 */
static char *const *make_env(char *const envp[], enum kind kind,
			     struct envbuf *b)
{
	const char *preload = env_get(envp, "LD_PRELOAD");
	const char *active = env_get(envp, "GLIBCT");
	char *new_preload = NULL, *new_path = NULL, *new_ldpath = NULL;
	char *new_shell = NULL;
	int drop_preload = 1, drop_ldpath = 0, count = 0, n = 0;

	if (kind == KIND_BIONIC && active && !strcmp(active, "1")) {
		const char *v = env_get(envp, "GLIBCT_TERMUX_LD_PRELOAD");
		const char *path = env_get(envp, "PATH");
		if (v && *v &&
		    set_entry(b->preload, sizeof(b->preload), "LD_PRELOAD",
			      v) == 0)
			new_preload = b->preload;
		drop_ldpath = 1;
		v = env_get(envp, "GLIBCT_TERMUX_LD_LIBRARY_PATH");
		if (v && set_entry(b->ldpath, sizeof(b->ldpath),
				   "LD_LIBRARY_PATH", v) == 0)
			new_ldpath = b->ldpath;
		if (path && filter_list(b->path, sizeof(b->path), "PATH", path,
					not_glibc_dir) >= 0)
			new_path = b->path;
		v = env_get(envp, "GLIBCT_TERMUX_SHELL");
		if (v && *v &&
		    set_entry(b->shell, sizeof(b->shell), "SHELL", v) == 0)
			new_shell = b->shell;
	} else if (preload && kind != KIND_OTHER && kind != KIND_SCRIPT) {
		int left = filter_list(b->preload, sizeof(b->preload),
				       "LD_PRELOAD", preload,
				       kind == KIND_BIONIC ? not_glibc_dir
							   : not_bionic_lib);
		if (left < 0 || !strcmp(b->preload + strlen("LD_PRELOAD="),
					preload))
			return envp; /* nothing to change */
		if (left > 0)
			new_preload = b->preload;
	} else {
		return envp;
	}

	while (envp && envp[count])
		count++;
	b->list = b->stack;
	if (count + 3 > STACK_ARGS) {
		b->list = malloc((count + 3) * sizeof(char *));
		if (!b->list)
			return envp;
	}
	for (int i = 0; i < count; i++) {
		char *e = envp[i];
		if (drop_preload && is_key(e, "LD_PRELOAD"))
			continue;
		if (drop_ldpath && is_key(e, "LD_LIBRARY_PATH"))
			continue;
		if (new_path && is_key(e, "PATH"))
			e = new_path;
		if (new_shell && is_key(e, "SHELL"))
			e = new_shell;
		b->list[n++] = e;
	}
	if (new_preload)
		b->list[n++] = new_preload;
	if (new_ldpath)
		b->list[n++] = new_ldpath;
	b->list[n] = NULL;
	return b->list;
}

/* ---- argv ------------------------------------------------------------ */

struct argbuf {
	char **list;
	char *stack[STACK_ARGS];
};

static char *const *make_argv(const struct route *r, char *const argv[],
			      struct argbuf *b)
{
	int ntail = 0;

	if (!r->rewritten)
		return argv;
	while (r->tail && r->tail[ntail])
		ntail++;
	b->list = b->stack;
	if (r->npre + ntail + 1 > STACK_ARGS) {
		b->list = malloc((r->npre + ntail + 1) * sizeof(char *));
		if (!b->list)
			return NULL;
	}
	memcpy(b->list, r->pre, r->npre * sizeof(char *));
	memcpy(b->list + r->npre, r->tail, ntail * sizeof(char *));
	b->list[r->npre + ntail] = NULL;
	return b->list;
}

static void free_list(char **list, char **stack)
{
	if (list && list != stack)
		free(list);
}

/* ---- exec and spawn wrappers ----------------------------------------- */

int execve(const char *file, char *const argv[], char *const envp[])
{
	struct route r;
	struct envbuf eb;
	struct argbuf ab;
	char *const *av, *const *ev;
	int ret, saved;

	resolve_real();
	if (is_null(file))
		return real_execve(file, argv, envp);
	if (route_exec(&r, file, argv, envp) != 0)
		return -1;
	eb.list = ab.list = NULL;
	av = make_argv(&r, argv, &ab);
	if (!av) {
		errno = ENOMEM;
		return -1;
	}
	ev = make_env(envp, r.kind, &eb);
	if (debug)
		log_route(file, route_file(&r, file), kind_names[r.kind]);
	ret = real_execve(route_file(&r, file), av, ev);
	saved = errno;
	free_list(ab.list, ab.stack);
	free_list(eb.list, eb.stack);
	errno = saved;
	return ret;
}

int execv(const char *file, char *const argv[])
{
	return execve(file, argv, environ);
}

/* Runs a file that is neither a program nor a #! script with sh. */
static int exec_with_sh(const char *path, char *const argv[],
			char *const envp[])
{
	int argc = 0;

	while (argv && argv[argc])
		argc++;
	char *sh_argv[argc + 3];
	sh_argv[0] = "sh";
	sh_argv[1] = (char *)path;
	for (int i = 1; i < argc; i++)
		sh_argv[i + 1] = argv[i];
	sh_argv[argc > 0 ? argc + 1 : 2] = NULL;
	execve(GLIBC_PREFIX "/bin/sh", sh_argv, envp);
	execve(TERMUX_PREFIX "/bin/sh", sh_argv, envp);
	errno = ENOEXEC;
	return -1;
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
	char path[PATH_BUF];
	int eacces = 0;

	if (is_null(file) || !*file) {
		errno = ENOENT;
		return -1;
	}
	if (strchr(file, '/')) {
		execve(file, argv, envp);
		return errno == ENOEXEC ? exec_with_sh(file, argv, envp) : -1;
	}
	for (const char *p = default_path(getenv("PATH"));; p++) {
		const char *colon = strchr(p, ':');
		size_t len = colon ? (size_t)(colon - p) : strlen(p);
		if (join(path, sizeof(path), p, len, file) == 0) {
			execve(path, argv, envp);
			if (errno == ENOEXEC)
				return exec_with_sh(path, argv, envp);
			if (errno == EACCES)
				eacces = 1;
			else if (errno != ENOENT && errno != ENOTDIR &&
				 errno != ESTALE && errno != ENODEV &&
				 errno != ETIMEDOUT)
				return -1;
		}
		if (!colon)
			break;
		p = colon;
	}
	errno = eacces ? EACCES : ENOENT;
	return -1;
}

int execvp(const char *file, char *const argv[])
{
	return execvpe(file, argv, environ);
}

/* execl, execlp and execle collect their arguments into an array first. */
static int count_args(const char *arg, va_list ap)
{
	int argc = 0;

	for (; arg; arg = va_arg(ap, const char *))
		argc++;
	return argc;
}

int execl(const char *file, const char *arg, ...)
{
	va_list ap;
	int argc;

	va_start(ap, arg);
	argc = count_args(arg, ap);
	va_end(ap);
	char *argv[argc + 1];
	argv[0] = (char *)arg;
	va_start(ap, arg);
	for (int i = 1; i <= argc; i++)
		argv[i] = va_arg(ap, char *);
	va_end(ap);
	return execve(file, argv, environ);
}

int execlp(const char *file, const char *arg, ...)
{
	va_list ap;
	int argc;

	va_start(ap, arg);
	argc = count_args(arg, ap);
	va_end(ap);
	char *argv[argc + 1];
	argv[0] = (char *)arg;
	va_start(ap, arg);
	for (int i = 1; i <= argc; i++)
		argv[i] = va_arg(ap, char *);
	va_end(ap);
	return execvpe(file, argv, environ);
}

int execle(const char *file, const char *arg, ...)
{
	va_list ap;
	char **envp;
	int argc;

	va_start(ap, arg);
	argc = count_args(arg, ap);
	va_end(ap);
	char *argv[argc + 1];
	argv[0] = (char *)arg;
	va_start(ap, arg);
	for (int i = 1; i <= argc; i++)
		argv[i] = va_arg(ap, char *);
	envp = va_arg(ap, char **);
	va_end(ap);
	return execve(file, argv, envp);
}

int posix_spawn(pid_t *pid, const char *file,
		const posix_spawn_file_actions_t *actions,
		const posix_spawnattr_t *attr, char *const argv[],
		char *const envp[])
{
	struct route r;
	struct envbuf eb;
	struct argbuf ab;
	char *const *av, *const *ev;
	int ret;

	resolve_real();
	if (is_null(file))
		return real_posix_spawn(pid, file, actions, attr, argv, envp);
	if (route_exec(&r, file, argv, envp) != 0)
		return errno;
	eb.list = ab.list = NULL;
	av = make_argv(&r, argv, &ab);
	if (!av)
		return ENOMEM;
	ev = make_env(envp, r.kind, &eb);
	if (debug)
		log_route(file, route_file(&r, file), kind_names[r.kind]);
	ret = real_posix_spawn(pid, route_file(&r, file), actions, attr, av,
			       ev);
	free_list(ab.list, ab.stack);
	free_list(eb.list, eb.stack);
	return ret;
}

int posix_spawnp(pid_t *pid, const char *file,
		 const posix_spawn_file_actions_t *actions,
		 const posix_spawnattr_t *attr, char *const argv[],
		 char *const envp[])
{
	char path[PATH_BUF];
	int err;

	if (is_null(file) || !*file)
		return ENOENT;
	if (strchr(file, '/'))
		return posix_spawn(pid, file, actions, attr, argv, envp);
	err = path_search(file, getenv("PATH"), path, sizeof(path));
	if (err)
		return err;
	return posix_spawn(pid, path, actions, attr, argv, envp);
}
