// fsync_test.c
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_PATH "/mnt/client/fsync_test_file"
#define WRITE_SIZE 4096

int main(void)
{
    int fd;
    char buf[WRITE_SIZE];
    ssize_t written;
    int ret;

    memset(buf, 'A', sizeof(buf));

    fd = open(TEST_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", TEST_PATH, strerror(errno));
        return 1;
    }

    written = write(fd, buf, sizeof(buf));
    if (written < 0) {
        fprintf(stderr, "write failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    if (written != WRITE_SIZE) {
        fprintf(stderr, "partial write: %zd / %d bytes\n", written, WRITE_SIZE);
        close(fd);
        return 1;
    }

    printf("write 4KB done\n");

    ret = fsync(fd);
    if (ret < 0) {
        fprintf(stderr, "fsync failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("fsync done\n");

    ret = close(fd);
    if (ret < 0) {
        fprintf(stderr, "close failed: %s\n", strerror(errno));
        return 1;
    }

    printf("close done\n");

    return 0;
}
