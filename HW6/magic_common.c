#include "magic_common.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }

    return 0;
}

int send_line(int fd, const char *line)
{
    char out[MAX_LINE + 2];
    size_t len = strlen(line);

    if (len > MAX_LINE) {
        len = MAX_LINE;
    }
    memcpy(out, line, len);
    out[len++] = '\n';

    return send_all(fd, out, len);
}

int parse_positive_int(const char *text, int *value)
{
    char *end = NULL;
    long v;

    if (text == NULL || *text == '\0') {
        return 0;
    }
    errno = 0;
    v = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || v <= 0 || v > INT_MAX) {
        return 0;
    }
    *value = (int)v;
    return 1;
}

int valid_username(const char *s)
{
    size_t len;

    if (s == NULL) {
        return 0;
    }
    len = strlen(s);
    if (len == 0 || len >= 32) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') {
            return 0;
        }
    }
    return 1;
}

int valid_ingredient_name(const char *s)
{
    size_t len;

    if (s == NULL) {
        return 0;
    }
    len = strlen(s);
    if (len == 0 || len > 16) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!isupper((unsigned char)s[i]) && !isdigit((unsigned char)s[i]) && s[i] != '_') {
            return 0;
        }
    }
    return 1;
}
