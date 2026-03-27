#define _POSIX_C_SOURCE 200809L
#include "pattern.h"
#include <ctype.h>
#include <string.h>

static char to_lower(char c) { return (char)tolower((unsigned char)c); }

static int match_rec(const char *pat_ptr, const char *str_ptr)
{
    if (*pat_ptr == '\0' && *str_ptr == '\0') { return 1; }
    if (*pat_ptr == '\0') { return 0; }

    if (*(pat_ptr + 1) == '+') {
        char repeat_char = to_lower(*pat_ptr);
        if (*str_ptr == '\0' || to_lower(*str_ptr) != repeat_char) { return 0; }

        const char *repeat_end = str_ptr;
        while (*repeat_end != '\0' && to_lower(*repeat_end) == repeat_char) {
            repeat_end++;
        }

        for (const char *backtrack_ptr = repeat_end; backtrack_ptr >= str_ptr + 1; backtrack_ptr--) {
            if (match_rec(pat_ptr + 2, backtrack_ptr)) { return 1; }
        }

        return 0;
    }

    if (*str_ptr == '\0') { return 0; }
    if (to_lower(*pat_ptr) != to_lower(*str_ptr)) { return 0; }
    return match_rec(pat_ptr + 1, str_ptr + 1);
}

int match_pattern(const char *pattern, const char *filename)
{
    if (!pattern || !filename) { return 0; }

    if (match_rec(pattern, filename)) { return 1; }

    const char *dot_ptr = filename;
    while (*dot_ptr != '\0' && *dot_ptr != '.') { dot_ptr++; }
    if (*dot_ptr == '.') {
        char stem[256];
        size_t stem_len = (size_t)(dot_ptr - filename);
        if (stem_len >= sizeof(stem)) { stem_len = sizeof(stem) - 1; }
        for (size_t i = 0; i < stem_len; i++) { stem[i] = filename[i]; }
        stem[stem_len] = '\0';
        if (match_rec(pattern, stem)) { return 1; }
    }

    return 0;
}
