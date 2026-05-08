#include "common.h"
#include <ctype.h>

volatile sig_atomic_t g_sigint_received = 0;

/* Returns the text name of a log level. */
const char *level_name(int level) {
    switch (level) {
        case LEVEL_ERROR:
            return "ERROR";
        case LEVEL_WARN:
            return "WARN";
        case LEVEL_INFO:
            return "INFO";
        case LEVEL_DEBUG:
            return "DEBUG";
        default:
            return "INVALID";
    }
}

/* Returns the scoring weight for a log level. */
int level_weight(int level) {
    switch (level) {
        case LEVEL_ERROR:
            return 4;
        case LEVEL_WARN:
            return 3;
        case LEVEL_INFO:
            return 2;
        case LEVEL_DEBUG:
            return 1;
        default:
            return 0;
    }
}

/* Converts a level string into the internal level index. */
int parse_level(const char *level_str) {
    if (strcmp(level_str, "ERROR") == 0) {
        return LEVEL_ERROR;
    }
    if (strcmp(level_str, "WARN") == 0) {
        return LEVEL_WARN;
    }
    if (strcmp(level_str, "INFO") == 0) {
        return LEVEL_INFO;
    }
    if (strcmp(level_str, "DEBUG") == 0) {
        return LEVEL_DEBUG;
    }
    return LEVEL_INVALID;
}

/* Removes trailing newline characters from a string. */
void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        len--;
    }
}

/* Parses one log line into a log_entry_t structure. */
int parse_log_line(const char *line, log_entry_t *entry) {
    const char *p = line;
    const char *end;
    size_t len;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return 0;
    }

    if (*p != '[') {
        return 0;
    }

    end = strchr(p + 1, ']');
    if (end == NULL || (size_t)(end - (p + 1)) != sizeof(entry->timestamp) - 1) {
        return 0;
    }

    memcpy(entry->timestamp, p + 1, sizeof(entry->timestamp) - 1);
    entry->timestamp[sizeof(entry->timestamp) - 1] = '\0';
    p = end + 1;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '[') {
        return 0;
    }

    end = strchr(p + 1, ']');
    if (end == NULL) {
        return 0;
    }
    len = (size_t)(end - (p + 1));
    if (len == 0 || len >= sizeof(entry->level)) {
        return 0;
    }
    memcpy(entry->level, p + 1, len);
    entry->level[len] = '\0';
    entry->level_index = parse_level(entry->level);
    if (entry->level_index == LEVEL_INVALID) {
        return 0;
    }
    p = end + 1;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '[') {
        return 0;
    }

    end = strchr(p + 1, ']');
    if (end == NULL) {
        return 0;
    }
    len = (size_t)(end - (p + 1));
    if (len == 0 || len >= sizeof(entry->source)) {
        return 0;
    }
    memcpy(entry->source, p + 1, len);
    entry->source[len] = '\0';
    for (len = 0; entry->source[len] != '\0'; ++len) {
        if (!isalnum((unsigned char)entry->source[len])) {
            return 0;
        }
    }
    p = end + 1;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    strncpy(entry->message, p, sizeof(entry->message) - 1);
    entry->message[sizeof(entry->message) - 1] = '\0';
    trim_newline(entry->message);
    entry->is_eof = 0;
    return 1;
}

/* Counts all keyword matches in a message, including overlapping matches. */
long count_overlapping_keyword(const char *haystack, const char *needle) {
    size_t nlen;
    size_t hlen;
    size_t i;
    long count = 0;

    if (needle == NULL || *needle == '\0') {
        return 0;
    }

    nlen = strlen(needle);
    hlen = strlen(haystack);
    if (nlen > hlen) {
        return 0;
    }

    for (i = 0; i + nlen <= hlen; ++i) {
        if (strncmp(haystack + i, needle, nlen) == 0) {
            count++;
        }
    }
    return count;
}

/* Prints a system error message and exits the program. */
void die_errno(const char *msg) {
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

/* Prints a custom error message and exits the program. */
void die_message(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

/* Builds an absolute timeout value for timed condition waits. */
int timed_wait_seconds(struct timespec *ts, int timeout_sec) {
    if (clock_gettime(CLOCK_REALTIME, ts) != 0) {
        return -1;
    }
    ts->tv_sec += timeout_sec;
    return 0;
}
