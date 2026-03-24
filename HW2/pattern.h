#ifndef PATTERN_H
#define PATTERN_H

/*
 * match_pattern:
 *   Returns 1 if 'filename' matches 'pattern' (case-insensitive).
 *   The '+' operator means "one or more of the immediately preceding character".
 *   No regex library is used; implemented from scratch.
 */
int match_pattern(const char *pattern, const char *filename);

#endif /* PATTERN_H */
