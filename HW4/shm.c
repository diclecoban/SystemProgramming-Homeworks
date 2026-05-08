#include "shm.h"

/* Initializes a mutex for process-shared use. */
static void init_pshared_mutex(pthread_mutex_t *mutex) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        die_message("pthread_mutexattr_init failed");
    }
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        die_message("pthread_mutexattr_setpshared failed");
    }
    if (pthread_mutex_init(mutex, &attr) != 0) {
        die_message("pthread_mutex_init failed");
    }
    pthread_mutexattr_destroy(&attr);
}

/* Initializes a condition variable for process-shared use. */
static void init_pshared_cond(pthread_cond_t *cond) {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        die_message("pthread_condattr_init failed");
    }
    if (pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        die_message("pthread_condattr_setpshared failed");
    }
    if (pthread_cond_init(cond, &attr) != 0) {
        die_message("pthread_cond_init failed");
    }
    pthread_condattr_destroy(&attr);
}

/* Creates all shared memory regions and their synchronization objects. */
int init_shared_regions(shared_regions_t *shared, const program_options_t *opts) {
    int i;

    memset(shared, 0, sizeof(*shared));

    shared->region_a_size = sizeof(region_a_t) + (size_t)opts->capacity_a * sizeof(log_entry_t);
    shared->region_b_size = sizeof(region_b_level_t) + (size_t)opts->capacity_b * sizeof(log_entry_t);
    shared->region_c_size = sizeof(region_c_t);
    shared->region_d_size = sizeof(region_d_t) + (size_t)opts->capacity_d * sizeof(log_entry_t);

    shared->region_a = mmap(NULL, shared->region_a_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared->region_a == MAP_FAILED) {
        die_errno("mmap region_a");
    }
    memset(shared->region_a, 0, shared->region_a_size);
    shared->region_a->capacity = opts->capacity_a;
    shared->region_a->total_readers = opts->num_files;
    init_pshared_mutex(&shared->region_a->input_mutex);
    init_pshared_cond(&shared->region_a->not_full_a);
    init_pshared_cond(&shared->region_a->not_empty_a);

    for (i = 0; i < MAX_LEVELS; ++i) {

        shared->region_b[i] = mmap(NULL, shared->region_b_size, PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (shared->region_b[i] == MAP_FAILED) {
            die_errno("mmap region_b");
        }
        memset(shared->region_b[i], 0, shared->region_b_size);
        shared->region_b[i]->capacity = opts->capacity_b;
        init_pshared_mutex(&shared->region_b[i]->level_mutex);
        init_pshared_cond(&shared->region_b[i]->not_full_b);
        init_pshared_cond(&shared->region_b[i]->not_empty_b);
    }

    shared->region_c = mmap(NULL, shared->region_c_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared->region_c == MAP_FAILED) {
        die_errno("mmap region_c");
    }
    memset(shared->region_c, 0, shared->region_c_size);
    init_pshared_mutex(&shared->region_c->result_mutex);
    init_pshared_cond(&shared->region_c->result_cond);
    for (i = 0; i < MAX_LEVELS; ++i) {

        if (hw_sem_init(&shared->region_c->level_ready[i], 1, 0) != 0) {
            die_errno("hw_sem_init");
        }
        strncpy(shared->region_c->results[i].level, level_name(i),
                sizeof(shared->region_c->results[i].level) - 1);
    }

    shared->region_d = mmap(NULL, shared->region_d_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared->region_d == MAP_FAILED) {
        die_errno("mmap region_d");
    }
    memset(shared->region_d, 0, shared->region_d_size);
    shared->region_d->capacity = opts->capacity_d;
    init_pshared_mutex(&shared->region_d->priority_mutex);
    init_pshared_cond(&shared->region_d->not_full_d);
    init_pshared_cond(&shared->region_d->not_empty_d);

    return 0;
}

/* Destroys synchronization objects and unmaps all shared memory regions. */
void destroy_shared_regions(shared_regions_t *shared) {
    int i;
    if (shared->region_a != NULL) {
        pthread_mutex_destroy(&shared->region_a->input_mutex);
        pthread_cond_destroy(&shared->region_a->not_full_a);
        pthread_cond_destroy(&shared->region_a->not_empty_a);
        munmap(shared->region_a, shared->region_a_size);
    }
    for (i = 0; i < MAX_LEVELS; ++i) {
        if (shared->region_b[i] != NULL) {
            pthread_mutex_destroy(&shared->region_b[i]->level_mutex);
            pthread_cond_destroy(&shared->region_b[i]->not_full_b);
            pthread_cond_destroy(&shared->region_b[i]->not_empty_b);
            munmap(shared->region_b[i], shared->region_b_size);
        }
    }
    if (shared->region_c != NULL) {
        for (i = 0; i < MAX_LEVELS; ++i) {
            hw_sem_destroy(&shared->region_c->level_ready[i]);
        }
        pthread_mutex_destroy(&shared->region_c->result_mutex);
        pthread_cond_destroy(&shared->region_c->result_cond);
        munmap(shared->region_c, shared->region_c_size);
    }
    if (shared->region_d != NULL) {
        pthread_mutex_destroy(&shared->region_d->priority_mutex);
        pthread_cond_destroy(&shared->region_d->not_full_d);
        pthread_cond_destroy(&shared->region_d->not_empty_d);
        munmap(shared->region_d, shared->region_d_size);
    }
}
