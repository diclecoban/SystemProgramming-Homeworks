#ifndef MAGIC_COMMON_H
#define MAGIC_COMMON_H

#include <stddef.h>

#define MAX_LINE 512

int send_all(int fd, const char *buf, size_t len);
int send_line(int fd, const char *line);
int parse_positive_int(const char *text, int *value);
int valid_username(const char *s);
int valid_ingredient_name(const char *s);

#endif
