/* usage: devbench <file> <chunk bytes> <writes per fsync> <total MB>
 * the sustained rate of the journal's write pattern on the disk under <file>;
 * build: gcc -O2 -o devbench devbench.c */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
/* the journal's pattern: `chunk` bytes written `group` times, then one fsync */
int main(int argc, char **argv)
{
	const char *path = argv[1];
	size_t chunk = atol(argv[2]);
	size_t group = atol(argv[3]);
	double total_mb = atof(argv[4]);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	char *buf = malloc(chunk);
	memset(buf, 'x', chunk);
	double done = 0;
	struct timeval t0, t1;
	gettimeofday(&t0, NULL);
	while (done < total_mb) {
		for (size_t i = 0; i < group; i++) {
			if (write(fd, buf, chunk) < 0) { perror("write"); return 1; }
		}
		if (fsync(fd) != 0) { perror("fsync"); return 1; }
		done += (double)(chunk * group) / (1024*1024);
	}
	gettimeofday(&t1, NULL);
	double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec)/1e6;
	close(fd);
	printf("chunk=%zu group=%zu  %.1f MB/s\n", chunk, group, done/el);
	return 0;
}
