#define _POSIX_C_SOURCE 200809L

#include "aggregator.h"
#include "analyzer.h"
#include "dispatcher.h"
#include "reader.h"
#include "shm.h"
#include "watchdog.h"

typedef struct {
    pid_t pids[MAX_FILES + 6];
    int count;
} child_list_t;

static void mark_child_reaped(child_list_t *children, pid_t pid) {
    int i;
    for (i = 0; i < children->count; ++i) {
        if (children->pids[i] == pid) {
            children->pids[i] = 0;
            return;
        }
    }
}

static void terminate_children(child_list_t *children, int timeout_sec) {
    time_t deadline = time(NULL) + timeout_sec;
    int i;

    for (i = 0; i < children->count; ++i) {
        if (children->pids[i] > 0) {
            kill(children->pids[i], SIGTERM);
        }
    }

    while (time(NULL) < deadline) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            mark_child_reaped(children, pid);
            continue;
        }
        if (pid == 0) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 100000000L;
            nanosleep(&ts, NULL);
            continue;
        }
        if (errno == ECHILD) {
            break;
        }
        if (errno != EINTR) {
            break;
        }
    }

    for (i = 0; i < children->count; ++i) {
        if (children->pids[i] > 0) {
            kill(children->pids[i], SIGKILL);
        }
    }

    while (waitpid(-1, NULL, WNOHANG) > 0) {
        ;
    }
}

static void sigint_handler(int signo) {
    (void)signo;
    g_sigint_received = 1;
}

static void install_sigint_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        die_errno("sigaction");
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s -c <config> -f <filter> -k <keywords> -t <reader_threads> "
            "-w <worker_threads> -a <capA> -b <capB> -d <capD> [-T timeout] "
            "-o <output> -O <binary>\n",
            prog);
}

static void parse_keywords(program_options_t *opts, const char *arg) {
    char buf[512];
    char *saveptr = NULL;
    char *token;

    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    token = strtok_r(buf, ",", &saveptr);
    while (token != NULL) {
        if (opts->num_keywords >= MAX_KEYWORDS) {
            die_message("Too many keywords");
        }
        strncpy(opts->keywords[opts->num_keywords], token, sizeof(opts->keywords[0]) - 1);
        opts->num_keywords++;
        token = strtok_r(NULL, ",", &saveptr);
    }
    if (opts->num_keywords == 0) {
        die_message("At least one keyword required");
    }
}

static void load_file_paths(const char *path, char out[][MAX_PATH_LEN], int *count, int max_count) {
    FILE *fp = fopen(path, "r");
    char line[MAX_PATH_LEN];
    if (fp == NULL) {
        die_errno("fopen config/filter");
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        trim_newline(line);
        if (line[0] == '\0') {
            continue;
        }
        if (*count >= max_count) {
            die_message("Too many lines in config/filter");
        }
        strncpy(out[*count], line, MAX_PATH_LEN - 1);
        (*count)++;
    }
    fclose(fp);
}

static void load_sources_simple(const char *path, char out[][64], int *count, int max_count) {
    FILE *fp = fopen(path, "r");
    char line[128];
    if (fp == NULL) {
        die_errno("fopen filter");
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        trim_newline(line);
        if (line[0] == '\0') {
            continue;
        }
        if (*count >= max_count) {
            die_message("Too many lines in filter");
        }
        strncpy(out[*count], line, 63);
        out[*count][63] = '\0';
        (*count)++;
    }
    fclose(fp);
}

static void parse_args(int argc, char **argv, program_options_t *opts) {
    int ch;
    memset(opts, 0, sizeof(*opts));
    opts->timeout_sec = 10;

    while ((ch = getopt(argc, argv, "c:f:k:t:w:a:b:d:T:o:O:")) != -1) {
        switch (ch) {
            case 'c':
                strncpy(opts->config_file, optarg, sizeof(opts->config_file) - 1);
                break;
            case 'f':
                strncpy(opts->filter_file, optarg, sizeof(opts->filter_file) - 1);
                break;
            case 'k':
                parse_keywords(opts, optarg);
                break;
            case 't':
                opts->reader_threads = atoi(optarg);
                break;
            case 'w':
                opts->worker_threads = atoi(optarg);
                break;
            case 'a':
                opts->capacity_a = atoi(optarg);
                break;
            case 'b':
                opts->capacity_b = atoi(optarg);
                break;
            case 'd':
                opts->capacity_d = atoi(optarg);
                break;
            case 'T':
                opts->timeout_sec = atoi(optarg);
                break;
            case 'o':
                strncpy(opts->output_file, optarg, sizeof(opts->output_file) - 1);
                break;
            case 'O':
                strncpy(opts->binary_file, optarg, sizeof(opts->binary_file) - 1);
                break;
            default:
                usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (opts->config_file[0] == '\0' || opts->filter_file[0] == '\0' || opts->num_keywords == 0 ||
        opts->reader_threads < 1 || opts->worker_threads < 1 || opts->worker_threads > MAX_WORKERS ||
        opts->capacity_a < 4 || opts->capacity_b < 4 || opts->capacity_d < 2 ||
        opts->timeout_sec < 1 ||
        opts->output_file[0] == '\0' || opts->binary_file[0] == '\0') {
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    load_file_paths(opts->config_file, opts->files, &opts->num_files, MAX_FILES);
    load_sources_simple(opts->filter_file, opts->priority_sources, &opts->num_priority_sources, MAX_SOURCES);
    if (opts->num_files == 0) {
        die_message("Config file must list at least one log file");
    }
}

static pid_t fork_reader(program_options_t *opts, shared_regions_t *shared, int reader_id, int write_fd) {
    pid_t pid = fork();
    if (pid < 0) {
        die_errno("fork reader");
    }
    if (pid == 0) {
        reader_process_args_t args;
        args.opts = opts;
        args.shared = shared;
        args.reader_id = reader_id;
        args.file_path = opts->files[reader_id];
        args.heartbeat_fd = write_fd;
        run_reader_process(&args);
        exit(EXIT_SUCCESS);
    }
    return pid;
}

static pid_t fork_dispatcher(program_options_t *opts, shared_regions_t *shared) {
    pid_t pid = fork();
    if (pid < 0) {
        die_errno("fork dispatcher");
    }
    if (pid == 0) {
        dispatcher_process_args_t args = {opts, shared};
        run_dispatcher_process(&args);
        exit(EXIT_SUCCESS);
    }
    return pid;
}

static pid_t fork_analyzer(program_options_t *opts, shared_regions_t *shared, int level_index) {
    pid_t pid = fork();
    if (pid < 0) {
        die_errno("fork analyzer");
    }
    if (pid == 0) {
        analyzer_process_args_t args = {opts, shared, level_index};
        run_analyzer_process(&args);
        exit(EXIT_SUCCESS);
    }
    return pid;
}

static pid_t fork_aggregator(program_options_t *opts, shared_regions_t *shared) {
    pid_t pid = fork();
    if (pid < 0) {
        die_errno("fork aggregator");
    }
    if (pid == 0) {
        aggregator_process_args_t args = {opts, shared};
        run_aggregator_process(&args);
        exit(EXIT_SUCCESS);
    }
    return pid;
}

int main(int argc, char **argv) {
    program_options_t opts;
    shared_regions_t shared;
    child_list_t children;
    pthread_t watchdog_thread;
    watchdog_args_t watchdog_args;
    volatile sig_atomic_t shutdown_watchdog = 0;
    int pipe_fds[MAX_FILES][2];
    int read_ends[MAX_FILES];
    int i;
    int status;
    int remaining_children;
    long total_entries = 0;
    double total_weighted = 0.0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    parse_args(argc, argv, &opts);
    install_sigint_handler();
    init_shared_regions(&shared, &opts);
    memset(&children, 0, sizeof(children));

    printf("[PID:%d] Parent started. Files: %d, Keywords: ", getpid(), opts.num_files);
    for (i = 0; i < opts.num_keywords; ++i) {
        printf("%s%s", opts.keywords[i], (i + 1 < opts.num_keywords) ? "," : "");
    }
    printf("\n");
    printf("[PID:%d] Shared memory initialized (A:%d B:%dx4 D:%d).\n",
           getpid(), opts.capacity_a, opts.capacity_b, opts.capacity_d);

    for (i = 0; i < opts.num_files; ++i) {
        if (pipe(pipe_fds[i]) != 0) {
            die_errno("pipe");
        }
        read_ends[i] = pipe_fds[i][0];
        printf("[PID:%d] Forking Reader %d -> %s\n", getpid(), i, opts.files[i]);
        fflush(stdout);
        children.pids[children.count++] = fork_reader(&opts, &shared, i, pipe_fds[i][1]);
        close(pipe_fds[i][1]);
    }

    printf("[PID:%d] Forking Dispatcher\n", getpid());
    fflush(stdout);
    children.pids[children.count++] = fork_dispatcher(&opts, &shared);

    for (i = 0; i < MAX_LEVELS; ++i) {
        printf("[PID:%d] Forking Analyzer %s (index %d)\n", getpid(), level_name(i), i);
        fflush(stdout);
        children.pids[children.count++] = fork_analyzer(&opts, &shared, i);
    }

    printf("[PID:%d] Forking Aggregator\n", getpid());
    fflush(stdout);
    children.pids[children.count++] = fork_aggregator(&opts, &shared);

    watchdog_args.opts = &opts;
    watchdog_args.child_pids = children.pids;
    watchdog_args.child_count = children.count;
    watchdog_args.pipe_fds = read_ends;
    watchdog_args.shutdown_flag = &shutdown_watchdog;
    pthread_create(&watchdog_thread, NULL, watchdog_thread_main, &watchdog_args);
    printf("[PID:%d] Watchdog thread started.\n", getpid());

    remaining_children = children.count;
    while (remaining_children > 0) {
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno == EINTR && g_sigint_received) {
                terminate_children(&children, 5);
                shutdown_watchdog = 1;
                pthread_join(watchdog_thread, NULL);
                destroy_shared_regions(&shared);
                _exit(EXIT_FAILURE);
            }
            if (errno == ECHILD) {
                break;
            }
            die_errno("wait");
        }
        mark_child_reaped(&children, pid);
        remaining_children--;
    }

    shutdown_watchdog = 1;
    pthread_join(watchdog_thread, NULL);

    for (i = 0; i < MAX_LEVELS; ++i) {
        total_entries += shared.region_c->results[i].total_entries;
        total_weighted += shared.region_c->results[i].total_weighted_score;
    }

    printf("==================================================\n");
    printf("SYSTEM SUMMARY\n");
    printf("Keywords : ");
    for (i = 0; i < opts.num_keywords; ++i) {
        printf("%s%s", opts.keywords[i], (i + 1 < opts.num_keywords) ? ", " : "");
    }
    printf("\n");
    printf("Log files : %d\n", opts.num_files);
    printf("Total entries : %ld\n", total_entries);
    printf("Total weighted : %.1f\n", total_weighted);
    printf("High-priority : %.1f (source filter: %s)\n",
           shared.region_c->high_priority_score, opts.filter_file);
    for (i = 0; i < MAX_LEVELS; ++i) {
        printf("%s : %ld entries, score: %.1f\n",
               level_name(i),
               shared.region_c->results[i].total_entries,
               shared.region_c->results[i].total_weighted_score);
    }
    printf("==================================================\n");
    printf("Program terminated successfully.\n");

    destroy_shared_regions(&shared);
    return 0;
}
