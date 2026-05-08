#include "dispatcher.h"

/* Checks whether a source is in the high-priority filter list. */
static int source_is_priority(const program_options_t *opts, const char *source) {
    int i;
    for (i = 0; i < opts->num_priority_sources; ++i) {
        if (strcmp(opts->priority_sources[i], source) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Reads the next entry from Region A with timeout support. */
static int pop_region_a(region_a_t *region_a, log_entry_t *entry, int timeout_sec, int *timed_out) {
    struct timespec ts;
    int rc = 0;

    *timed_out = 0;
    pthread_mutex_lock(&region_a->input_mutex);
    while (region_a->count == 0) {

        if (timed_wait_seconds(&ts, timeout_sec) != 0) {
            pthread_mutex_unlock(&region_a->input_mutex);
            return -1;
        }
        rc = pthread_cond_timedwait(&region_a->not_empty_a, &region_a->input_mutex, &ts);
        if (rc == ETIMEDOUT) {
            *timed_out = 1;
            pthread_mutex_unlock(&region_a->input_mutex);
            return 0;
        }
    }
    *entry = region_a->entries[region_a->head];
    region_a->head = (region_a->head + 1) % region_a->capacity;
    region_a->count--;
    pthread_cond_signal(&region_a->not_full_a);
    pthread_mutex_unlock(&region_a->input_mutex);
    return 1;
}

/* Writes an entry into the Region B queue for its log level. */
static void push_region_b(region_b_level_t *region_b, const log_entry_t *entry) {
    pthread_mutex_lock(&region_b->level_mutex);
    while (region_b->count == region_b->capacity) {
        pthread_cond_wait(&region_b->not_full_b, &region_b->level_mutex);
    }
    region_b->entries[region_b->tail] = *entry;
    region_b->tail = (region_b->tail + 1) % region_b->capacity;
    region_b->count++;
    pthread_cond_signal(&region_b->not_empty_b);
    pthread_mutex_unlock(&region_b->level_mutex);
}

/* Writes a high-priority entry copy into Region D. */
static void push_region_d(region_d_t *region_d, const log_entry_t *entry) {
    pthread_mutex_lock(&region_d->priority_mutex);
    while (region_d->count == region_d->capacity) {
        pthread_cond_wait(&region_d->not_full_d, &region_d->priority_mutex);
    }
    region_d->entries[region_d->tail] = *entry;
    region_d->tail = (region_d->tail + 1) % region_d->capacity;
    region_d->count++;
    pthread_cond_signal(&region_d->not_empty_d);
    pthread_mutex_unlock(&region_d->priority_mutex);
}

/* Routes entries from Region A to Region B and Region D. */
void run_dispatcher_process(dispatcher_process_args_t *args) {
    int eof_seen[MAX_LEVELS] = {0};
    int finished_levels = 0;

    printf("[PID:%d] Dispatcher started.\n", getpid());

    while (finished_levels < MAX_LEVELS) {
        log_entry_t entry;
        int timed_out = 0;
        int pop_status = pop_region_a(args->shared->region_a, &entry, args->opts->timeout_sec, &timed_out);

        if (pop_status < 0) {
            die_message("Dispatcher timed wait setup failed");
        }
        if (timed_out) {
            int all_done = 1;
            int i;
            region_a_t *region_a = args->shared->region_a;
            pthread_mutex_lock(&region_a->input_mutex);
            for (i = 0; i < MAX_LEVELS; ++i) {
                if (region_a->eof_count_per_level[i] < region_a->total_readers) {
                    all_done = 0;
                    break;
                }
            }
            pthread_mutex_unlock(&region_a->input_mutex);
            if (all_done) {
                break;
            }
            continue;
        }

        if (entry.is_eof) {

            eof_seen[entry.level_index]++;
            if (eof_seen[entry.level_index] == args->opts->num_files &&
                !args->shared->region_b[entry.level_index]->eof_posted) {
                pthread_mutex_lock(&args->shared->region_b[entry.level_index]->level_mutex);
                args->shared->region_b[entry.level_index]->eof_posted = 1;
                pthread_cond_broadcast(&args->shared->region_b[entry.level_index]->not_empty_b);
                pthread_mutex_unlock(&args->shared->region_b[entry.level_index]->level_mutex);
                finished_levels++;
            }
            continue;
        }

        push_region_b(args->shared->region_b[entry.level_index], &entry);

        if (source_is_priority(args->opts, entry.source)) {

            push_region_d(args->shared->region_d, &entry);
            printf("[PID:%d] Routed entry to %s buffer. High-priority: YES (source: %s)\n",
                   getpid(), level_name(entry.level_index), entry.source);
        }
    }

    pthread_mutex_lock(&args->shared->region_d->priority_mutex);
    args->shared->region_d->dispatcher_done = 1;
    pthread_cond_broadcast(&args->shared->region_d->not_empty_d);
    pthread_mutex_unlock(&args->shared->region_d->priority_mutex);

    printf("[PID:%d] All EOF markers forwarded to Region B. Exiting.\n", getpid());
}
