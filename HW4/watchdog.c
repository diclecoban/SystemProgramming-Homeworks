#include "watchdog.h"

static int count_alive_children(const pid_t *child_pids, int child_count) {
    int i;
    int alive = 0;
    for (i = 0; i < child_count; ++i) {
        if (child_pids[i] > 0 && kill(child_pids[i], 0) == 0) {
            alive++;
        }
    }
    return alive;
}

void *watchdog_thread_main(void *arg) {
    watchdog_args_t *ctx = arg;
    int *progress = calloc((size_t)ctx->opts->num_files, sizeof(int));
    time_t start = time(NULL);
    time_t next_print = start + 3;

    if (progress == NULL) {
        return NULL;
    }

    while (!*ctx->shutdown_flag) {
        fd_set readfds;
        struct timeval tv;
        time_t now;
        int maxfd = -1;
        int i;
        int rc;

        FD_ZERO(&readfds);
        for (i = 0; i < ctx->opts->num_files; ++i) {
            if (ctx->pipe_fds[i] < 0) {
                continue;
            }
            FD_SET(ctx->pipe_fds[i], &readfds);
            if (ctx->pipe_fds[i] > maxfd) {
                maxfd = ctx->pipe_fds[i];
            }
        }

        now = time(NULL);
        tv.tv_sec = (next_print > now) ? (next_print - now) : 0;
        tv.tv_usec = 0;
        rc = select((maxfd >= 0) ? (maxfd + 1) : 0, &readfds, NULL, NULL, &tv);
        if (rc > 0) {
            for (i = 0; i < ctx->opts->num_files; ++i) {
                if (ctx->pipe_fds[i] < 0) {
                    continue;
                }
                if (FD_ISSET(ctx->pipe_fds[i], &readfds)) {
                    char buf[256];
                    ssize_t n = read(ctx->pipe_fds[i], buf, sizeof(buf) - 1);
                    if (n > 0) {
                        char *saveptr = NULL;
                        char *line;
                        buf[n] = '\0';
                        line = strtok_r(buf, "\n", &saveptr);
                        while (line != NULL) {
                            int reader_no = 0;
                            int lines = 0;
                            if (sscanf(line, "[R%d] %d lines processed", &reader_no, &lines) == 2 &&
                                reader_no >= 0 && reader_no < ctx->opts->num_files) {
                                progress[reader_no] = lines;
                            }
                            line = strtok_r(NULL, "\n", &saveptr);
                        }
                    } else if (n == 0) {
                        close(ctx->pipe_fds[i]);
                        ctx->pipe_fds[i] = -1;
                    }
                }
            }
        }

        now = time(NULL);
        if (now >= next_print) {
            fprintf(stderr, "[WATCHDOG] Progress at T+%lds:", (long)(now - start));
            for (i = 0; i < ctx->opts->num_files; ++i) {
                fprintf(stderr, " Reader %d=%d", i, progress[i]);
            }
            fprintf(stderr, " children_alive=%d\n", count_alive_children(ctx->child_pids, ctx->child_count));
            next_print = now + 3;
        }
    }

    free(progress);
    return NULL;
}
