/* Prints how this program sees itself. Used by run-tests.sh. */
#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	char exe[PATH_MAX], real[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

	exe[n > 0 ? n : 0] = '\0';
	if (!realpath("/proc/self/exe", real))
		strcpy(real, "?");
	printf("argv0=%s\n", argv[0]);
	printf("exe=%s\n", exe);
	printf("realpath=%s\n", real);
	FILE *f = fopen("/proc/self/comm", "r");
	char comm[64] = "?";
	if (f) {
		if (fgets(comm, sizeof(comm), f))
			comm[strcspn(comm, "\n")] = '\0';
		fclose(f);
	}
	printf("comm=%s\n", comm);
	for (int i = 1; i < argc; i++)
		printf("arg%d=%s\n", i, argv[i]);
	if (argc > 1 && !strcmp(argv[1], "reexec")) {
		char *args[] = { "reexeced", "done", NULL };
		fflush(stdout);
		execv("/proc/self/exe", args);
		perror("execv /proc/self/exe");
		return 1;
	}
	return 0;
}
