/*
 * libglibct.so - the hook inside every glibc program.
 *
 * Termux keeps glibc in $PREFIX/glibc. Programs built for it start fine, but
 * programs built for regular Linux distributions ask for a loader such as
 * /lib/ld-linux-aarch64.so.1, and scripts ask for /bin/sh or /usr/bin/env.
 * None of these paths exist on Android.
 *
 * glibct lists this library in $PREFIX/glibc/etc/ld.so.preload. Only the
 * glibc loader reads that file, so every glibc program loads the library and
 * Android's own linker never sees it. For every program that is started it:
 *
 *   - runs Linux glibc programs through Termux's glibc loader,
 *   - runs x86_64 programs with box64, if it is installed,
 *   - maps /bin, /usr/bin and missing /usr/local/bin and /sbin paths to
 *     $PREFIX/glibc/bin or $PREFIX/bin,
 *   - reads #! lines itself so the interpreter gets the same treatment,
 *   - gives Android (Bionic) programs the normal Termux environment back,
 *     plus libglibct-termux.so so they can start glibc programs too.
 *
 * It also fixes what a program started through the loader sees about itself
 * (/proc/self/exe, its name), and turns system calls that Android blocks
 * into "not supported" errors instead of letting them kill the program.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <ucontext.h>
#include <unistd.h>

#include "common.h"

#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1 /* si_code of a seccomp SIGSYS; older headers lack it */
#endif

static int (*real_execve)(const char *, char *const[], char *const[]);
static int (*real_posix_spawn)(pid_t *, const char *, const void *,
			       const void *, char *const[], char *const[]);
static ssize_t (*real_readlink)(const char *, char *, size_t);
static ssize_t (*real_readlinkat)(int, const char *, char *, size_t);
static char *(*real_realpath)(const char *, char *);

const char *const hook_shells[] = {
	GLIBC_PREFIX "/bin/sh", TERMUX_PREFIX "/bin/sh", NULL
};

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

/* ---- system calls blocked by Android ---------------------------------- */

/*
 * Android's seccomp filter stops some newer system calls (openat2,
 * cachestat, pkey_alloc, ...) with SIGSYS, which kills the program. A
 * kernel without these calls answers ENOSYS instead, and programs then use
 * their fallback. Give them that answer.
 */
static void on_sigsys(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;

	if (info->si_code != SYS_SECCOMP) {
		signal(sig, SIG_DFL);
		raise(sig);
		return;
	}
	uc->uc_mcontext.regs[0] = (unsigned long long)-ENOSYS;
	if (glibct_debug) {
		char num[16];
		int n = sizeof(num) - 1, nr = info->si_syscall;
		num[n] = '\0';
		do
			num[--n] = '0' + nr % 10;
		while ((nr /= 10) && n > 0);
		const char *parts[] = { "system call ", num + n,
					" is blocked by Android, returning ENOSYS" };
		log_line(parts, 3);
	}
}

static void catch_blocked_syscalls(void)
{
	struct sigaction sa, old;

	if (sigaction(SIGSYS, NULL, &old) != 0 ||
	    (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN))
		return;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_sigsys;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSYS, &sa, NULL);
}

/* ---- /proc/self/exe ---------------------------------------------------- */

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
	if (!real_realpath || !real_realpath(arg, glibct_self_exe)) {
		glibct_self_exe[0] = '\0';
		return;
	}
	/* Like the kernel: the name of the path that was started, so top,
	 * ps and pkill show the program instead of the loader. */
	name = strrchr(arg, '/');
	prctl(PR_SET_NAME, name ? name + 1 : arg, 0, 0, 0);
}

__attribute__((constructor)) static void glibct_init(void)
{
	char exe[PATH_BUF];
	ssize_t n;

	resolve_real();
	init_debug();
	catch_blocked_syscalls();
	if (!real_readlink)
		return;
	n = real_readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n == (ssize_t)strlen(LOADER) && !memcmp(exe, LOADER, n))
		find_self_exe();
}

static ssize_t copy_self_exe(char *buf, size_t size)
{
	size_t len = strlen(glibct_self_exe);

	if (len > size)
		len = size;
	memcpy(buf, glibct_self_exe, len);
	return len;
}

EXPORT ssize_t readlink(const char *path, char *buf, size_t size)
{
	if (is_self_exe(path))
		return copy_self_exe(buf, size);
	resolve_real();
	return real_readlink(path, buf, size);
}

EXPORT ssize_t readlinkat(int dirfd, const char *path, char *buf,
			  size_t size)
{
	if (is_self_exe(path))
		return copy_self_exe(buf, size);
	resolve_real();
	return real_readlinkat(dirfd, path, buf, size);
}

EXPORT ssize_t __readlink_chk(const char *path, char *buf, size_t size,
			      size_t buflen)
{
	(void)buflen;
	return readlink(path, buf, size);
}

EXPORT ssize_t __readlinkat_chk(int dirfd, const char *path, char *buf,
				size_t size, size_t buflen)
{
	(void)buflen;
	return readlinkat(dirfd, path, buf, size);
}

EXPORT char *realpath(const char *path, char *resolved)
{
	if (is_self_exe(path))
		return resolved ? strcpy(resolved, glibct_self_exe)
				: strdup(glibct_self_exe);
	resolve_real();
	return real_realpath(path, resolved);
}

EXPORT char *__realpath_chk(const char *path, char *resolved,
			    size_t resolvedlen)
{
	(void)resolvedlen;
	return realpath(path, resolved);
}

EXPORT char *canonicalize_file_name(const char *path)
{
	return realpath(path, NULL);
}

/* ---- environment ----------------------------------------------------- */

/*
 * Inside glibct (GLIBCT=1) a Termux program gets the Termux values of
 * LD_PRELOAD, LD_LIBRARY_PATH, PATH and SHELL back. LD_PRELOAD also gets
 * libglibct-termux.so in front, so that the program can start glibc
 * programs as well.
 */
static char *const *termux_env(char *const envp[], struct envbuf *b)
{
	const char *drop[] = { "LD_PRELOAD", "LD_LIBRARY_PATH" };
	const char *saved = env_get(envp, "GLIBCT_TERMUX_LD_PRELOAD");
	const char *hook = "", *v;
	char *set[4];
	int nset = 0;

	if (!saved)
		saved = "";
	if (access(TERMUX_HOOK, R_OK) == 0 && !strstr(saved, TERMUX_HOOK))
		hook = TERMUX_HOOK;
	if (*hook || *saved) {
		const char *parts[] = { "LD_PRELOAD=", hook,
					*hook && *saved ? ":" : "", saved };
		if (concat(b->preload, sizeof(b->preload), parts, 4) == 0)
			set[nset++] = b->preload;
	}
	v = env_get(envp, "GLIBCT_TERMUX_LD_LIBRARY_PATH");
	if (v && set_entry(b->ldpath, sizeof(b->ldpath), "LD_LIBRARY_PATH",
			   v) == 0)
		set[nset++] = b->ldpath;
	v = env_get(envp, "PATH");
	if (v && filter_list(b->path, sizeof(b->path), "PATH", v,
			     not_glibc_dir) >= 0)
		set[nset++] = b->path;
	v = env_get(envp, "GLIBCT_TERMUX_SHELL");
	if (v && *v && set_entry(b->shell, sizeof(b->shell), "SHELL", v) == 0)
		set[nset++] = b->shell;
	return edit_env(envp, b, drop, 2, set, nset);
}

/*
 * glibc programs must not get Android libraries in LD_PRELOAD and Android
 * programs must not get glibc libraries there: both linkers stop with an
 * error otherwise.
 */
static char *const *make_env(char *const envp[], enum kind kind,
			     struct envbuf *b)
{
	const char *drop[] = { "LD_PRELOAD" };
	const char *preload = env_get(envp, "LD_PRELOAD");
	const char *active = env_get(envp, "GLIBCT");
	char *set[1];
	int left;

	b->list = NULL;
	if (kind == KIND_BIONIC && active && !strcmp(active, "1"))
		return termux_env(envp, b);
	if (!preload || kind == KIND_OTHER || kind == KIND_SCRIPT)
		return envp;
	left = filter_list(b->preload, sizeof(b->preload), "LD_PRELOAD",
			   preload,
			   kind == KIND_BIONIC ? not_glibc_dir : not_bionic_lib);
	if (left < 0 || !strcmp(b->preload + strlen("LD_PRELOAD="), preload))
		return envp; /* nothing to change */
	set[0] = b->preload;
	return edit_env(envp, b, drop, 1, set, left > 0);
}

/* ---- exec and spawn -------------------------------------------------- */

int hook_execve(const char *file, char *const argv[], char *const envp[])
{
	struct route r;
	struct envbuf eb;
	struct argbuf ab;
	char *const *av, *const *ev;
	int ret, saved;

	resolve_real();
	if (is_null(file))
		return real_execve(file, argv, envp);
	if (route_exec(&r, file, argv, envp, PATHS_GLIBC) != 0)
		return -1;
	av = make_argv(&r, argv, &ab);
	if (!av) {
		errno = ENOMEM;
		return -1;
	}
	ev = make_env(envp, r.kind, &eb);
	if (glibct_debug)
		log_route(file, route_file(&r, file), r.kind);
	ret = real_execve(route_file(&r, file), av, ev);
	saved = errno;
	free_list(ab.list, ab.stack);
	free_list(eb.list, eb.stack);
	errno = saved;
	return ret;
}

int hook_posix_spawn(pid_t *pid, const char *file, const void *actions,
		     const void *attr, char *const argv[], char *const envp[])
{
	struct route r;
	struct envbuf eb;
	struct argbuf ab;
	char *const *av, *const *ev;
	int ret;

	resolve_real();
	if (is_null(file))
		return real_posix_spawn(pid, file, actions, attr, argv, envp);
	if (route_exec(&r, file, argv, envp, PATHS_GLIBC) != 0)
		return errno;
	av = make_argv(&r, argv, &ab);
	if (!av)
		return ENOMEM;
	ev = make_env(envp, r.kind, &eb);
	if (glibct_debug)
		log_route(file, route_file(&r, file), r.kind);
	ret = real_posix_spawn(pid, route_file(&r, file), actions, attr, av,
			       ev);
	free_list(ab.list, ab.stack);
	free_list(eb.list, eb.stack);
	return ret;
}

EXPORT int execve(const char *file, char *const argv[], char *const envp[])
{
	return hook_execve(file, argv, envp);
}

EXPORT int posix_spawn(pid_t *pid, const char *file,
		       const posix_spawn_file_actions_t *actions,
		       const posix_spawnattr_t *attr, char *const argv[],
		       char *const envp[])
{
	return hook_posix_spawn(pid, file, actions, attr, argv, envp);
}
