/*
 * pattern.c — Filename pattern matching with the '+' operator.
 *
 * Rule: 'X+' means one or more occurrences of the character X
 *       immediately to the left of '+' (standard regex left-side semantics).
 *       Matching is case-insensitive.
 *
 * The pattern is matched against:
 *   1. The full filename (e.g. "report.txt")
 *   2. The stem before the first '.' (e.g. "report")
 * This lets "rep+ort" match "report.txt".
 */

#define _POSIX_C_SOURCE 200809L
#include "pattern.h"
#include <ctype.h>
#include <string.h>

static char lc(char c) { return (char)tolower((unsigned char)c); }

/*
 * match_rec(p, s): try to match pattern p against string s from start to end.
 * Returns 1 on full match.
 */
static int match_rec(const char *p, const char *s)
{
    if (*p == '\0' && *s == '\0') return 1;
    if (*p == '\0') return 0;

    /* X+ : one or more of p[0] */
    if (*(p + 1) == '+') {
        char rep = lc(*p);
        if (*s == '\0' || lc(*s) != rep) return 0;

        /* Greedily consume all matching chars */
        const char *end = s;
        while (*end != '\0' && lc(*end) == rep)
            end++;

        /* Backtrack: try each valid consumed length (at least 1) */
        for (const char *t = end; t >= s + 1; t--)
            if (match_rec(p + 2, t)) return 1;

        return 0;
    }

    /* Normal character */
    if (*s == '\0') return 0;
    if (lc(*p) != lc(*s)) return 0;
    return match_rec(p + 1, s + 1);
}

int match_pattern(const char *pattern, const char *filename)
{
    if (!pattern || !filename) return 0;

    /* Try full filename */
    if (match_rec(pattern, filename)) return 1;

    /*
     * Also try against the stem (before first '.').
     * Allows "rep+ort" to match "report.txt", "repport.log", etc.
     */
    const char *dot = filename;
    while (*dot != '\0' && *dot != '.') dot++;
    if (*dot == '.') {
        char stem[256];
        size_t len = (size_t)(dot - filename);
        if (len >= sizeof(stem)) len = sizeof(stem) - 1;
        for (size_t i = 0; i < len; i++) stem[i] = filename[i];
        stem[len] = '\0';
        if (match_rec(pattern, stem)) return 1;
    }

    return 0;
}
