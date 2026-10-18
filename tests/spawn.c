/* spawn METHOD FILE [ARGS...]: starts FILE with one of the libc functions. */
#define _GNU_SOURCE
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int wait_for(pid_t pid)
{
	int status;

	if (waitpid(pid, &status, 0) < 0)
		return 127;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int main(int argc, char **argv)
{
	const char *m;
	char **args;
	pid_t pid;
	int err;

	if (argc < 3) {
		fprintf(stderr, "usage: spawn METHOD FILE [ARGS...]\n");
		return 2;
	}
	m = argv[1];
	args = argv + 2;
	fflush(stdout);
	if (!strcmp(m, "execve"))
		execve(args[0], args, environ);
	else if (!strcmp(m, "execv"))
		execv(args[0], args);
	else if (!strcmp(m, "execvp"))
		execvp(args[0], args);
	else if (!strcmp(m, "execvpe"))
		execvpe(args[0], args, environ);
	else if (!strcmp(m, "execl"))
		execl(args[0], args[0], args[1], (char *)NULL);
	else if (!strcmp(m, "execlp"))
		execlp(args[0], args[0], args[1], (char *)NULL);
	else if (!strcmp(m, "execle"))
		execle(args[0], args[0], args[1], (char *)NULL, environ);
	else if (!strcmp(m, "vfork")) {
		pid = vfork();
		if (pid == 0) {
			execvp(args[0], args);
			_exit(127);
		}
		return wait_for(pid);
	} else if (!strcmp(m, "posix_spawn") || !strcmp(m, "posix_spawnp")) {
		err = m[11] ? posix_spawnp(&pid, args[0], NULL, NULL, args, environ)
			    : posix_spawn(&pid, args[0], NULL, NULL, args, environ);
		if (err) {
			fprintf(stderr, "%s: %s\n", m, strerror(err));
			return 127;
		}
		return wait_for(pid);
	} else if (!strcmp(m, "system")) {
		int st = system(args[0]);
		return WIFEXITED(st) ? WEXITSTATUS(st) : 127;
	} else {
		fprintf(stderr, "unknown method %s\n", m);
		return 2;
	}
	fprintf(stderr, "%s %s: %s\n", m, args[0], strerror(errno));
	return 127;
}
