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

#define MAX_WORKERS  8
#define MIN_WORKERS  2
#define MAX_SUBDIRS  512
#define MAX_MATCHES  4096

static volatile sig_atomic_t g_sigusr1_count   = 0;
static volatile sig_atomic_t g_sigint_received  = 0;
static volatile sig_atomic_t g_sigchld_received = 0;

static pid_t g_child_pids[MAX_WORKERS];
static int   g_num_children = 0;
static int   g_num_workers  = 0;

static pid_t                 g_reaped_pids    [MAX_WORKERS];
static int                   g_reaped_statuses[MAX_WORKERS];
static volatile sig_atomic_t g_num_reaped     = 0;
static int                   g_sigchld_processed = 0;

static void handler_sigusr1(int sig) { (void)sig; g_sigusr1_count++;    }
static void handler_sigint (int sig) { (void)sig; g_sigint_received = 1; }
static void handler_sigchld(int sig)
{
    (void)sig;
    int   status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        sig_atomic_t num_already_reaped = g_num_reaped;
        if (num_already_reaped < MAX_WORKERS) {
            g_reaped_pids    [num_already_reaped] = pid;
            g_reaped_statuses[num_already_reaped] = status;
            g_num_reaped                          = num_already_reaped + 1;
        }
    }
    g_sigchld_received = 1;
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = handler_sigusr1;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    sa.sa_handler = handler_sigint;
    sa.sa_flags   = 0;
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = handler_sigchld;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

static void process_sigchld(WorkerResult *results, int *num_results)
{
    int n = (int)g_num_reaped;
    for (int j = g_sigchld_processed; j < n; j++) {
        pid_t pid       = g_reaped_pids[j];
        int   status    = g_reaped_statuses[j];
        int   exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 0;

        int known = 0;
        for (int i = 0; i < g_num_children; i++) {
            if (g_child_pids[i] == pid) { known = 1; break; }
        }

        if (known) {
            int all_done = (g_sigusr1_count >= (sig_atomic_t)g_num_workers);
            if (!all_done && (!WIFEXITED(status) || WIFSIGNALED(status))) {
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
    g_sigchld_processed = n;
}

static void terminate_all_workers(void)
{
    for (int i = 0; i < g_num_children; i++) {
        kill(g_child_pids[i], SIGTERM);
    }

    time_t deadline = time(NULL) + 3;
    while (time(NULL) < deadline) {
        int all_gone = 1;
        for (int i = 0; i < g_num_children; i++) {
            if (waitpid(g_child_pids[i], NULL, WNOHANG) == 0) { all_gone = 0; }
        }
        if (all_gone) { return; }
        struct timespec ts = {0, 100000000L};
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < g_num_children; i++) {
        kill(g_child_pids[i], SIGKILL);
    }
    int st;
    while (waitpid(-1, &st, WNOHANG) > 0) {
        ;
    }
}

static int count_total_files(const char *path)
{
    DIR *dir_stream = opendir(path);
    if (!dir_stream) { return 0; }
    int count = 0;
    struct dirent *dir_entry;
    while ((dir_entry = readdir(dir_stream)) != NULL) {
        if (!strcmp(dir_entry->d_name, ".") || !strcmp(dir_entry->d_name, "..")) { continue; }
        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, dir_entry->d_name);
        struct stat file_stat;
        if (lstat(fullpath, &file_stat) < 0) { continue; }
        if (S_ISDIR(file_stat.st_mode)) { count += count_total_files(fullpath); }
        else if (S_ISREG(file_stat.st_mode)) { count++; }
    }
    closedir(dir_stream);
    return count;
}

static void collect_matches(const char *path, const char *pattern, long min_size,
                             MatchEntry *entries, int *num_matches, pid_t pid)
{
    DIR *dir_stream = opendir(path);
    if (!dir_stream) { return; }
    struct dirent *dir_entry;
    while ((dir_entry = readdir(dir_stream)) != NULL) {
        if (!strcmp(dir_entry->d_name, ".") || !strcmp(dir_entry->d_name, "..")) { continue; }
        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, dir_entry->d_name);
        struct stat file_stat;
        if (lstat(fullpath, &file_stat) < 0) { continue; }
        if (S_ISDIR(file_stat.st_mode)) {
            collect_matches(fullpath, pattern, min_size, entries, num_matches, pid);
        } else if (S_ISREG(file_stat.st_mode)) {
            if (min_size >= 0 && file_stat.st_size < (off_t)min_size) { continue; }
            if (match_pattern(pattern, dir_entry->d_name) && *num_matches < MAX_MATCHES) {
                strncpy(entries[*num_matches].path, fullpath, 4095);
                entries[*num_matches].path[4095] = '\0';
                entries[*num_matches].size       = (long)file_stat.st_size;
                entries[*num_matches].worker_pid = pid;
                (*num_matches)++;
            }
        }
    }
    closedir(dir_stream);
}

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

static char g_tmpfiles[MAX_WORKERS][64];
static int  g_num_tmpfiles = 0;
static int  g_is_parent    = 0;

static void cleanup_tmpfiles(void)
{
    if (!g_is_parent) { return; }
    for (int i = 0; i < g_num_tmpfiles; i++) {
        unlink(g_tmpfiles[i]);
    }
}

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

    char *subdirs[MAX_SUBDIRS];
    int   num_subdirs = 0;
    {
        DIR *dir_stream = opendir(root_dir);
        if (!dir_stream) { perror("opendir"); return 1; }
        struct dirent *dir_entry;
        while ((dir_entry = readdir(dir_stream)) != NULL && num_subdirs < MAX_SUBDIRS) {
            if (!strcmp(dir_entry->d_name, ".") || !strcmp(dir_entry->d_name, "..")) { continue; }
            char fullpath[4096];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", root_dir, dir_entry->d_name);
            struct stat file_stat;
            if (lstat(fullpath, &file_stat) < 0) { continue; }
            if (S_ISDIR(file_stat.st_mode)) {
                subdirs[num_subdirs] = strdup(fullpath);
                if (!subdirs[num_subdirs]) { perror("strdup"); closedir(dir_stream); return 1; }
                num_subdirs++;
            }
        }
        closedir(dir_stream);
    }

    static MatchEntry match_entries[MAX_MATCHES];
    int num_entries = 0;
    WorkerResult worker_results[MAX_WORKERS];
    int num_results = 0;

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

    if (num_subdirs < num_workers) {
        printf("Notice: only %d subdirector%s found; using %d worker%s instead of %d.\n",
               num_subdirs, num_subdirs == 1 ? "y" : "ies",
               num_subdirs, num_subdirs == 1 ? "" : "s",
               num_workers);
        num_workers = num_subdirs;
    }
    g_num_workers = num_workers;

    char *wdirs[MAX_WORKERS][MAX_SUBDIRS + 1];
    int   wdir_count[MAX_WORKERS];
    memset(wdir_count, 0, sizeof(wdir_count));
    for (int i = 0; i < num_subdirs; i++) {
        int w = i % num_workers;
        wdirs[w][wdir_count[w]++] = subdirs[i];
    }
    for (int w = 0; w < num_workers; w++) {
        wdirs[w][wdir_count[w]] = NULL;
    }

    for (int w = 0; w < num_workers; w++) {
        snprintf(g_tmpfiles[w], sizeof(g_tmpfiles[w]),
                 "/tmp/procsearch_%d_XXXXXX", (int)getpid());
        int fd = mkstemp(g_tmpfiles[w]);
        if (fd < 0) { perror("mkstemp"); return 1; }
        close(fd);
        g_num_tmpfiles++;
    }
    atexit(cleanup_tmpfiles);

    install_signals();

    pid_t parent_pid = getpid();

    fflush(stdout);
    fflush(stderr);

    for (int w = 0; w < num_workers; w++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }

        if (pid == 0) {
            WorkerArgs args;
            args.parent_pid  = parent_pid;
            args.dirs        = wdirs[w];
            args.num_dirs    = wdir_count[w];
            args.pattern     = pattern;
            args.min_size    = min_size;
            args.result_file = g_tmpfiles[w];
            run_worker(&args);
        }

        g_child_pids[g_num_children++] = pid;
    }

    g_is_parent = 1;

    sigset_t mask_all, mask_waiting;
    sigfillset(&mask_all);
    sigemptyset(&mask_waiting);

    while (g_sigusr1_count < (sig_atomic_t)g_num_workers) {

        if (g_sigint_received) {
            fprintf(stderr, "\n[Parent] SIGINT received. Terminating all workers...\n");
            terminate_all_workers();
            fprintf(stderr, "[Parent] Partial shutdown complete.\n");
            fprintf(stderr, "Matches found so far: %d\n", num_entries);
            for (int i = 0; i < num_subdirs; i++) { free(subdirs[i]); }
            return 1;
        }

        if (g_sigchld_received) {
            g_sigchld_received = 0;
            process_sigchld(worker_results, &num_results);
        }

        sigsuspend(&mask_waiting);
    }

    sigset_t block_chld, old_sigmask;
    sigemptyset(&block_chld);
    sigaddset(&block_chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block_chld, &old_sigmask);

    num_results = 0;
    for (int j = 0; j < (int)g_num_reaped; j++) {
        pid_t pid       = g_reaped_pids[j];
        int   exit_code = WIFEXITED(g_reaped_statuses[j]) ? WEXITSTATUS(g_reaped_statuses[j]) : 0;
        for (int i = 0; i < g_num_children; i++) {
            if (g_child_pids[i] == pid && num_results < MAX_WORKERS) {
                worker_results[num_results].worker_pid  = pid;
                worker_results[num_results].match_count = exit_code;
                num_results++;
                break;
            }
        }
    }
    for (int i = 0; i < g_num_children; i++) {
        int already = 0;
        for (int j = 0; j < num_results; j++) {
            if (worker_results[j].worker_pid == g_child_pids[i]) { already = 1; break; }
        }
        if (!already) {
            int status;
            if (waitpid(g_child_pids[i], &status, 0) > 0 && num_results < MAX_WORKERS) {
                worker_results[num_results].worker_pid  = g_child_pids[i];
                worker_results[num_results].match_count =
                    WIFEXITED(status) ? WEXITSTATUS(status) : 0;
                num_results++;
            }
        }
    }

    sigprocmask(SIG_SETMASK, &old_sigmask, NULL);

    for (int w = 0; w < num_workers; w++) {
        FILE *fp = fopen(g_tmpfiles[w], "r");
        if (!fp) { continue; }
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            size_t ln = strlen(line);
            if (ln > 0 && line[ln-1] == '\n') { line[ln-1] = '\0'; }
            MatchEntry me;
            if (parse_match_line(line, &me) && num_entries < MAX_MATCHES) {
                match_entries[num_entries++] = me;
            }
        }
        fclose(fp);
    }

    int total_scanned = count_total_files(root_dir);
    int total_matches = 0;
    for (int i = 0; i < num_results; i++) {
        total_matches += worker_results[i].match_count;
    }

    printf("\n");
    print_tree(root_dir, match_entries, num_entries);
    print_summary(num_workers, total_scanned, total_matches,
                  worker_results, num_results);

    for (int i = 0; i < num_subdirs; i++) { free(subdirs[i]); }
    return 0;
}
