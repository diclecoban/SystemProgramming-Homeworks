#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include "worker.h"
#include "pattern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

static volatile sig_atomic_t g_sigterm_received = 0;

static void worker_sigterm_handler(int sig)
{
    (void)sig;
    g_sigterm_received = 1;
}

static void search_dir(const char *path,
                       const char *pattern,
                       long        min_size,
                       FILE       *result_fp,
                       int        *match_count)
{
    if (g_sigterm_received) return;

    DIR *dp = opendir(path);
    if (!dp) return;

    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (g_sigterm_received) break;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        char fullpath[4096];
        int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
        if (n < 0 || n >= (int)sizeof(fullpath)) continue;

        struct stat st;
        if (lstat(fullpath, &st) < 0) continue;

        if (S_ISDIR(st.st_mode)) {
            search_dir(fullpath, pattern, min_size, result_fp, match_count);
        } else if (S_ISREG(st.st_mode)) {
            if (min_size >= 0 && st.st_size < (off_t)min_size) continue;
            if (match_pattern(pattern, entry->d_name)) {
                printf("[Worker PID:%d] MATCH: %s (%lld bytes)\n",
                       (int)getpid(), fullpath, (long long)st.st_size);
                fflush(stdout);
                if (result_fp) {
                    fprintf(result_fp, "[Worker PID:%d] MATCH: %s (%lld bytes)\n",
                            (int)getpid(), fullpath, (long long)st.st_size);
                    fflush(result_fp);
                }
                (*match_count)++;
            }
        }
    }
    closedir(dp);
}

void run_worker(const WorkerArgs *args)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = worker_sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);

    FILE *result_fp = NULL;
    if (args->result_file)
        result_fp = fopen(args->result_file, "w");

    int match_count = 0;
    for (int i = 0; i < args->num_dirs; i++) {
        if (g_sigterm_received) break;
        search_dir(args->dirs[i], args->pattern, args->min_size, result_fp, &match_count);
    }

    if (result_fp) fclose(result_fp);

    if (g_sigterm_received) {
        printf("[Worker PID:%d] SIGTERM received. Partial matches: %d. Exiting.\n",
               (int)getpid(), match_count);
        fflush(stdout);
        exit(match_count % 256);
    }

    kill(args->parent_pid, SIGUSR1);
    exit(match_count % 256);
}
