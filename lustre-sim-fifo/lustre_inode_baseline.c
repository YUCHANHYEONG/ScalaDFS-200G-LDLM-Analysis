// lustre_inode_baseline.c
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define WRITE_SIZE 4096
#define WRITE_COUNT 10

int main(int argc, char *argv[])
{
    const char *path;
    int fd;
    char *buf;
    int ret;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file_path>\n", argv[0]);
        fprintf(stderr, "Example: %s /mnt/client/test/baseline.dat\n", argv[0]);
        return 1;
    }

    path = argv[1];

    ret = posix_memalign((void **)&buf, WRITE_SIZE, WRITE_SIZE);
    if (ret != 0) {
        fprintf(stderr, "posix_memalign failed: %s\n", strerror(ret));
        return 1;
    }

    memset(buf, 'A', WRITE_SIZE);

    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr, "open failed: %s\n", strerror(errno));
        free(buf);
        return 1;
    }

    for (int i = 0; i < WRITE_COUNT; i++) {
        off_t offset = (off_t)i * WRITE_SIZE;
        ssize_t written;

        written = pwrite(fd, buf, WRITE_SIZE, offset);
        if (written < 0) {
            fprintf(stderr, "pwrite failed at i=%d: %s\n", i, strerror(errno));
            close(fd);
            free(buf);
            return 1;
        }

        if (written != WRITE_SIZE) {
            fprintf(stderr, "partial write at i=%d: %zd bytes\n", i, written);
            close(fd);
            free(buf);
            return 1;
        }
    }

    ret = fsync(fd);
    if (ret < 0) {
        fprintf(stderr, "fsync failed: %s\n", strerror(errno));
        close(fd);
        free(buf);
        return 1;
    }

    close(fd);
    free(buf);

    printf("Done: wrote %d x %d bytes to %s\n",
           WRITE_COUNT, WRITE_SIZE, path);

    return 0;
}
