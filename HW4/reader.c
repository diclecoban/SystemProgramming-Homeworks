#define _POSIX_C_SOURCE 200809L

#include "reader.h"
#include <sys/stat.h>

typedef struct {
    reader_process_args_t *proc_args;
    private_buffer_t *buffer;
    off_t start_offset;
    off_t end_offset;
    int is_last_thread;
    int thread_index;
    long lines_read;
    long malformed;
} reader_thread_arg_t;

typedef struct {
    reader_process_args_t *proc_args;
    private_buffer_t *buffer;
    int dispatched[MAX_LEVELS];
} parser_thread_arg_t;

static void private_buffer_init(private_buffer_t *buffer, int capacity) {
    memset(buffer, 0, sizeof(*buffer));
    buffer->capacity = capacity;
    buffer->entries = calloc((size_t)capacity, sizeof(log_entry_t));
    if (buffer->entries == NULL) {
        die_errno("calloc private buffer");
    }
    pthread_mutex_init(&buffer->mutex, NULL);
    pthread_cond_init(&buffer->not_full, NULL);
    pthread_cond_init(&buffer->not_empty, NULL);
}

static void private_buffer_destroy(private_buffer_t *buffer) {
    pthread_mutex_destroy(&buffer->mutex);
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    free(buffer->entries);
}

static void private_buffer_push(private_buffer_t *buffer, const log_entry_t *entry) {
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == buffer->capacity) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
    buffer->entries[buffer->tail] = *entry;
    buffer->tail = (buffer->tail + 1) % buffer->capacity;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
}

static int private_buffer_pop(private_buffer_t *buffer, log_entry_t *entry) {
    int should_continue = 1;
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->producers_done) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    if (buffer->count == 0 && buffer->producers_done) {
        should_continue = 0;
    } else {
        *entry = buffer->entries[buffer->head];
        buffer->head = (buffer->head + 1) % buffer->capacity;
        buffer->count--;
        pthread_cond_signal(&buffer->not_full);
    }
    pthread_mutex_unlock(&buffer->mutex);
    return should_continue;
}

static void send_heartbeat(int fd, int reader_id, long lines) {
    char buf[128];
    int len;
    if (fd < 0 || lines == 0 || lines % 50 != 0) {
        return;
    }
    len = snprintf(buf, sizeof(buf), "[R%d] %ld lines processed\n", reader_id, lines);
    if (len > 0) {
        (void)write(fd, buf, (size_t)len);
    }
}

static void *reader_thread_main(void *arg) {
    reader_thread_arg_t *ctx = arg;
    FILE *fp;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    printf("[PID:%d][TID:%d] Reader thread %d: range [%lld, %lld) bytes\n",
           getpid(), get_system_tid(), ctx->thread_index,
           (long long)ctx->start_offset, (long long)ctx->end_offset);

    fp = fopen(ctx->proc_args->file_path, "r");
    if (fp == NULL) {
        die_errno("reader thread fopen");
    }

    if (fseeko(fp, ctx->start_offset, SEEK_SET) != 0) {
        die_errno("reader thread fseeko");
    }

    if (ctx->start_offset > 0) {
        int ch;
        if (fseeko(fp, ctx->start_offset - 1, SEEK_SET) != 0) {
            die_errno("reader thread boundary fseeko");
        }
        ch = fgetc(fp);
        if (ch != '\n') {
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
                ;
            }
        }
    }

    while (1) {
        off_t line_start = ftello(fp);
        log_entry_t entry;

        if (line_start < 0) {
            die_errno("reader thread ftello");
        }
        if (!ctx->is_last_thread && line_start >= ctx->end_offset) {
            break;
        }

        len = getline(&line, &cap, fp);
        if (len == -1) {
            break;
        }

        memset(&entry, 0, sizeof(entry));
        if (parse_log_line(line, &entry)) {
            entry.reader_id = ctx->proc_args->reader_id;
            private_buffer_push(ctx->buffer, &entry);
        } else {
            ctx->malformed++;
        }
        ctx->lines_read++;
        send_heartbeat(ctx->proc_args->heartbeat_fd, ctx->proc_args->reader_id, ctx->lines_read);
    }

    free(line);
    fclose(fp);

    printf("[PID:%d][TID:%d] Reader thread %d: finished, lines_read=%ld, malformed=%ld\n",
           getpid(), get_system_tid(), ctx->thread_index, ctx->lines_read, ctx->malformed);
    return NULL;
}

static void push_region_a(region_a_t *region_a, const log_entry_t *entry) {
    pthread_mutex_lock(&region_a->input_mutex);
    while (region_a->count == region_a->capacity) {
        pthread_cond_wait(&region_a->not_full_a, &region_a->input_mutex);
    }
    region_a->entries[region_a->tail] = *entry;
    region_a->tail = (region_a->tail + 1) % region_a->capacity;
    region_a->count++;
    if (entry->is_eof) {
        region_a->eof_count_per_level[entry->level_index]++;
    }
    pthread_cond_signal(&region_a->not_empty_a);
    pthread_mutex_unlock(&region_a->input_mutex);
}

static void *parser_thread_main(void *arg) {
    parser_thread_arg_t *ctx = arg;
    log_entry_t entry;
    int level;

    while (private_buffer_pop(ctx->buffer, &entry)) {
        push_region_a(ctx->proc_args->shared->region_a, &entry);
        ctx->dispatched[entry.level_index]++;
    }

    for (level = 0; level < MAX_LEVELS; ++level) {
        memset(&entry, 0, sizeof(entry));
        entry.is_eof = 1;
        entry.level_index = level;
        strncpy(entry.level, level_name(level), sizeof(entry.level) - 1);
        entry.reader_id = ctx->proc_args->reader_id;
        push_region_a(ctx->proc_args->shared->region_a, &entry);
    }

    printf("[PID:%d] Parser thread: dispatched E:%d W:%d I:%d D:%d -> Region A\n",
           getpid(), ctx->dispatched[LEVEL_ERROR], ctx->dispatched[LEVEL_WARN],
           ctx->dispatched[LEVEL_INFO], ctx->dispatched[LEVEL_DEBUG]);
    return NULL;
}

void run_reader_process(reader_process_args_t *args) {
    struct stat st;
    off_t file_size;
    off_t chunk;
    int i;
    pthread_t *threads = NULL;
    reader_thread_arg_t *thread_args = NULL;
    pthread_t parser_thread;
    parser_thread_arg_t parser_arg;
    private_buffer_t buffer;

    if (stat(args->file_path, &st) != 0) {
        die_errno("reader stat file");
    }
    file_size = st.st_size;

    printf("[PID:%d] Reader %d started. File: %s, Threads: %d\n",
           getpid(), args->reader_id, args->file_path, args->opts->reader_threads);

    private_buffer_init(&buffer, args->opts->capacity_b);
    threads = calloc((size_t)args->opts->reader_threads, sizeof(pthread_t));
    thread_args = calloc((size_t)args->opts->reader_threads, sizeof(reader_thread_arg_t));
    if (threads == NULL || thread_args == NULL) {
        die_errno("calloc reader threads");
    }

    parser_arg.proc_args = args;
    parser_arg.buffer = &buffer;
    memset(parser_arg.dispatched, 0, sizeof(parser_arg.dispatched));
    pthread_create(&parser_thread, NULL, parser_thread_main, &parser_arg);

    chunk = (file_size + args->opts->reader_threads - 1) / args->opts->reader_threads;
    for (i = 0; i < args->opts->reader_threads; ++i) {
        thread_args[i].proc_args = args;
        thread_args[i].buffer = &buffer;
        thread_args[i].start_offset = i * chunk;
        thread_args[i].end_offset = (i + 1) * chunk;
        if (thread_args[i].end_offset > file_size) {
            thread_args[i].end_offset = file_size;
        }
        thread_args[i].is_last_thread = (i == args->opts->reader_threads - 1);
        thread_args[i].thread_index = i;
        pthread_create(&threads[i], NULL, reader_thread_main, &thread_args[i]);
    }

    for (i = 0; i < args->opts->reader_threads; ++i) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_lock(&buffer.mutex);
    buffer.producers_done = 1;
    pthread_cond_broadcast(&buffer.not_empty);
    pthread_mutex_unlock(&buffer.mutex);

    pthread_join(parser_thread, NULL);

    close(args->heartbeat_fd);
    private_buffer_destroy(&buffer);
    free(threads);
    free(thread_args);

    printf("[PID:%d] Reader %d exiting.\n", getpid(), args->reader_id);
}
