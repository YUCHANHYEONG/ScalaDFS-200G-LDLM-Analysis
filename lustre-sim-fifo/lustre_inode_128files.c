// lustre_inode_128files.c
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>

#define NTHREADS 128
#define WRITE_SIZE 4096
#define WRITE_COUNT 10

struct thread_arg {
    int tid;
    const char *dir;
};

void *writer_thread(void *arg)
{
    struct thread_arg *targ = (struct thread_arg *)arg;
    char path[512];
    char *buf;
    int fd;
    int ret;

    snprintf(path, sizeof(path), "%s/file_%03d.dat", targ->dir, targ->tid);

    ret = posix_memalign((void **)&buf, WRITE_SIZE, WRITE_SIZE);
    if (ret != 0) {
        fprintf(stderr, "tid=%d posix_memalign failed: %s\n",
                targ->tid, strerror(ret));
        return NULL;
    }

    memset(buf, 'A' + (targ->tid % 26), WRITE_SIZE);

    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr, "tid=%d open failed: %s\n",
                targ->tid, strerror(errno));
        free(buf);
        return NULL;
    }

    for (int i = 0; i < WRITE_COUNT; i++) {
        off_t offset = (off_t)i * WRITE_SIZE;
        ssize_t written = pwrite(fd, buf, WRITE_SIZE, offset);

        if (written != WRITE_SIZE) {
            fprintf(stderr, "tid=%d pwrite failed at i=%d: ret=%zd errno=%s\n",
                    targ->tid, i, written, strerror(errno));
            close(fd);
            free(buf);
            return NULL;
        }
    }

    fsync(fd);
    close(fd);
    free(buf);

    return NULL;
}

int main(int argc, char *argv[])
{
    pthread_t threads[NTHREADS];
    struct thread_arg args[NTHREADS];

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <directory>\n", argv[0]);
        fprintf(stderr, "Example: %s /mnt/client/inode_test\n", argv[0]);
        return 1;
    }

    mkdir(argv[1], 0755);

    for (int i = 0; i < NTHREADS; i++) {
        args[i].tid = i;
        args[i].dir = argv[1];

        if (pthread_create(&threads[i], NULL, writer_thread, &args[i]) != 0) {
            fprintf(stderr, "pthread_create failed at i=%d\n", i);
            return 1;
        }
    }

    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("Done: %d threads, each wrote %d x %d bytes\n",
           NTHREADS, WRITE_COUNT, WRITE_SIZE);

    return 0;
}
