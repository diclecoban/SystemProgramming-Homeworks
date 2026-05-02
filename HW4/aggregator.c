#include "aggregator.h"

typedef struct {
    int level_index;
    double score;
} level_sort_item_t;

static int cmp_level_desc(const void *a, const void *b) {
    const level_sort_item_t *lhs = a;
    const level_sort_item_t *rhs = b;
    if (lhs->score < rhs->score) {
        return 1;
    }
    if (lhs->score > rhs->score) {
        return -1;
    }
    return lhs->level_index - rhs->level_index;
}

static void aggregate_high_priority(aggregator_process_args_t *args) {
    region_d_t *region_d = args->shared->region_d;
    while (1) {
        log_entry_t entry;
        int i;
        int done = 0;

        pthread_mutex_lock(&region_d->priority_mutex);
        while (region_d->count == 0 && !region_d->dispatcher_done) {
            struct timespec ts;
            timed_wait_seconds(&ts, args->opts->timeout_sec);
            if (pthread_cond_timedwait(&region_d->not_empty_d, &region_d->priority_mutex, &ts) == ETIMEDOUT) {
                break;
            }
        }
        if (region_d->count == 0 && region_d->dispatcher_done) {
            done = 1;
        } else if (region_d->count > 0) {
            entry = region_d->entries[region_d->head];
            region_d->head = (region_d->head + 1) % region_d->capacity;
            region_d->count--;
            pthread_cond_signal(&region_d->not_full_d);
        } else {
            pthread_mutex_unlock(&region_d->priority_mutex);
            continue;
        }
        pthread_mutex_unlock(&region_d->priority_mutex);

        if (done) {
            break;
        }
        if (entry.level_index < 0 || entry.level_index >= MAX_LEVELS) {
            continue;
        }
        for (i = 0; i < args->opts->num_keywords; ++i) {
            long hits = count_overlapping_keyword(entry.message, args->opts->keywords[i]);
            args->shared->region_c->high_priority_score +=
                (double)hits * (double)level_weight(entry.level_index);
        }
        args->shared->region_c->high_priority_entries++;
    }
}

static void write_text_output(aggregator_process_args_t *args) {
    FILE *fp;
    int i, k;
    double total_weighted = 0.0;
    level_sort_item_t items[MAX_LEVELS];

    fp = fopen(args->opts->output_file, "w");
    if (fp == NULL) {
        die_errno("fopen output file");
    }

    fprintf(fp, "KEYWORD_LIST: ");
    for (i = 0; i < args->opts->num_keywords; ++i) {
        fprintf(fp, "%s%s", args->opts->keywords[i], (i + 1 < args->opts->num_keywords) ? "," : "");
    }
    fprintf(fp, "\n");
    fprintf(fp, "FILES: %d\n", args->opts->num_files);
    for (i = 0; i < MAX_LEVELS; ++i) {
        total_weighted += args->shared->region_c->results[i].total_weighted_score;
        items[i].level_index = i;
        items[i].score = args->shared->region_c->results[i].total_weighted_score;
    }
    qsort(items, MAX_LEVELS, sizeof(items[0]), cmp_level_desc);

    fprintf(fp, "TOTAL_WEIGHTED_SCORE: %.1f\n", total_weighted);
    fprintf(fp, "HIGH_PRIORITY_SCORE: %.1f\n", args->shared->region_c->high_priority_score);
    fprintf(fp, "# Levels sorted by total_weighted_score DESC\n");
    fprintf(fp, "%-7s  %7s  %14s", "LEVEL", "ENTRIES", "WEIGHTED_SCORE");
    for (k = 0; k < args->opts->num_keywords; ++k) {
        fprintf(fp, "  %s", args->opts->keywords[k]);
    }
    fprintf(fp, "\n");

    for (i = 0; i < MAX_LEVELS; ++i) {
        level_result_t *r = &args->shared->region_c->results[items[i].level_index];
        fprintf(fp, "%-7s  %7ld  %14.1f", r->level, r->total_entries, r->total_weighted_score);
        for (k = 0; k < args->opts->num_keywords; ++k) {
            fprintf(fp, "  %.1f", r->per_keyword_score[k]);
        }
        fprintf(fp, "\n");
    }

    fprintf(fp, "# Top-3 sources per level\n");
    for (i = 0; i < MAX_LEVELS; ++i) {
        level_result_t *r = &args->shared->region_c->results[i];
        fprintf(fp, "  %s %s:%ld %s:%ld %s:%ld\n",
                r->level,
                r->top_source[0][0] ? r->top_source[0] : "-", r->top_source_hits[0],
                r->top_source[1][0] ? r->top_source[1] : "-", r->top_source_hits[1],
                r->top_source[2][0] ? r->top_source[2] : "-", r->top_source_hits[2]);
    }

    fprintf(fp, "# Per-thread contributions (weighted score)\n");
    for (i = 0; i < MAX_LEVELS; ++i) {
        level_result_t *r = &args->shared->region_c->results[i];
        fprintf(fp, "  %s", r->level);
        for (k = 0; k < args->opts->worker_threads; ++k) {
            fprintf(fp, " thread_%d:%.1f", k, r->per_thread_score[k]);
        }
        fprintf(fp, "\n");
    }

    fclose(fp);
}

static void write_binary_output(aggregator_process_args_t *args) {
    FILE *fp;
    char tmp_path[MAX_PATH_LEN + 8];
    checkpoint_header_t header;
    int i;
    double total_weighted = 0.0;

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", args->opts->binary_file);
    fp = fopen(tmp_path, "wb");
    if (fp == NULL) {
        die_errno("fopen binary tmp");
    }

    for (i = 0; i < MAX_LEVELS; ++i) {
        total_weighted += args->shared->region_c->results[i].total_weighted_score;
    }

    header.magic = MAGIC_CHECKPOINT;
    header.version = 1;
    header.num_levels = MAX_LEVELS;
    header.num_keywords = (uint32_t)args->opts->num_keywords;
    header.total_weighted = total_weighted;
    header.high_priority_weighted = args->shared->region_c->high_priority_score;

    if (fwrite(&header, sizeof(header), 1, fp) != 1) {
        die_message("Failed to write binary header");
    }
    for (i = 0; i < MAX_LEVELS; ++i) {
        if (fwrite(&args->shared->region_c->results[i], sizeof(level_result_t), 1, fp) != 1) {
            die_message("Failed to write binary result");
        }
    }
    fclose(fp);
    if (rename(tmp_path, args->opts->binary_file) != 0) {
        die_errno("rename binary checkpoint");
    }
}

void run_aggregator_process(aggregator_process_args_t *args) {
    int ready_levels = 0;
    int i;

    printf("[PID:%d] Aggregator started. Waiting for 4 levels...\n", getpid());
    aggregate_high_priority(args);

    for (i = 0; i < MAX_LEVELS; ++i) {
        struct timespec ts;
        pthread_mutex_lock(&args->shared->region_c->result_mutex);
        while (!args->shared->region_c->results[i].ready) {
            timed_wait_seconds(&ts, args->opts->timeout_sec);
            if (pthread_cond_timedwait(&args->shared->region_c->result_cond,
                                       &args->shared->region_c->result_mutex, &ts) == ETIMEDOUT) {
                continue;
            }
        }
        pthread_mutex_unlock(&args->shared->region_c->result_mutex);
        hw_sem_wait(&args->shared->region_c->level_ready[i]);
        printf("[PID:%d] %s result received.\n", getpid(), level_name(i));
        ready_levels++;
    }

    if (ready_levels == MAX_LEVELS) {
        printf("[PID:%d] All results received. Writing output files...\n", getpid());
        write_text_output(args);
        write_binary_output(args);
        printf("[PID:%d] Output files written: %s, %s\n",
               getpid(), args->opts->output_file, args->opts->binary_file);
    }

    printf("[PID:%d] Aggregator exiting.\n", getpid());
}
