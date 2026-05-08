#include "analyzer.h"

typedef struct {
    analyzer_process_args_t *proc_args;
    pthread_barrier_t *barrier;
    pthread_key_t *tls_key;
    pthread_mutex_t *local_mutex;
    long *source_hits;
    char (*source_names)[64];
    int *source_count;
    pid_t *all_tids;
    long *entries_per_worker;
    int worker_index;
} worker_arg_t;

typedef struct {
    analyzer_process_args_t *proc_args;
    pthread_mutex_t *local_mutex;
    long *source_hits;
    char (*source_names)[64];
    int *source_count;
    long *entries_per_worker;
    int worker_index;
} tls_payload_t;

/* Stores worker thread local scores into shared Region C before the thread exits. */
static void tls_destructor(void *ptr) {
    tls_payload_t *payload = ptr;
    int k;
    double *scores;
    region_c_t *region_c;
    level_result_t *result;

    if (payload == NULL) {
        return;
    }

    scores = (double *)(payload + 1);
    region_c = payload->proc_args->shared->region_c;
    result = &region_c->results[payload->proc_args->level_index];

    pthread_mutex_lock(&region_c->result_mutex);
    for (k = 0; k < payload->proc_args->opts->num_keywords; ++k) {
        result->per_keyword_score[k] += scores[k];
        result->total_weighted_score += scores[k];
    }
    result->per_thread_score[payload->worker_index] = 0.0;
    for (k = 0; k < payload->proc_args->opts->num_keywords; ++k) {
        result->per_thread_score[payload->worker_index] += scores[k];
    }
    result->total_entries += payload->entries_per_worker[payload->worker_index];
    pthread_mutex_unlock(&region_c->result_mutex);

    free(payload);
}

/* Reads the next entry from the analyzer level queue. */
static int pop_level_entry(region_b_level_t *region_b, log_entry_t *entry) {
    pthread_mutex_lock(&region_b->level_mutex);
    while (region_b->count == 0 && !region_b->eof_posted) {
        pthread_cond_wait(&region_b->not_empty_b, &region_b->level_mutex);
    }
    if (region_b->count == 0 && region_b->eof_posted) {
        pthread_mutex_unlock(&region_b->level_mutex);
        return 0;
    }
    *entry = region_b->entries[region_b->head];
    region_b->head = (region_b->head + 1) % region_b->capacity;
    region_b->count--;
    pthread_cond_signal(&region_b->not_full_b);
    pthread_mutex_unlock(&region_b->level_mutex);
    return 1;
}

/* Updates the accumulated score for one source. */
static void update_source_hits(worker_arg_t *ctx, const char *source, long hits) {
    int i;
    pthread_mutex_lock(ctx->local_mutex);
    for (i = 0; i < *ctx->source_count; ++i) {
        if (strcmp(ctx->source_names[i], source) == 0) {
            ctx->source_hits[i] += hits;
            pthread_mutex_unlock(ctx->local_mutex);
            return;
        }
    }
    if (*ctx->source_count < MAX_SOURCES) {
        strncpy(ctx->source_names[*ctx->source_count], source, 63);
        ctx->source_names[*ctx->source_count][63] = '\0';
        ctx->source_hits[*ctx->source_count] = hits;
        (*ctx->source_count)++;
    }
    pthread_mutex_unlock(ctx->local_mutex);
}

/* Finds the top three scoring sources for the analyzer level. */
static void compute_top3(worker_arg_t *ctx) {
    int i, j;
    level_result_t *result = &ctx->proc_args->shared->region_c->results[ctx->proc_args->level_index];
    long best_hits[3] = {0, 0, 0};
    char best_names[3][64] = {{0}};

    for (i = 0; i < *ctx->source_count; ++i) {
        for (j = 0; j < 3; ++j) {
            if (ctx->source_hits[i] > best_hits[j]) {
                int move;

                for (move = 2; move > j; --move) {
                    best_hits[move] = best_hits[move - 1];
                    strncpy(best_names[move], best_names[move - 1], sizeof(best_names[move]) - 1);
                }
                best_hits[j] = ctx->source_hits[i];
                strncpy(best_names[j], ctx->source_names[i], sizeof(best_names[j]) - 1);
                break;
            }
        }
    }

    pthread_mutex_lock(&ctx->proc_args->shared->region_c->result_mutex);
    for (i = 0; i < 3; ++i) {
        strncpy(result->top_source[i], best_names[i], sizeof(result->top_source[i]) - 1);
        result->top_source_hits[i] = best_hits[i];
    }
    pthread_mutex_unlock(&ctx->proc_args->shared->region_c->result_mutex);
}

/* Processes Region B entries and computes keyword scores for one worker thread. */
static void *worker_main(void *arg) {
    worker_arg_t *ctx = arg;
    tls_payload_t *payload;
    double *scores;
    pid_t tid = get_system_tid();
    region_b_level_t *region_b = ctx->proc_args->shared->region_b[ctx->proc_args->level_index];
    int k;
    double thread_total = 0.0;

    ctx->all_tids[ctx->worker_index] = tid;
    printf("[PID:%d][TID:%d] Worker %d started.\n", getpid(), tid, ctx->worker_index);

    payload = calloc(1, sizeof(*payload) + (size_t)ctx->proc_args->opts->num_keywords * sizeof(double));
    if (payload == NULL) {
        die_errno("calloc tls_payload");
    }
    payload->proc_args = ctx->proc_args;
    payload->local_mutex = ctx->local_mutex;
    payload->source_hits = ctx->source_hits;
    payload->source_names = ctx->source_names;
    payload->source_count = ctx->source_count;
    payload->entries_per_worker = ctx->entries_per_worker;
    payload->worker_index = ctx->worker_index;
    scores = (double *)(payload + 1);
    pthread_setspecific(*ctx->tls_key, payload);

    while (1) {
        log_entry_t entry;
        double total_for_entry = 0.0;

        if (!pop_level_entry(region_b, &entry)) {
            break;
        }

        ctx->entries_per_worker[ctx->worker_index]++;
        for (k = 0; k < ctx->proc_args->opts->num_keywords; ++k) {

            long matches = count_overlapping_keyword(entry.message, ctx->proc_args->opts->keywords[k]);
            if (matches > 0) {
                double weighted = (double)matches * (double)level_weight(ctx->proc_args->level_index);
                scores[k] += weighted;
                total_for_entry += weighted;
                thread_total += weighted;
            }
        }
        if (total_for_entry > 0.0) {

            update_source_hits(ctx, entry.source, (long)total_for_entry);
        }
    }

    printf("[PID:%d][TID:%d] Worker %d done. Entries: %ld, Weighted score: %.1f\n",
           getpid(), tid, ctx->worker_index, ctx->entries_per_worker[ctx->worker_index], thread_total);

    pthread_barrier_wait(ctx->barrier);

    pthread_exit(NULL);
    return NULL;
}

/* Runs all worker threads for one log level and publishes the result. */
void run_analyzer_process(analyzer_process_args_t *args) {
    pthread_t workers[MAX_WORKERS];
    worker_arg_t worker_args[MAX_WORKERS];
    pthread_barrier_t barrier;
    pthread_key_t tls_key;
    pthread_mutex_t local_mutex;
    long source_hits[MAX_SOURCES] = {0};
    char source_names[MAX_SOURCES][64] = {{0}};
    int source_count = 0;
    pid_t tids[MAX_WORKERS] = {0};
    long entries_per_worker[MAX_WORKERS] = {0};
    int i;
    pid_t lowest_tid;
    int reporter_index = 0;
    level_result_t *result;

    printf("[PID:%d] Analyzer %s started. Workers: %d\n",
           getpid(), level_name(args->level_index), args->opts->worker_threads);

    pthread_barrier_init(&barrier, NULL, (unsigned)args->opts->worker_threads);
    pthread_key_create(&tls_key, tls_destructor);
    pthread_mutex_init(&local_mutex, NULL);

    for (i = 0; i < args->opts->worker_threads; ++i) {

        worker_args[i].proc_args = args;
        worker_args[i].barrier = &barrier;
        worker_args[i].tls_key = &tls_key;
        worker_args[i].local_mutex = &local_mutex;
        worker_args[i].source_hits = source_hits;
        worker_args[i].source_names = source_names;
        worker_args[i].source_count = &source_count;
        worker_args[i].all_tids = tids;
        worker_args[i].entries_per_worker = entries_per_worker;
        worker_args[i].worker_index = i;
        pthread_create(&workers[i], NULL, worker_main, &worker_args[i]);
    }

    for (i = 0; i < args->opts->worker_threads; ++i) {
        pthread_join(workers[i], NULL);
    }

    lowest_tid = tids[0];
    for (i = 1; i < args->opts->worker_threads; ++i) {

        if (tids[i] < lowest_tid) {
            lowest_tid = tids[i];
            reporter_index = i;
        }
    }

    compute_top3(&worker_args[reporter_index]);
    result = &args->shared->region_c->results[args->level_index];

    pthread_mutex_lock(&args->shared->region_c->result_mutex);

    result->ready = 1;
    pthread_cond_broadcast(&args->shared->region_c->result_cond);
    pthread_mutex_unlock(&args->shared->region_c->result_mutex);
    hw_sem_post(&args->shared->region_c->level_ready[args->level_index]);

    printf("[PID:%d][TID:%d] ** Reporting thread (lowest TID). Level: %s **\n",
           getpid(), lowest_tid, level_name(args->level_index));
    printf("[PID:%d][TID:%d] Total entries: %ld | Total weighted score: %.1f\n",
           getpid(), lowest_tid, result->total_entries, result->total_weighted_score);
    printf("[PID:%d] Analyzer %s exiting.\n", getpid(), level_name(args->level_index));

    pthread_key_delete(tls_key);
    pthread_barrier_destroy(&barrier);
    pthread_mutex_destroy(&local_mutex);
}
