/*
 * libglibct-termux.so - the hook inside Termux (Bionic) programs.
 *
 * Inside glibct, Termux programs run with the normal Termux environment:
 * LD_PRELOAD holds termux-exec, which glibc cannot load. Without help, a
 * Termux program that starts a glibc program (Termux's make running the
 * glibc gcc, for example) passes that LD_PRELOAD on and the glibc program
 * stops right away.
 *
 * libglibct.so puts this library in front of termux-exec in LD_PRELOAD
 * when it starts a Termux program. The library looks at every program the
 * Termux program starts:
 *
 *   - glibc programs, Linux programs and x86_64 programs get the glibct
 *     environment (no Android libraries in LD_PRELOAD, glibc programs first
 *     in PATH) and are started the same way libglibct.so starts them,
 *   - everything else goes to termux-exec unchanged.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common.h"

static int (*next_execve)(const char *, char *const[], char *const[]);
static int (*next_posix_spawn)(pid_t *, const char *, const void *,
			       const void *, char *const[], char *const[]);

const char *const hook_shells[] = {
	TERMUX_PREFIX "/bin/sh", "/system/bin/sh", NULL
};

static void resolve_next(void)
{
	if (!next_execve)
		next_execve = dlsym(RTLD_NEXT, "execve");
	if (!next_posix_spawn)
		next_posix_spawn = dlsym(RTLD_NEXT, "posix_spawn");
}

__attribute__((constructor)) static void glibct_termux_init(void)
{
	resolve_next();
	init_debug();
}

static int is_glibc_world(enum kind kind)
{
	return kind == KIND_GLIBC || kind == KIND_LINUX;
}

/* The environment of the glibc world, see the glibct command. */
static char *const *glibc_env(char *const envp[], struct envbuf *b)
{
	const char *drop[2];
	const char *active = env_get(envp, "GLIBCT"), *v;
	char *set[4];
	int ndrop = 0, nset = 0, left;

	v = env_get(envp, "LD_PRELOAD");
	if (v) {
		left = filter_list(b->preload, sizeof(b->preload),
				   "LD_PRELOAD", v, not_bionic_lib);
		if (left > 0)
			set[nset++] = b->preload;
		else
			drop[ndrop++] = "LD_PRELOAD";
	}
	v = env_get(envp, "LD_LIBRARY_PATH");
	if (v) {
		left = filter_list(b->ldpath, sizeof(b->ldpath),
				   "LD_LIBRARY_PATH", v, not_bionic_lib);
		if (left > 0)
			set[nset++] = b->ldpath;
		else
			drop[ndrop++] = "LD_LIBRARY_PATH";
	}
	if (active && !strcmp(active, "1")) {
		v = env_get(envp, "PATH");
		if (v && glibc_path(b->path, sizeof(b->path), v) == 0)
			set[nset++] = b->path;
		if (is_exec_file(GLIBC_PREFIX "/bin/bash"))
			set[nset++] = "SHELL=" GLIBC_PREFIX "/bin/bash";
	}
	return edit_env(envp, b, drop, ndrop, set, nset);
}

int hook_execve(const char *file, char *const argv[], char *const envp[])
{
	struct route r;
	struct envbuf eb;
	struct argbuf ab;
	char *const *av, *const *ev;
	int ret, saved;

	resolve_next();
	if (is_null(file) ||
	    route_exec(&r, file, argv, envp, PATHS_KEEP) != 0 ||
	    !is_glibc_world(r.kind))
		return next_execve(file, argv, envp);
	av = make_argv(&r, argv, &ab);
	if (!av) {
		errno = ENOMEM;
		return -1;
	}
	ev = glibc_env(envp, &eb);
	if (glibct_debug)
		log_route(file, route_file(&r, file), r.kind);
	/* Straight to the kernel: termux-exec only knows Termux programs. */
	ret = syscall(SYS_execve, route_file(&r, file), av, ev);
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

	resolve_next();
	if (!next_posix_spawn)
		return ENOSYS;
	if (is_null(file) ||
	    route_exec(&r, file, argv, envp, PATHS_KEEP) != 0 ||
	    !is_glibc_world(r.kind))
		return next_posix_spawn(pid, file, actions, attr, argv, envp);
	av = make_argv(&r, argv, &ab);
	if (!av)
		return ENOMEM;
	ev = glibc_env(envp, &eb);
	if (glibct_debug)
		log_route(file, route_file(&r, file), r.kind);
	ret = next_posix_spawn(pid, route_file(&r, file), actions, attr, av,
			       ev);
	free_list(ab.list, ab.stack);
	free_list(eb.list, eb.stack);
	return ret;
}

EXPORT int execve(const char *file, char *const argv[], char *const envp[])
{
	return hook_execve(file, argv, envp);
}

EXPORT int posix_spawn(pid_t *pid, const char *file, const void *actions,
		       const void *attr, char *const argv[],
		       char *const envp[])
{
	return hook_posix_spawn(pid, file, actions, attr, argv, envp);
}
