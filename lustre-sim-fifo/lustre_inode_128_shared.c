// lustre_inode_128_shared.c
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
#define WRITE_COUNT 1000

struct thread_arg {
    int tid;
    int fd;
};

void *writer_thread(void *arg)
{
    struct thread_arg *targ = (struct thread_arg *)arg;
    char *buf;
    int ret;

    ret = posix_memalign((void **)&buf, WRITE_SIZE, WRITE_SIZE);
    if (ret != 0) {
        fprintf(stderr, "tid=%d posix_memalign failed: %s\n",
                targ->tid, strerror(ret));
        return NULL;
    }

    memset(buf, 'A' + (targ->tid % 26), WRITE_SIZE);

    for (int i = 0; i < WRITE_COUNT; i++) {
        off_t offset = ((off_t)targ->tid * WRITE_COUNT + i) * WRITE_SIZE;
        ssize_t written = pwrite(targ->fd, buf, WRITE_SIZE, offset);

        if (written != WRITE_SIZE) {
            fprintf(stderr, "tid=%d pwrite failed at i=%d: ret=%zd errno=%s\n",
                    targ->tid, i, written, strerror(errno));
            free(buf);
            return NULL;
        }
    }

    free(buf);
    return NULL;
}

int main(int argc, char *argv[])
{
    pthread_t threads[NTHREADS];
    struct thread_arg args[NTHREADS];
    int fd;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file_path>\n", argv[0]);
        fprintf(stderr, "Example: %s /mnt/client/inode_test/shared.dat\n", argv[0]);
        return 1;
    }

    fd = open(argv[1], O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr, "open failed: %s\n", strerror(errno));
        return 1;
    }

    for (int i = 0; i < NTHREADS; i++) {
        args[i].tid = i;
        args[i].fd = fd;

        if (pthread_create(&threads[i], NULL, writer_thread, &args[i]) != 0) {
            fprintf(stderr, "pthread_create failed at i=%d\n", i);
            close(fd);
            return 1;
        }
    }

    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    fsync(fd);
    close(fd);

    printf("Done: %d threads wrote to one shared file\n", NTHREADS);
    return 0;
}
