#ifndef WORKER_H
#define WORKER_H

#include <sys/types.h>

typedef struct {
    pid_t   parent_pid;
    char  **dirs;
    int     num_dirs;
    char   *pattern;
    long    min_size;
    char   *result_file;  /* temp file path: worker writes MATCH lines here */
} WorkerArgs;

void run_worker(const WorkerArgs *args);

#endif /* WORKER_H */
