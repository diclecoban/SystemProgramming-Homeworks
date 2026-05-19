#define _POSIX_C_SOURCE 200809L

#include "magic_common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_INGREDIENTS 128
#define BACKLOG 32

typedef struct {
    char name[17];
    int qty;
} ingredient_t;

typedef struct {
    int fd;
    char username[32];
    char type[16];
    char line_buf[MAX_LINE + 1];
    size_t line_len;
    int discard_long_line;
    time_t last_active;
    int *spellbook;
} client_t;

static volatile sig_atomic_t shutdown_requested = 0;

static ingredient_t ingredients[MAX_INGREDIENTS];
static int ingredient_count = 0;
static client_t *clients = NULL;
static int max_clients = 0;
static int timeout_seconds = 0;
static int listen_fd = -1;
static FILE *log_file = NULL;

static void on_sigint(int signo)
{
    (void)signo;
    shutdown_requested = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s -p <tcp_port> -s <ingredients.txt> -l <logfile> -n <max_clients> -t <timeout>\n", prog);
}

static void log_event(const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm tm_now;
    char ts[32];
    char message[1024];
    va_list ap;

    localtime_r(&now, &tm_now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    printf("[%s] [SERVER] %s\n", ts, message);
    fflush(stdout);
    if (log_file != NULL) {
        fprintf(log_file, "[%s] [SERVER] %s\n", ts, message);
        fflush(log_file);
    }
}

static int parse_args(int argc, char **argv, int *port, char **ingredient_path, char **log_path)
{
    int opt;
    int have_p = 0;
    int have_s = 0;
    int have_l = 0;
    int have_n = 0;
    int have_t = 0;

    while ((opt = getopt(argc, argv, "p:s:l:n:t:")) != -1) {
        switch (opt) {
        case 'p':
            have_p = parse_positive_int(optarg, port);
            break;
        case 's':
            *ingredient_path = optarg;
            have_s = 1;
            break;
        case 'l':
            *log_path = optarg;
            have_l = 1;
            break;
        case 'n':
            have_n = parse_positive_int(optarg, &max_clients);
            break;
        case 't':
            have_t = parse_positive_int(optarg, &timeout_seconds);
            break;
        default:
            return 0;
        }
    }

    if (!have_p || !have_s || !have_l || !have_n || !have_t || *port < 1024 || optind != argc) {
        return 0;
    }
    return 1;
}

static int load_ingredients(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[256];

    if (fp == NULL) {
        perror("ingredients");
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL && ingredient_count < MAX_INGREDIENTS) {
        char name[64];
        char qty_text[64];
        char extra[64];
        int qty;

        if (sscanf(line, "%63s %63s %63s", name, qty_text, extra) != 2) {
            continue;
        }
        if (!valid_ingredient_name(name) || !parse_positive_int(qty_text, &qty)) {
            continue;
        }
        strncpy(ingredients[ingredient_count].name, name, sizeof(ingredients[ingredient_count].name) - 1);
        ingredients[ingredient_count].qty = qty;
        ingredient_count++;
    }

    fclose(fp);
    return 0;
}

static int create_listener(int port)
{
    int fd;
    int yes = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

static int find_ingredient(const char *name)
{
    for (int i = 0; i < ingredient_count; ++i) {
        if (strcmp(ingredients[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int active_client_count(void)
{
    int count = 0;

    for (int i = 0; i < max_clients; ++i) {
        if (clients[i].fd >= 0) {
            count++;
        }
    }
    return count;
}

static int find_free_slot(void)
{
    for (int i = 0; i < max_clients; ++i) {
        if (clients[i].fd < 0) {
            return i;
        }
    }
    return -1;
}

static int username_exists(const char *username)
{
    for (int i = 0; i < max_clients; ++i) {
        if (clients[i].fd >= 0 && clients[i].username[0] != '\0' && strcmp(clients[i].username, username) == 0) {
            return 1;
        }
    }
    return 0;
}

static void reset_client(client_t *c)
{
    if (c->spellbook != NULL) {
        free(c->spellbook);
    }
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static void disconnect_client(int index, const char *reason)
{
    client_t *c = &clients[index];
    char shown_name[32];

    if (c->fd < 0) {
        return;
    }
    if (c->username[0] == '\0') {
        snprintf(shown_name, sizeof(shown_name), "unknown");
    } else {
        snprintf(shown_name, sizeof(shown_name), "%s", c->username);
    }

    close(c->fd);
    log_event("CLIENT_DISCONNECTED username=%s reason=%s", shown_name, reason);
    reset_client(c);
}

static void append_csv_ingredients(char *out, size_t out_sz, int only_nonzero, const int *values)
{
    size_t used = 0;
    int first = 1;

    out[0] = '\0';
    for (int i = 0; i < ingredient_count; ++i) {
        int qty = values == NULL ? ingredients[i].qty : values[i];
        int written;

        if (only_nonzero && qty <= 0) {
            continue;
        }
        written = snprintf(out + used, out_sz - used, "%s%s:%d", first ? "" : ",", ingredients[i].name, qty);
        if (written < 0 || (size_t)written >= out_sz - used) {
            break;
        }
        used += (size_t)written;
        first = 0;
    }
}

static void send_unknown(client_t *c, const char *command)
{
    char response[MAX_LINE + 1];

    snprintf(response, sizeof(response), "ERR UNKNOWN %s", command != NULL && command[0] != '\0' ? command : "");
    send_line(c->fd, response);
}

static void handle_enroll(client_t *c, char **tokens, int ntokens)
{
    char response[MAX_LINE + 1];

    if (ntokens != 3 || (strcmp(tokens[1], "WIZARD") != 0 && strcmp(tokens[1], "PROFESSOR") != 0) || !valid_username(tokens[2])) {
        send_line(c->fd, "ERR UNKNOWN ENROLL");
        return;
    }
    if (c->username[0] != '\0' || username_exists(tokens[2])) {
        send_line(c->fd, "ERR ENROLL name_taken");
        return;
    }

    snprintf(c->username, sizeof(c->username), "%s", tokens[2]);
    snprintf(c->type, sizeof(c->type), "%s", tokens[1]);
    snprintf(response, sizeof(response), "OK ENROLL %s", c->username);
    send_line(c->fd, response);
    log_event("ENROLL username=%s type=%s fd=%d", c->username, c->type, c->fd);
}

static void handle_brew(client_t *c, char **tokens, int ntokens)
{
    char response[MAX_LINE + 1];
    int idx;
    int qty;
    int old_qty;

    if (strcmp(c->type, "WIZARD") != 0) {
        send_line(c->fd, "ERR UNAUTHORIZED");
        return;
    }
    if (ntokens != 3 || !parse_positive_int(tokens[2], &qty)) {
        send_unknown(c, tokens[0]);
        return;
    }
    idx = find_ingredient(tokens[1]);
    if (idx < 0) {
        send_line(c->fd, "ERR UNKNOWN_INGREDIENT");
        return;
    }

    old_qty = ingredients[idx].qty;
    ingredients[idx].qty += qty;
    c->spellbook[idx] += qty;
    snprintf(response, sizeof(response), "OK BREW %s %d %d", ingredients[idx].name, qty, ingredients[idx].qty);
    send_line(c->fd, response);
    log_event("BREW wizard=%s ingredient=%s qty=%d old_qty=%d new_qty=%d", c->username, ingredients[idx].name, qty, old_qty, ingredients[idx].qty);
}

static void handle_consume(client_t *c, char **tokens, int ntokens)
{
    char response[MAX_LINE + 1];
    int idx;
    int qty;
    int old_qty;

    if (strcmp(c->type, "WIZARD") != 0) {
        send_line(c->fd, "ERR UNAUTHORIZED");
        return;
    }
    if (ntokens != 3 || !parse_positive_int(tokens[2], &qty)) {
        send_unknown(c, tokens[0]);
        return;
    }
    idx = find_ingredient(tokens[1]);
    if (idx < 0) {
        send_line(c->fd, "ERR UNKNOWN_INGREDIENT");
        return;
    }
    if (ingredients[idx].qty < qty || c->spellbook[idx] < qty) {
        send_line(c->fd, "ERR INSUFFICIENT_INGREDIENTS");
        return;
    }

    old_qty = ingredients[idx].qty;
    ingredients[idx].qty -= qty;
    c->spellbook[idx] -= qty;
    snprintf(response, sizeof(response), "OK CONSUME %s %d %d", ingredients[idx].name, qty, ingredients[idx].qty);
    send_line(c->fd, response);
    log_event("CONSUME wizard=%s ingredient=%s qty=%d old_qty=%d new_qty=%d", c->username, ingredients[idx].name, qty, old_qty, ingredients[idx].qty);
}

static void handle_spellbook(client_t *c, int ntokens)
{
    char csv[MAX_LINE + 1];
    char response[MAX_LINE + 1];

    if (strcmp(c->type, "WIZARD") != 0) {
        send_line(c->fd, "ERR UNAUTHORIZED");
        return;
    }
    if (ntokens != 1) {
        send_unknown(c, "SPELLBOOK");
        return;
    }
    append_csv_ingredients(csv, sizeof(csv), 1, c->spellbook);
    if (csv[0] == '\0') {
        send_line(c->fd, "OK SPELLBOOK EMPTY");
    } else {
        snprintf(response, sizeof(response), "OK SPELLBOOK %s", csv);
        send_line(c->fd, response);
    }
}

static void handle_inspect(client_t *c, char **tokens, int ntokens)
{
    char response[MAX_LINE + 1];
    int idx;

    if (strcmp(c->type, "PROFESSOR") != 0) {
        send_line(c->fd, "ERR UNAUTHORIZED");
        return;
    }
    if (ntokens != 2) {
        send_unknown(c, tokens[0]);
        return;
    }
    idx = find_ingredient(tokens[1]);
    if (idx < 0) {
        send_line(c->fd, "ERR UNKNOWN_INGREDIENT");
        return;
    }
    snprintf(response, sizeof(response), "OK INSPECT %s %d", ingredients[idx].name, ingredients[idx].qty);
    send_line(c->fd, response);
    log_event("INSPECT professor=%s ingredient=%s qty=%d", c->username, ingredients[idx].name, ingredients[idx].qty);
}

static void handle_scroll(client_t *c, int ntokens)
{
    char csv[MAX_LINE + 1];
    char response[MAX_LINE + 1];

    if (strcmp(c->type, "PROFESSOR") != 0) {
        send_line(c->fd, "ERR UNAUTHORIZED");
        return;
    }
    if (ntokens != 1) {
        send_unknown(c, "SCROLL");
        return;
    }
    append_csv_ingredients(csv, sizeof(csv), 0, NULL);
    snprintf(response, sizeof(response), "OK SCROLL %s", csv);
    send_line(c->fd, response);
    log_event("SCROLL professor=%s ingredients=%d", c->username, ingredient_count);
}

static void handle_roster(client_t *c, int ntokens)
{
    char roster[MAX_LINE + 1];
    char response[MAX_LINE + 1];
    size_t used = 0;
    int first = 1;
    int count = 0;

    if (strcmp(c->type, "PROFESSOR") != 0) {
        send_line(c->fd, "ERR UNAUTHORIZED");
        return;
    }
    if (ntokens != 1) {
        send_unknown(c, "ROSTER");
        return;
    }

    roster[0] = '\0';
    for (int i = 0; i < max_clients; ++i) {
        if (clients[i].fd >= 0 && clients[i].username[0] != '\0') {
            int written = snprintf(roster + used, sizeof(roster) - used, "%s%s", first ? "" : ",", clients[i].username);
            if (written < 0 || (size_t)written >= sizeof(roster) - used) {
                break;
            }
            used += (size_t)written;
            first = 0;
            count++;
        }
    }
    snprintf(response, sizeof(response), "OK ROSTER %s", roster);
    send_line(c->fd, response);
    log_event("ROSTER professor=%s clients=%d", c->username, count);
}

static int process_command(int index, char *line)
{
    client_t *c = &clients[index];
    char *tokens[8];
    int ntokens = 0;
    char *save = NULL;
    char *tok;

    c->last_active = time(NULL);
    tok = strtok_r(line, " \t\r\n", &save);
    while (tok != NULL && ntokens < 8) {
        tokens[ntokens++] = tok;
        tok = strtok_r(NULL, " \t\r\n", &save);
    }
    if (ntokens == 0) {
        return 0;
    }

    if (strcmp(tokens[0], "ENROLL") == 0) {
        handle_enroll(c, tokens, ntokens);
        return 0;
    }

    if (c->username[0] == '\0') {
        send_line(c->fd, "ERR NOT_ENROLLED");
        return 0;
    }

    if (strcmp(tokens[0], "APPARATE") == 0) {
        if (ntokens != 1) {
            send_unknown(c, tokens[0]);
            return 0;
        }
        send_line(c->fd, "OK APPARATE");
        disconnect_client(index, "APPARATE");
        return 1;
    }
    if (strcmp(tokens[0], "BREW") == 0) {
        handle_brew(c, tokens, ntokens);
    } else if (strcmp(tokens[0], "CONSUME") == 0) {
        handle_consume(c, tokens, ntokens);
    } else if (strcmp(tokens[0], "SPELLBOOK") == 0) {
        handle_spellbook(c, ntokens);
    } else if (strcmp(tokens[0], "INSPECT") == 0) {
        handle_inspect(c, tokens, ntokens);
    } else if (strcmp(tokens[0], "SCROLL") == 0) {
        handle_scroll(c, ntokens);
    } else if (strcmp(tokens[0], "ROSTER") == 0) {
        handle_roster(c, ntokens);
    } else {
        send_unknown(c, tokens[0]);
    }

    return 0;
}

static int feed_client_data(int index, const char *buf, ssize_t nread)
{
    client_t *c = &clients[index];

    for (ssize_t i = 0; i < nread; ++i) {
        char ch = buf[i];

        if (c->discard_long_line) {
            if (ch == '\n') {
                c->discard_long_line = 0;
                c->line_len = 0;
            }
            continue;
        }

        if (ch == '\n') {
            char line[MAX_LINE + 1];
            memcpy(line, c->line_buf, c->line_len);
            line[c->line_len] = '\0';
            c->line_len = 0;
            if (process_command(index, line)) {
                return 1;
            }
        } else {
            if (c->line_len >= MAX_LINE) {
                send_line(c->fd, "ERR TOOLONG");
                c->discard_long_line = 1;
                c->line_len = 0;
            } else {
                c->line_buf[c->line_len++] = ch;
            }
        }
    }

    return 0;
}

static void accept_new_client(void)
{
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    char ip[INET_ADDRSTRLEN];
    int fd;
    int slot;

    fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (fd < 0) {
        if (errno != EINTR) {
            perror("accept");
        }
        return;
    }
    if (inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip)) == NULL) {
        snprintf(ip, sizeof(ip), "unknown");
    }

    slot = find_free_slot();
    if (slot < 0) {
        send_line(fd, "ERR HOGWARTS_FULL");
        log_event("REJECTED fd=%d ip=%s reason=HOGWARTS_FULL", fd, ip);
        close(fd);
        return;
    }

    clients[slot].fd = fd;
    clients[slot].last_active = time(NULL);
    clients[slot].spellbook = calloc((size_t)ingredient_count, sizeof(int));
    if (clients[slot].spellbook == NULL) {
        send_line(fd, "ERR SERVER");
        close(fd);
        reset_client(&clients[slot]);
        return;
    }
    log_event("CLIENT_CONNECTED fd=%d ip=%s", fd, ip);
}

static void disconnect_expired_clients(time_t now)
{
    for (int i = 0; i < max_clients; ++i) {
        if (clients[i].fd >= 0) {
            time_t elapsed = now - clients[i].last_active;
            if (elapsed >= timeout_seconds) {
                char name[32];
                snprintf(name, sizeof(name), "%s", clients[i].username[0] == '\0' ? "unknown" : clients[i].username);
                send_line(clients[i].fd, "TIMEOUT DISCONNECT");
                log_event("TIMEOUT username=%s fd=%d elapsed=%lds", name, clients[i].fd, (long)elapsed);
                disconnect_client(i, "timeout");
            }
        }
    }
}

static void compute_select_timeout(struct timeval *tv, time_t now)
{
    long min_remaining = timeout_seconds;
    int have_clients = 0;

    for (int i = 0; i < max_clients; ++i) {
        if (clients[i].fd >= 0) {
            long elapsed = (long)(now - clients[i].last_active);
            long remaining = timeout_seconds - elapsed;
            if (remaining < 0) {
                remaining = 0;
            }
            if (!have_clients || remaining < min_remaining) {
                min_remaining = remaining;
            }
            have_clients = 1;
        }
    }

    tv->tv_sec = have_clients ? min_remaining : timeout_seconds;
    tv->tv_usec = 0;
}

static void shutdown_all_clients(void)
{
    for (int i = 0; i < max_clients; ++i) {
        if (clients[i].fd >= 0) {
            send_line(clients[i].fd, "SERVER SHUTDOWN");
            close(clients[i].fd);
            reset_client(&clients[i]);
        }
    }
}

int main(int argc, char **argv)
{
    int port = 0;
    char *ingredient_path = NULL;
    char *log_path = NULL;
    struct sigaction sa;

    if (!parse_args(argc, argv, &port, &ingredient_path, &log_path)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (load_ingredients(ingredient_path) < 0) {
        return EXIT_FAILURE;
    }
    log_file = fopen(log_path, "a");
    if (log_file == NULL) {
        perror("logfile");
        return EXIT_FAILURE;
    }

    clients = calloc((size_t)max_clients, sizeof(client_t));
    if (clients == NULL) {
        perror("calloc");
        fclose(log_file);
        return EXIT_FAILURE;
    }
    for (int i = 0; i < max_clients; ++i) {
        clients[i].fd = -1;
    }

    listen_fd = create_listener(port);
    if (listen_fd < 0) {
        fclose(log_file);
        free(clients);
        return EXIT_FAILURE;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction");
        close(listen_fd);
        fclose(log_file);
        free(clients);
        return EXIT_FAILURE;
    }

    log_event("SERVER_STARTED port=%d max_clients=%d timeout=%d ingredients=%d", port, max_clients, timeout_seconds, ingredient_count);
    printf("Hogwarts is ready. Port: %d | Max Clients: %d | Timeout: %ds\n", port, max_clients, timeout_seconds);
    fflush(stdout);

    while (!shutdown_requested) {
        fd_set readfds;
        int maxfd = listen_fd;
        struct timeval tv;
        int ready;
        time_t now = time(NULL);

        disconnect_expired_clients(now);
        compute_select_timeout(&tv, time(NULL));

        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        for (int i = 0; i < max_clients; ++i) {
            if (clients[i].fd >= 0) {
                FD_SET(clients[i].fd, &readfds);
                if (clients[i].fd > maxfd) {
                    maxfd = clients[i].fd;
                }
            }
        }

        ready = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }
        if (ready == 0) {
            continue;
        }
        if (FD_ISSET(listen_fd, &readfds)) {
            accept_new_client();
        }
        for (int i = 0; i < max_clients; ++i) {
            if (clients[i].fd >= 0 && FD_ISSET(clients[i].fd, &readfds)) {
                char buf[256];
                ssize_t nread = recv(clients[i].fd, buf, sizeof(buf), 0);
                if (nread < 0) {
                    if (errno != EINTR) {
                        disconnect_client(i, "hangup");
                    }
                } else if (nread == 0) {
                    disconnect_client(i, "hangup");
                } else {
                    (void)feed_client_data(i, buf, nread);
                }
            }
        }
    }

    log_event("SHUTDOWN signal=SIGINT");
    shutdown_all_clients();
    if (listen_fd >= 0) {
        close(listen_fd);
    }
    log_event("CLEANUP_DONE clients=%d", active_client_count());
    fclose(log_file);
    free(clients);

    return EXIT_SUCCESS;
}
