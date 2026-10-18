/*
 * Shared code of the two glibct hooks.
 *
 * libglibct.so is loaded into glibc programs (through
 * $PREFIX/glibc/etc/ld.so.preload) and libglibct-termux.so into Termux
 * programs started inside glibct (through LD_PRELOAD). Both look at every
 * program that is about to start and decide how it has to be started.
 */
#ifndef GLIBCT_COMMON_H
#define GLIBCT_COMMON_H

#include <stddef.h>
#include <sys/types.h>

#ifndef TERMUX_PREFIX
#define TERMUX_PREFIX "/data/data/com.termux/files/usr"
#endif
#define GLIBC_PREFIX TERMUX_PREFIX "/glibc"

#if !defined(__aarch64__)
#error "glibct supports aarch64 only"
#endif
#define LOADER GLIBC_PREFIX "/lib/ld-linux-aarch64.so.1"
#define BOX64 GLIBC_PREFIX "/bin/box64"
#define TERMUX_HOOK TERMUX_PREFIX "/lib/libglibct-termux.so"

#define MAX_DEPTH 4    /* nested #! interpreters, same limit as Linux */
#define MAX_WORDS 8    /* words after "#!/usr/bin/env -S" */
#define MAX_PRE (MAX_DEPTH * (MAX_WORDS + 2) + 8)
#define PATH_BUF 1024
#define LINE_BUF 256   /* Linux reads at most 256 bytes of a #! line */
#define STACK_ARGS 256 /* longer argv or envp arrays use malloc */

#define EXPORT __attribute__((visibility("default")))

enum kind {
	KIND_OTHER,  /* run unchanged: static programs, unknown files */
	KIND_GLIBC,  /* built for Termux glibc */
	KIND_LINUX,  /* built for a regular Linux system, needs the loader */
	KIND_X86_64, /* built for x86_64 PCs, runs with box64 */
	KIND_BIONIC, /* built for Android */
	KIND_SCRIPT, /* starts with #! */
};

struct probe {
	enum kind kind;
	char line[LINE_BUF];  /* script: interpreter, NUL, optional argument */
	char *arg;
	char words[LINE_BUF]; /* the argument split into words */
};

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

/* How route_exec treats paths that do not exist on Android. */
enum paths {
	PATHS_GLIBC, /* map /bin and /usr/bin, glibc programs first */
	PATHS_KEEP,  /* leave them to termux-exec */
};

struct envbuf {
	char **list;
	char *stack[STACK_ARGS];
	char preload[1024];
	char path[4096];
	char ldpath[2048];
	char shell[PATH_BUF];
};

struct argbuf {
	char **list;
	char *stack[STACK_ARGS];
};

extern int glibct_debug;
extern char glibct_self_exe[];
extern const char *const glibct_kind_names[];

int is_null(const void *p);
int starts_with(const char *s, const char *prefix);
void log_line(const char *const *parts, size_t count);
void log_route(const char *from, const char *to, enum kind kind);
void init_debug(void);

int is_self_exe(const char *path);
int is_exec_file(const char *path);
int join(char *out, size_t size, const char *dir, size_t dirlen,
	 const char *name);
const char *default_path(const char *list);
int path_search(const char *name, const char *list, char *out, size_t size);

int route_exec(struct route *r, const char *file, char *const argv[],
	       char *const envp[], enum paths paths);
const char *route_file(const struct route *r, const char *file);
char *const *make_argv(const struct route *r, char *const argv[],
		       struct argbuf *b);
void free_list(char **list, char **stack);

const char *env_get(char *const envp[], const char *key);
int is_key(const char *entry, const char *key);
int concat(char *out, size_t size, const char *const *parts, int count);
int set_entry(char *out, size_t size, const char *key, const char *value);
int filter_list(char *out, size_t size, const char *key, const char *value,
		int (*keep)(const char *, size_t));
int not_bionic_lib(const char *s, size_t len);
int not_glibc_dir(const char *s, size_t len);
int glibc_path(char *out, size_t size, const char *path);
char *const *edit_env(char *const envp[], struct envbuf *b,
		      const char *const *drop, int ndrop, char *const *set,
		      int nset);

/*
 * Each hook implements these two. common.c builds execv, execvp, execvpe,
 * execl, execlp, execle and posix_spawnp on top of them.
 */
int hook_execve(const char *file, char *const argv[], char *const envp[]);
/* Shells for files that are neither programs nor #! scripts. */
extern const char *const hook_shells[];
int hook_posix_spawn(pid_t *pid, const char *file, const void *actions,
		     const void *attr, char *const argv[], char *const envp[]);

#endif
