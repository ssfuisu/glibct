/* Calls system calls that Android blocks, the way some Linux programs do. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void)
{
	long nrs[] = { 437 /* openat2 */, 451 /* cachestat */, 289 /* pkey_alloc */ };
	for (int i = 0; i < 3; i++) {
		errno = 0;
		long r = syscall(nrs[i], 0, 0, 0, 0);
		printf("syscall %ld: ret=%ld %s\n", nrs[i], r, strerror(errno));
	}
	return 0;
}
