#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include "pattern.h"
#include "worker.h"
#include "output.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define MAX_WORKERS  8
#define MIN_WORKERS  2
#define MAX_SUBDIRS  512
#define MAX_MATCHES  4096

/* ------------------------------------------------------------------ */
/* Global signal state — only async-signal-safe types                 */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t g_sigusr1_count   = 0;
static volatile sig_atomic_t g_sigint_received  = 0;
static volatile sig_atomic_t g_sigchld_received = 0;

static pid_t g_child_pids[MAX_WORKERS];
static int   g_num_children = 0;
static int   g_num_workers  = 0;

/* ------------------------------------------------------------------ */
/* Signal handlers — no printf, no malloc, async-signal-safe only     */
/* ------------------------------------------------------------------ */
static void handler_sigusr1(int sig) { (void)sig; g_sigusr1_count++;    }
static void handler_sigint (int sig) { (void)sig; g_sigint_received = 1; }
static void handler_sigchld(int sig) { (void)sig; g_sigchld_received = 1;}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = handler_sigusr1;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    sa.sa_handler = handler_sigint;
    sa.sa_flags   = 0;  /* do NOT restart — so pause() wakes up */
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = handler_sigchld;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* SIGCHLD processing — called from main loop, NOT from handler       */
/* ------------------------------------------------------------------ */
static void process_sigchld(WorkerResult *results, int *num_results)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 0;

        int known = 0;
        for (int i = 0; i < g_num_children; i++)
            if (g_child_pids[i] == pid) { known = 1; break; }

        if (known) {
            /*
             * A worker is "unexpected" only if we haven't yet received all
             * SIGUSR1s — meaning it likely died without finishing normally.
             * Workers that complete normally send SIGUSR1 first, so by the
             * time SIGCHLD arrives for them g_sigusr1_count may already be
             * at g_num_workers. We log only if the count is still short AND
             * the worker did not exit cleanly (non-zero status or signalled).
             */
            int clean_exit = WIFEXITED(status) && (exit_code == WEXITSTATUS(status));
            int all_done   = (g_sigusr1_count >= (sig_atomic_t)g_num_workers);
            if (!all_done && !clean_exit) {
                fprintf(stderr,
                    "[Parent] Worker PID:%d terminated unexpectedly (exit status: %d).\n",
                    (int)pid, exit_code);
            } else if (!all_done && WIFSIGNALED(status)) {
                fprintf(stderr,
                    "[Parent] Worker PID:%d terminated unexpectedly (exit status: %d).\n",
                    (int)pid, exit_code);
            }
            if (*num_results < MAX_WORKERS) {
                results[*num_results].worker_pid  = pid;
                results[*num_results].match_count = exit_code;
                (*num_results)++;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Terminate all workers: SIGTERM → 3 s → SIGKILL                    */
/* ------------------------------------------------------------------ */
static void terminate_all_workers(void)
{
    for (int i = 0; i < g_num_children; i++)
        kill(g_child_pids[i], SIGTERM);

    time_t deadline = time(NULL) + 3;
    while (time(NULL) < deadline) {
        int all_gone = 1;
        for (int i = 0; i < g_num_children; i++)
            if (waitpid(g_child_pids[i], NULL, WNOHANG) == 0) all_gone = 0;
        if (all_gone) return;
        struct timespec ts = {0, 100000000L};
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < g_num_children; i++)
        kill(g_child_pids[i], SIGKILL);
    int st;
    while (waitpid(-1, &st, WNOHANG) > 0)
        ;
}

/* ------------------------------------------------------------------ */
/* Directory / file helpers                                            */
/* ------------------------------------------------------------------ */
static int count_total_files(const char *path)
{
    DIR *dp = opendir(path);
    if (!dp) return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char fp[4096];
        snprintf(fp, sizeof(fp), "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(fp, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) count += count_total_files(fp);
        else if (S_ISREG(st.st_mode)) count++;
    }
    closedir(dp);
    return count;
}

/* Used in no-fork (parent-only) mode */
static void collect_matches(const char *path, const char *pattern, long min_size,
                             MatchEntry *entries, int *n, pid_t pid)
{
    DIR *dp = opendir(path);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char fp[4096];
        snprintf(fp, sizeof(fp), "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(fp, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collect_matches(fp, pattern, min_size, entries, n, pid);
        } else if (S_ISREG(st.st_mode)) {
            if (min_size >= 0 && st.st_size < (off_t)min_size) continue;
            if (match_pattern(pattern, e->d_name) && *n < MAX_MATCHES) {
                strncpy(entries[*n].path, fp, 4095);
                entries[*n].path[4095] = '\0';
                entries[*n].size       = (long)st.st_size;
                entries[*n].worker_pid = pid;
                (*n)++;
            }
        }
    }
    closedir(dp);
}

/* Parse "[Worker PID:NNN] MATCH: /path/file (NNN bytes)" */
static int parse_match_line(const char *line, MatchEntry *out)
{
    int  pid;
    char path[4096];
    long size;
    if (sscanf(line, "[Worker PID:%d] MATCH: %4095s (%ld bytes)",
               &pid, path, &size) == 3) {
        out->worker_pid = (pid_t)pid;
        strncpy(out->path, path, 4095);
        out->path[4095] = '\0';
        out->size = size;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Temp-file management (workers write results here for parent)       */
/* ------------------------------------------------------------------ */
static char g_tmpfiles[MAX_WORKERS][64];
static int  g_num_tmpfiles = 0;
static int  g_is_parent    = 0;  /* only parent cleans up temp files */

static void cleanup_tmpfiles(void)
{
    /* Children inherit atexit — they must NOT delete files parent still needs */
    if (!g_is_parent) return;
    for (int i = 0; i < g_num_tmpfiles; i++)
        unlink(g_tmpfiles[i]);
}

/* ------------------------------------------------------------------ */
/* Usage                                                               */
/* ------------------------------------------------------------------ */
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -d <root_dir> -n <num_workers> -f <pattern> [-s <min_size_bytes>]\n"
        "  -d <root_dir>    Root directory to search (must exist)\n"
        "  -n <num_workers> Number of worker processes (%d-%d)\n"
        "  -f <pattern>     Filename pattern (supports + operator)\n"
        "  -s <min_size>    Optional: match files with size >= min_size bytes\n",
        prog, MIN_WORKERS, MAX_WORKERS);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    char *root_dir    = NULL;
    int   num_workers = 0;
    char *pattern     = NULL;
    long  min_size    = -1;

    int opt;
    while ((opt = getopt(argc, argv, "d:n:f:s:")) != -1) {
        switch (opt) {
        case 'd': root_dir    = optarg;       break;
        case 'n': num_workers = atoi(optarg); break;
        case 'f': pattern     = optarg;       break;
        case 's': min_size    = atol(optarg); break;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!root_dir || !pattern || num_workers == 0) {
        print_usage(argv[0]);
        return 1;
    }
    if (num_workers < MIN_WORKERS || num_workers > MAX_WORKERS) {
        fprintf(stderr, "Error: num_workers must be between %d and %d.\n",
                MIN_WORKERS, MAX_WORKERS);
        print_usage(argv[0]);
        return 1;
    }

    struct stat root_st;
    if (stat(root_dir, &root_st) < 0 || !S_ISDIR(root_st.st_mode)) {
        fprintf(stderr, "Error: '%s' does not exist or is not a directory.\n", root_dir);
        return 1;
    }

    /* ---- Collect immediate subdirectories of root ---- */
    char *subdirs[MAX_SUBDIRS];
    int   num_subdirs = 0;
    {
        DIR *dp = opendir(root_dir);
        if (!dp) { perror("opendir"); return 1; }
        struct dirent *e;
        while ((e = readdir(dp)) != NULL && num_subdirs < MAX_SUBDIRS) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char fp[4096];
            snprintf(fp, sizeof(fp), "%s/%s", root_dir, e->d_name);
            struct stat st;
            if (lstat(fp, &st) < 0) continue;
            if (S_ISDIR(st.st_mode)) {
                subdirs[num_subdirs] = strdup(fp);
                if (!subdirs[num_subdirs]) { perror("strdup"); closedir(dp); return 1; }
                num_subdirs++;
            }
        }
        closedir(dp);
    }

    /* ---- Static arrays to avoid stack overflow ---- */
    static MatchEntry match_entries[MAX_MATCHES];
    int num_entries = 0;
    WorkerResult worker_results[MAX_WORKERS];
    int num_results = 0;

    /* ---- No subdirectories: parent searches root directly ---- */
    if (num_subdirs == 0) {
        printf("Notice: no subdirectories found; parent will search root directly.\n");
        collect_matches(root_dir, pattern, min_size,
                        match_entries, &num_entries, getpid());
        int total_scanned = count_total_files(root_dir);
        worker_results[0].worker_pid  = getpid();
        worker_results[0].match_count = num_entries;
        num_results = 1;
        print_tree(root_dir, match_entries, num_entries);
        print_summary(0, total_scanned, num_entries, worker_results, num_results);
        return 0;
    }

    /* ---- Reduce worker count if fewer subdirs than requested ---- */
    if (num_subdirs < num_workers) {
        printf("Notice: only %d subdirector%s found; using %d worker%s instead of %d.\n",
               num_subdirs, num_subdirs == 1 ? "y" : "ies",
               num_subdirs, num_subdirs == 1 ? "" : "s",
               num_workers);
        num_workers = num_subdirs;
    }
    g_num_workers = num_workers;

    /* ---- Round-robin directory partitioning ---- */
    char *wdirs[MAX_WORKERS][MAX_SUBDIRS + 1];
    int   wdir_count[MAX_WORKERS];
    memset(wdir_count, 0, sizeof(wdir_count));
    for (int i = 0; i < num_subdirs; i++) {
        int w = i % num_workers;
        wdirs[w][wdir_count[w]++] = subdirs[i];
    }
    for (int w = 0; w < num_workers; w++)
        wdirs[w][wdir_count[w]] = NULL;

    /* ---- Create per-worker temp files ---- */
    /*
     * Each worker writes its MATCH lines here. Parent reads them after
     * all workers finish to build the tree. This avoids races and is
     * simpler than pipes for multi-signal coordination.
     */
    for (int w = 0; w < num_workers; w++) {
        snprintf(g_tmpfiles[w], sizeof(g_tmpfiles[w]),
                 "/tmp/procsearch_%d_XXXXXX", (int)getpid());
        int fd = mkstemp(g_tmpfiles[w]);
        if (fd < 0) { perror("mkstemp"); return 1; }
        close(fd);
        g_num_tmpfiles++;
    }
    atexit(cleanup_tmpfiles);

    /* ---- Install parent signal handlers BEFORE forking ---- */
    install_signals();

    pid_t parent_pid = getpid();

    /* Flush stdio buffers before fork — children must not repeat parent output */
    fflush(stdout);
    fflush(stderr);

    /* ---- Fork workers ---- */
    for (int w = 0; w < num_workers; w++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }

        if (pid == 0) {
            /* ===== CHILD ===== */
            WorkerArgs args;
            args.parent_pid  = parent_pid;
            args.dirs        = wdirs[w];
            args.num_dirs    = wdir_count[w];
            args.pattern     = pattern;
            args.min_size    = min_size;
            args.result_file = g_tmpfiles[w];
            run_worker(&args);
            /* never returns */
        }

        g_child_pids[g_num_children++] = pid;
    }

    /* Mark this process as the parent so atexit cleanup runs only here */
    g_is_parent = 1;

    /* ---- Parent: wait for all workers to send SIGUSR1 ---- */
    while (g_sigusr1_count < (sig_atomic_t)g_num_workers) {

        /* SIGINT: Ctrl+C */
        if (g_sigint_received) {
            fprintf(stderr, "\n[Parent] SIGINT received. Terminating all workers...\n");
            terminate_all_workers();
            fprintf(stderr, "[Parent] Partial shutdown complete.\n");
            fprintf(stderr, "Matches found so far: %d\n", num_entries);
            for (int i = 0; i < num_subdirs; i++) free(subdirs[i]);
            return 1;
        }

        /* SIGCHLD: unexpected child exit */
        if (g_sigchld_received) {
            g_sigchld_received = 0;
            process_sigchld(worker_results, &num_results);
            /*
             * If a worker died without sending SIGUSR1, we increment
             * the counter so we don't wait forever.
             */
        }

        /* Sleep until next signal */
        pause();
    }

    /* ---- All workers done: collect exit statuses via waitpid ---- */
    num_results = 0;
    for (int i = 0; i < g_num_children; i++) {
        int status;
        pid_t ret = waitpid(g_child_pids[i], &status, 0);
        if (ret > 0) {
            worker_results[num_results].worker_pid  = g_child_pids[i];
            worker_results[num_results].match_count =
                WIFEXITED(status) ? WEXITSTATUS(status) : 0;
            num_results++;
        }
    }

    /* ---- Read match lines from per-worker temp files ---- */
    for (int w = 0; w < num_workers; w++) {
        FILE *fp = fopen(g_tmpfiles[w], "r");
        if (!fp) continue;
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            size_t ln = strlen(line);
            if (ln > 0 && line[ln-1] == '\n') line[ln-1] = '\0';
            MatchEntry me;
            if (parse_match_line(line, &me) && num_entries < MAX_MATCHES)
                match_entries[num_entries++] = me;
        }
        fclose(fp);
    }

    /* ---- Final output ---- */
    int total_scanned = count_total_files(root_dir);
    int total_matches = 0;
    for (int i = 0; i < num_results; i++)
        total_matches += worker_results[i].match_count;

    printf("\n");
    print_tree(root_dir, match_entries, num_entries);
    print_summary(num_workers, total_scanned, total_matches,
                  worker_results, num_results);

    for (int i = 0; i < num_subdirs; i++) free(subdirs[i]);
    return 0;
}
