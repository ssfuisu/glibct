/*
 * Shared code of the two glibct hooks: looking at a file before it starts,
 * deciding how to start it, building argv and environment lists, and the
 * exec functions that are built on execve and posix_spawn.
 *
 * Everything here keeps its state on the stack and calls malloc only for
 * very long argument lists, so it can run in a vfork() child.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"

extern char **environ;

int glibct_debug;
/* Real path of this program when it was started through the loader. */
char glibct_self_exe[PATH_MAX];

const char *const glibct_kind_names[] = {
	"other", "glibc", "linux", "x86_64", "bionic", "script"
};

/*
 * The headers mark file arguments as never NULL, so the compiler would drop
 * a plain NULL check. Programs still pass NULL sometimes; keep the check.
 */
int is_null(const void *p)
{
	__asm__("" : "+r"(p));
	return p == NULL;
}

int starts_with(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

void init_debug(void)
{
	const char *d = getenv("GLIBCT_DEBUG");

	glibct_debug = d && *d && strcmp(d, "0");
}

/* Writes "glibct: " and the parts as one line to stderr. */
void log_line(const char *const *parts, size_t count)
{
	char line[PATH_BUF * 2 + 64] = "glibct: ";
	size_t n = strlen(line);

	for (size_t i = 0; i < count; i++) {
		size_t len = strlen(parts[i]);
		if (n + len >= sizeof(line) - 1)
			len = sizeof(line) - n - 2;
		memcpy(line + n, parts[i], len);
		n += len;
	}
	line[n++] = '\n';
	if (write(2, line, n) < 0)
		return;
}

/* GLIBCT_DEBUG=1 prints one line per started program. */
void log_route(const char *from, const char *to, enum kind kind)
{
	const char *parts[] = { from, " -> ", to, " (",
				glibct_kind_names[kind], ")" };

	log_line(parts, sizeof(parts) / sizeof(parts[0]));
}

int is_self_exe(const char *path)
{
	char *end;
	long pid;

	if (!glibct_self_exe[0] || !path || !starts_with(path, "/proc/"))
		return 0;
	path += 6;
	if (!strcmp(path, "self/exe") || !strcmp(path, "thread-self/exe"))
		return 1;
	pid = strtol(path, &end, 10);
	return end != path && !strcmp(end, "/exe") && pid == getpid();
}

/* ---- looking at the file that is about to run ------------------------- */

static void probe_elf(int fd, const Elf64_Ehdr *eh, struct probe *p)
{
	char interp[PATH_BUF];
	Elf64_Phdr ph = { .p_type = PT_NULL };
	const char *base;
	ssize_t n;

	if (eh->e_ident[EI_CLASS] != ELFCLASS64)
		return;
	if (eh->e_machine == EM_X86_64) {
		p->kind = KIND_X86_64;
		return;
	}
	if (eh->e_machine != EM_AARCH64 || eh->e_phentsize != sizeof(ph) ||
	    eh->e_phnum > 128)
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

int is_exec_file(const char *path)
{
	struct stat st;

	return access(path, X_OK) == 0 && stat(path, &st) == 0 &&
	       S_ISREG(st.st_mode);
}

int join(char *out, size_t size, const char *dir, size_t dirlen,
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

const char *default_path(const char *list)
{
	return list ? list : GLIBC_PREFIX "/bin:" TERMUX_PREFIX "/bin";
}

/* Finds name in a PATH list. Returns 0, ENOENT or EACCES. */
int path_search(const char *name, const char *list, char *out, size_t size)
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

/*
 * Works out what really has to run for file and argv. On return r->kind
 * says what kind of program runs in the end. Returns -1 with errno set if
 * the program cannot be started at all.
 */
int route_exec(struct route *r, const char *file, char *const argv[],
	       char *const envp[], enum paths paths)
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
		r->file = glibct_self_exe;
		r->rewritten = 1;
	}

	for (int depth = 0;; depth++) {
		struct probe *p = &r->probes[depth];
		const char *mapped = NULL;
		if (paths == PATHS_GLIBC)
			mapped = map_missing(r->file, r->mapped[depth],
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
		if (p->kind == KIND_X86_64) {
			/* box64 runs x86_64 programs; without it the
			 * kernel reports "Exec format error" as before. */
			const char *items[] = { BOX64, r->file };
			if (!is_exec_file(BOX64)) {
				r->kind = KIND_OTHER;
				return 0;
			}
			if (replace_argv0(r, items, 2) != 0)
				return -1;
			r->file = BOX64;
			r->kind = KIND_GLIBC;
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
		if (paths == PATHS_GLIBC && !strcmp(base, "env") &&
		    access(interp, F_OK) != 0 &&
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
const char *route_file(const struct route *r, const char *file)
{
	return r->rewritten ? r->file : file;
}

char *const *make_argv(const struct route *r, char *const argv[],
		       struct argbuf *b)
{
	int ntail = 0;

	b->list = NULL;
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

void free_list(char **list, char **stack)
{
	if (list && list != stack)
		free(list);
}

/* ---- environment ----------------------------------------------------- */

const char *env_get(char *const envp[], const char *key)
{
	size_t len = strlen(key);

	for (int i = 0; envp && envp[i]; i++)
		if (!strncmp(envp[i], key, len) && envp[i][len] == '=')
			return envp[i] + len + 1;
	return NULL;
}

int is_key(const char *entry, const char *key)
{
	size_t len = strlen(key);

	return !strncmp(entry, key, len) && entry[len] == '=';
}

/* Joins the parts into out. Returns -1 if out is too small. */
int concat(char *out, size_t size, const char *const *parts, int count)
{
	size_t n = 0;

	for (int i = 0; i < count; i++) {
		size_t len = strlen(parts[i]);
		if (n + len + 1 > size)
			return -1;
		memcpy(out + n, parts[i], len);
		n += len;
	}
	out[n] = '\0';
	return 0;
}

int set_entry(char *out, size_t size, const char *key, const char *value)
{
	const char *parts[] = { key, "=", value };

	return concat(out, size, parts, 3);
}

/*
 * Writes "KEY=" plus the colon or space separated entries of value that
 * keep() accepts. Returns -1 if out is too small, 0 if no entry is left.
 */
int filter_list(char *out, size_t size, const char *key, const char *value,
		int (*keep)(const char *, size_t))
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

static int in_dir(const char *s, size_t len, const char *dir)
{
	size_t dlen = strlen(dir);

	return len >= dlen && !strncmp(s, dir, dlen) &&
	       (len == dlen || s[dlen] == '/');
}

/* Android libraries, and the directories that hold them. */
int not_bionic_lib(const char *s, size_t len)
{
	static const char *const dirs[] = {
		TERMUX_PREFIX "/lib", "/system", "/apex", "/vendor", NULL
	};

	for (int i = 0; dirs[i]; i++)
		if (in_dir(s, len, dirs[i]))
			return 0;
	return 1;
}

int not_glibc_dir(const char *s, size_t len)
{
	return !in_dir(s, len, GLIBC_PREFIX);
}

/*
 * Writes "PATH=" plus path with $GLIBC_PREFIX/bin right before
 * $TERMUX_PREFIX/bin, like the glibct command does.
 */
int glibc_path(char *out, size_t size, const char *path)
{
	static const char glibc_bin[] = GLIBC_PREFIX "/bin";
	static const char termux_bin[] = TERMUX_PREFIX "/bin";
	size_t n = strlen("PATH="), glen = strlen(glibc_bin);
	int added = 0;

	if (n + glen + 2 > size)
		return -1;
	memcpy(out, "PATH=", n);
	for (const char *p = path;; p++) {
		const char *colon = strchr(p, ':');
		size_t len = colon ? (size_t)(colon - p) : strlen(p);
		int skip = len == glen && !strncmp(p, glibc_bin, len);
		int termux = len == strlen(termux_bin) &&
			     !strncmp(p, termux_bin, len);
		if (termux && !added) {
			if (n + glen + 2 > size)
				return -1;
			memcpy(out + n, glibc_bin, glen);
			n += glen;
			out[n++] = ':';
			added = 1;
		}
		if (!skip) {
			if (n + len + 2 > size)
				return -1;
			memcpy(out + n, p, len);
			n += len;
			out[n++] = ':';
		}
		if (!colon)
			break;
		p = colon;
	}
	if (n > strlen("PATH="))
		n--; /* the last colon */
	out[n] = '\0';
	if (!added) {
		/* No Termux directory: put glibc first. */
		size_t rest = n - strlen("PATH=");
		if (n + glen + 2 > size)
			return -1;
		memmove(out + strlen("PATH=") + glen + 1,
			out + strlen("PATH="), rest + 1);
		memcpy(out + strlen("PATH="), glibc_bin, glen);
		out[strlen("PATH=") + glen] = rest ? ':' : '\0';
	}
	return 0;
}

static int same_key(const char *entry, const char *assignment)
{
	size_t len = strcspn(assignment, "=");

	return !strncmp(entry, assignment, len) && entry[len] == '=';
}

/*
 * Returns envp without the variables named in drop, and with the
 * "KEY=value" entries of set instead of the old values.
 */
char *const *edit_env(char *const envp[], struct envbuf *b,
		      const char *const *drop, int ndrop, char *const *set,
		      int nset)
{
	int count = 0, n = 0;

	while (envp && envp[count])
		count++;
	b->list = b->stack;
	if (count + nset + 1 > STACK_ARGS) {
		b->list = malloc((count + nset + 1) * sizeof(char *));
		if (!b->list)
			return envp;
	}
	for (int i = 0; i < count; i++) {
		int skip = 0;
		for (int j = 0; j < ndrop && !skip; j++)
			skip = is_key(envp[i], drop[j]);
		for (int j = 0; j < nset && !skip; j++)
			skip = same_key(envp[i], set[j]);
		if (!skip)
			b->list[n++] = envp[i];
	}
	for (int j = 0; j < nset; j++)
		b->list[n++] = set[j];
	b->list[n] = NULL;
	return b->list;
}

/* ---- exec functions built on hook_execve and hook_posix_spawn ---------- */

EXPORT int execv(const char *file, char *const argv[])
{
	return hook_execve(file, argv, environ);
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
	for (int i = 0; hook_shells[i]; i++)
		hook_execve(hook_shells[i], sh_argv, envp);
	errno = ENOEXEC;
	return -1;
}

EXPORT int execvpe(const char *file, char *const argv[], char *const envp[])
{
	char path[PATH_BUF];
	int eacces = 0;

	if (is_null(file) || !*file) {
		errno = ENOENT;
		return -1;
	}
	if (strchr(file, '/')) {
		hook_execve(file, argv, envp);
		return errno == ENOEXEC ? exec_with_sh(file, argv, envp) : -1;
	}
	for (const char *p = default_path(getenv("PATH"));; p++) {
		const char *colon = strchr(p, ':');
		size_t len = colon ? (size_t)(colon - p) : strlen(p);
		if (join(path, sizeof(path), p, len, file) == 0) {
			hook_execve(path, argv, envp);
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

EXPORT int execvp(const char *file, char *const argv[])
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

EXPORT int execl(const char *file, const char *arg, ...)
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
	return hook_execve(file, argv, environ);
}

EXPORT int execlp(const char *file, const char *arg, ...)
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

EXPORT int execle(const char *file, const char *arg, ...)
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
	return hook_execve(file, argv, envp);
}

EXPORT int posix_spawnp(pid_t *pid, const char *file, const void *actions,
			const void *attr, char *const argv[],
			char *const envp[])
{
	char path[PATH_BUF];
	int err;

	if (is_null(file) || !*file)
		return ENOENT;
	if (strchr(file, '/'))
		return hook_posix_spawn(pid, file, actions, attr, argv, envp);
	err = path_search(file, getenv("PATH"), path, sizeof(path));
	if (err)
		return err;
	return hook_posix_spawn(pid, path, actions, attr, argv, envp);
}
