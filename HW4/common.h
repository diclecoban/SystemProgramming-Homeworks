#ifndef HW4_COMMON_H
#define HW4_COMMON_H

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <semaphore.h>
#include <sys/syscall.h>
#endif

#define MAX_KEYWORDS 8
#define MAX_WORKERS 64
#define MAX_LEVELS 4
#define MAX_SOURCES 256
#define MAX_FILES 128
#define MAX_PATH_LEN 512
#define MAX_MESSAGE_LEN 8192
#define MAX_LEVEL_NAME 8
#define MAGIC_CHECKPOINT 0xC5E3440BU

typedef enum {
    LEVEL_ERROR = 0,
    LEVEL_WARN = 1,
    LEVEL_INFO = 2,
    LEVEL_DEBUG = 3,
    LEVEL_INVALID = -1
} log_level_t;

typedef struct {
    char timestamp[20];
    char level[MAX_LEVEL_NAME];
    char source[64];
    char message[MAX_MESSAGE_LEN];
    int level_index;
    int is_eof;
    int reader_id;
} log_entry_t;

typedef struct {
    char level[8];
    long total_entries;
    double total_weighted_score;
    double per_keyword_score[MAX_KEYWORDS];
    double per_thread_score[MAX_WORKERS];
    char top_source[3][64];
    long top_source_hits[3];
    int ready;
} level_result_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t num_levels;
    uint32_t num_keywords;
    double total_weighted;
    double high_priority_weighted;
} checkpoint_header_t;

typedef struct {
    int head;
    int tail;
    int count;
    int capacity;
    int total_readers;
    int eof_count_per_level[MAX_LEVELS];
    pthread_mutex_t input_mutex;
    pthread_cond_t not_full_a;
    pthread_cond_t not_empty_a;
    log_entry_t entries[];
} region_a_t;

typedef struct {
    int head;
    int tail;
    int count;
    int capacity;
    int eof_posted;
    pthread_mutex_t level_mutex;
    pthread_cond_t not_full_b;
    pthread_cond_t not_empty_b;
    log_entry_t entries[];
} region_b_level_t;

typedef struct {
    int head;
    int tail;
    int count;
    int capacity;
    int dispatcher_done;
    pthread_mutex_t priority_mutex;
    pthread_cond_t not_full_d;
    pthread_cond_t not_empty_d;
    log_entry_t entries[];
} region_d_t;

typedef struct region_c region_c_t;

typedef struct {
    int reader_threads;
    int worker_threads;
    int capacity_a;
    int capacity_b;
    int capacity_d;
    int timeout_sec;
    int num_keywords;
    int num_files;
    int num_priority_sources;
    char config_file[MAX_PATH_LEN];
    char filter_file[MAX_PATH_LEN];
    char output_file[MAX_PATH_LEN];
    char binary_file[MAX_PATH_LEN];
    char keywords[MAX_KEYWORDS][64];
    char files[MAX_FILES][MAX_PATH_LEN];
    char priority_sources[MAX_SOURCES][64];
} program_options_t;

typedef struct {
    region_a_t *region_a;
    region_b_level_t *region_b[MAX_LEVELS];
    region_c_t *region_c;
    region_d_t *region_d;
    size_t region_a_size;
    size_t region_b_size;
    size_t region_c_size;
    size_t region_d_size;
} shared_regions_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
    int head;
    int tail;
    int count;
    int capacity;
    int producers_done;
    log_entry_t *entries;
} private_buffer_t;

typedef struct {
    program_options_t *opts;
    shared_regions_t *shared;
    int reader_id;
    const char *file_path;
    int heartbeat_fd;
} reader_process_args_t;

typedef struct {
    program_options_t *opts;
    shared_regions_t *shared;
} dispatcher_process_args_t;

typedef struct {
    program_options_t *opts;
    shared_regions_t *shared;
    int level_index;
} analyzer_process_args_t;

typedef struct {
    program_options_t *opts;
    shared_regions_t *shared;
} aggregator_process_args_t;

typedef struct {
    program_options_t *opts;
    pid_t *child_pids;
    int child_count;
    int *pipe_fds;
    volatile sig_atomic_t *shutdown_flag;
} watchdog_args_t;

extern volatile sig_atomic_t g_sigint_received;

#ifdef __linux__
typedef sem_t hw_sem_t;

static inline int hw_sem_init(hw_sem_t *sem, int pshared, unsigned value) {
    return sem_init(sem, pshared, value);
}

static inline int hw_sem_post(hw_sem_t *sem) {
    return sem_post(sem);
}

static inline int hw_sem_wait(hw_sem_t *sem) {
    return sem_wait(sem);
}

static inline int hw_sem_destroy(hw_sem_t *sem) {
    return sem_destroy(sem);
}
#else
typedef struct hw_sem {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned count;
} hw_sem_t;

static inline int hw_sem_init(hw_sem_t *sem, int pshared, unsigned value) {
    (void)pshared;
    pthread_mutex_init(&sem->mutex, NULL);
    pthread_cond_init(&sem->cond, NULL);
    sem->count = value;
    return 0;
}

static inline int hw_sem_post(hw_sem_t *sem) {
    pthread_mutex_lock(&sem->mutex);
    sem->count++;
    pthread_cond_signal(&sem->cond);
    pthread_mutex_unlock(&sem->mutex);
    return 0;
}

static inline int hw_sem_wait(hw_sem_t *sem) {
    pthread_mutex_lock(&sem->mutex);
    while (sem->count == 0) {
        pthread_cond_wait(&sem->cond, &sem->mutex);
    }
    sem->count--;
    pthread_mutex_unlock(&sem->mutex);
    return 0;
}

static inline int hw_sem_destroy(hw_sem_t *sem) {
    pthread_mutex_destroy(&sem->mutex);
    pthread_cond_destroy(&sem->cond);
    return 0;
}
#endif

typedef struct region_c {
    level_result_t results[MAX_LEVELS];
    hw_sem_t level_ready[MAX_LEVELS];
    pthread_mutex_t result_mutex;
    pthread_cond_t result_cond;
    double high_priority_score;
    long high_priority_entries;
} region_c_t;

#ifndef __linux__
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned count;
    unsigned trip_count;
    unsigned generation;
} pthread_barrier_t;

static inline int pthread_barrier_init(pthread_barrier_t *barrier,
                                       const void *attr,
                                       unsigned count) {
    (void)attr;
    if (count == 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_init(&barrier->mutex, NULL);
    pthread_cond_init(&barrier->cond, NULL);
    barrier->count = 0;
    barrier->trip_count = count;
    barrier->generation = 0;
    return 0;
}

static inline int pthread_barrier_wait(pthread_barrier_t *barrier) {
    unsigned generation;
    pthread_mutex_lock(&barrier->mutex);
    generation = barrier->generation;
    barrier->count++;
    if (barrier->count == barrier->trip_count) {
        barrier->generation++;
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return 1;
    }
    while (generation == barrier->generation) {
        pthread_cond_wait(&barrier->cond, &barrier->mutex);
    }
    pthread_mutex_unlock(&barrier->mutex);
    return 0;
}

static inline int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    pthread_mutex_destroy(&barrier->mutex);
    pthread_cond_destroy(&barrier->cond);
    return 0;
}
#endif

static inline pid_t get_system_tid(void) {
#ifdef __linux__
    return (pid_t)syscall(SYS_gettid);
#else
    return (pid_t)(uintptr_t)pthread_self();
#endif
}

const char *level_name(int level);
int level_weight(int level);
int parse_level(const char *level_str);
int parse_log_line(const char *line, log_entry_t *entry);
long count_overlapping_keyword(const char *haystack, const char *needle);
void trim_newline(char *s);
void die_errno(const char *msg);
void die_message(const char *msg);
int timed_wait_seconds(struct timespec *ts, int timeout_sec);

#endif
#ifdef __linux__
#include <semaphore.h>
#endif
