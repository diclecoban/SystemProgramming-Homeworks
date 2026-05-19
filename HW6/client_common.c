#define _POSIX_C_SOURCE 200809L

#include "client_common.h"
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
#include <unistd.h>

static volatile sig_atomic_t exit_requested = 0;

static void on_sigint(int signo)
{
    (void)signo;
    exit_requested = 1;
}

static void usage(const char *prog, const char *role)
{
    fprintf(stderr, "Usage: %s <server_ip> <tcp_port> <username>\n", prog);
    fprintf(stderr, "Role: %s\n", role);
}

static void print_event(const char *role, const char *username, const char *fmt, ...)
{
    va_list ap;

    printf("[%s %s] ", role, username);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static int connect_to_server(const char *ip, int port)
{
    int fd;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP\n");
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

static int consume_socket_lines(const char *role, const char *username, char *buf, size_t *len, const char *data, ssize_t nread)
{
    for (ssize_t i = 0; i < nread; ++i) {
        char ch = data[i];
        if (ch == '\n') {
            buf[*len] = '\0';
            if (*len > 0 && buf[*len - 1] == '\r') {
                buf[*len - 1] = '\0';
            }
            print_event(role, username, "RECEIVED %s", buf);
            if (strcmp(buf, "TIMEOUT DISCONNECT") == 0) {
                print_event(role, username, "DISCONNECTED reason=timeout");
                return 1;
            }
            if (strcmp(buf, "SERVER SHUTDOWN") == 0) {
                print_event(role, username, "DISCONNECTED reason=shutdown");
                return 1;
            }
            if (strcmp(buf, "OK APPARATE") == 0) {
                print_event(role, username, "DISCONNECTED reason=APPARATE");
                return 1;
            }
            *len = 0;
        } else if (*len < MAX_LINE) {
            buf[(*len)++] = ch;
        } else {
            *len = 0;
        }
    }
    return 0;
}

static void send_apparate_and_wait(int fd, const char *role, const char *username)
{
    char line_buf[MAX_LINE + 1];
    size_t line_len = 0;
    struct timeval tv;
    fd_set readfds;

    if (send_line(fd, "APPARATE") == 0) {
        print_event(role, username, "SENT APPARATE");
    }

    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    if (select(fd + 1, &readfds, NULL, NULL, &tv) > 0 && FD_ISSET(fd, &readfds)) {
        char data[256];
        ssize_t nread = recv(fd, data, sizeof(data), 0);
        if (nread > 0) {
            if (consume_socket_lines(role, username, line_buf, &line_len, data, nread)) {
                return;
            }
        }
    }
    print_event(role, username, "DISCONNECTED reason=APPARATE");
}

int run_magic_client(const char *role, int argc, char **argv)
{
    const char *server_ip;
    const char *username;
    int port;
    int fd;
    char enroll[MAX_LINE + 1];
    char socket_line[MAX_LINE + 1];
    size_t socket_line_len = 0;
    int stdin_closed = 0;
    int apparate_sent = 0;
    struct sigaction sa;

    if (argc != 4 || !parse_positive_int(argv[2], &port) || port < 1024 || !valid_username(argv[3])) {
        usage(argv[0], role);
        return EXIT_FAILURE;
    }

    server_ip = argv[1];
    username = argv[3];

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    fd = connect_to_server(server_ip, port);
    if (fd < 0) {
        return EXIT_FAILURE;
    }

    print_event(role, username, "CONNECTED server=%s:%d", server_ip, port);
    snprintf(enroll, sizeof(enroll), "ENROLL %s %s", role, username);
    if (send_line(fd, enroll) < 0) {
        perror("send");
        close(fd);
        return EXIT_FAILURE;
    }
    print_event(role, username, "SENT %s", enroll);

    while (1) {
        fd_set readfds;
        int maxfd = fd > STDIN_FILENO ? fd : STDIN_FILENO;
        int ready;

        if (exit_requested) {
            if (!apparate_sent) {
                send_apparate_and_wait(fd, role, username);
            } else {
                print_event(role, username, "DISCONNECTED reason=APPARATE");
            }
            close(fd);
            return EXIT_SUCCESS;
        }

        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        if (!stdin_closed) {
            FD_SET(STDIN_FILENO, &readfds);
        }

        ready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            close(fd);
            return EXIT_FAILURE;
        }

        if (FD_ISSET(fd, &readfds)) {
            char data[256];
            ssize_t nread = recv(fd, data, sizeof(data), 0);
            if (nread < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("recv");
                close(fd);
                return EXIT_FAILURE;
            }
            if (nread == 0) {
                print_event(role, username, "DISCONNECTED reason=shutdown");
                close(fd);
                return EXIT_SUCCESS;
            }
            if (consume_socket_lines(role, username, socket_line, &socket_line_len, data, nread)) {
                close(fd);
                return EXIT_SUCCESS;
            }
        }

        if (!stdin_closed && FD_ISSET(STDIN_FILENO, &readfds)) {
            char input[MAX_LINE + 4];
            size_t len;

            if (fgets(input, sizeof(input), stdin) == NULL) {
                stdin_closed = 1;
                if (!apparate_sent) {
                    if (send_line(fd, "APPARATE") == 0) {
                        print_event(role, username, "SENT APPARATE");
                        apparate_sent = 1;
                    }
                }
                continue;
            }
            len = strlen(input);
            if (len > 0 && input[len - 1] == '\n') {
                input[len - 1] = '\0';
            }
            if (input[0] == '\0') {
                continue;
            }
            if (send_line(fd, input) < 0) {
                perror("send");
                close(fd);
                return EXIT_FAILURE;
            }
            print_event(role, username, "SENT %s", input);
            if (strcmp(input, "APPARATE") == 0) {
                apparate_sent = 1;
                stdin_closed = 1;
            }
        }
    }
}
