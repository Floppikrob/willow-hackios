/*
 * show_files DIR [MATCH [MAX]]: the files under DIR, and the contents of those whose name has MATCH in
 * it (at most MAX bytes of each, 65536 if not said), on standard output -- which, run from the job list
 * (etc/rc.jobs), is the console and so the kernel's log: the way to read a file of /private/var when
 * that is on the phone's flash (var_disk=1) and nothing else can mount it. Every line begins "sf: ",
 * and every line of a file "sf: N: " with its number, so that the lines can be put back together from
 * a log in which other cores' lines are mixed in.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *match;
static long most = 65536;

static void
show(const char *path, long size)
{
	char buf[4096], line[1024];
	long left = most, n, used = 0, number = 0;
	int fd = open(path, O_RDONLY);

	printf("sf: == %s (%ld bytes)\n", path, size);
	if (fd < 0) {
		printf("sf: cannot open it\n");
		return;
	}
	while (left > 0 && (n = read(fd, buf, sizeof(buf) < (size_t)left ? sizeof(buf) : (size_t)left)) > 0) {
		left -= n;
		for (long i = 0; i < n; i++) {
			char c = buf[i];

			if (c == '\n' || used == sizeof(line) - 1) {
				line[used] = 0;
				printf("sf: %ld: %s\n", ++number, line);
				used = 0;
				if (c == '\n') {
					continue;
				}
			}
			line[used++] = (c >= 32 && c < 127) || c == '\t' ? c : '.';
		}
		fflush(stdout);
	}
	if (used != 0) {
		line[used] = 0;
		printf("sf: %ld: %s\n", ++number, line);
	}
	if (left <= 0) {
		printf("sf: (cut at %ld bytes)\n", most);
	}
	printf("sf: == end of %s\n", path);
	fflush(stdout);
	close(fd);
}

static void
walk(const char *dir, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char path[1024];
	struct stat st;

	if (d == NULL) {
		printf("sf: %s: cannot open\n", dir);
		return;
	}
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (lstat(path, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			printf("sf: dir %s\n", path);
			if (depth < 4) {
				walk(path, depth + 1);
			}
		} else {
			printf("sf: file %s %lld bytes, modified %ld\n", path, (long long)st.st_size, (long)st.st_mtime);
			if (match != NULL && strstr(e->d_name, match) != NULL) {
				show(path, (long)st.st_size);
			}
		}
	}
	closedir(d);
	fflush(stdout);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: show_files DIR [MATCH [MAX]]\n");
		return 1;
	}
	match = argc > 2 ? argv[2] : NULL;
	most = argc > 3 ? atol(argv[3]) : most;
	setvbuf(stdout, NULL, _IOLBF, 0);
	walk(argv[1], 0);
	printf("sf: done\n");
	return 0;
}
