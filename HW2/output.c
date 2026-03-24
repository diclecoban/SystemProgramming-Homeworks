#define _POSIX_C_SOURCE 200809L

#include "output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tree printing                                                       */
/* ------------------------------------------------------------------ */

/*
 * relative_path:
 *   Returns a pointer into 'path' that skips 'root_dir/'.
 */
static const char *relative_path(const char *root_dir, const char *path)
{
    size_t rlen = strlen(root_dir);
    const char *p = path + rlen;
    if (*p == '/')
        p++;
    return p;
}

/*
 * We build a sorted list of unique directory paths that appear in the
 * matched entries, then walk through them to print the tree.
 *
 * Strategy:
 *   1. Collect all path prefixes (directories) that contain at least one
 *      match.
 *   2. Sort everything lexicographically.
 *   3. Walk entries; for each new directory component we haven't printed
 *      yet, print a directory line with appropriate indentation.
 *   4. Then print the file line indented one level deeper.
 *
 * Indentation rule from spec: each subdirectory level adds 6 dashes
 * relative to its parent.  Level 1 (direct child of root) → "|-- "
 * Level 2 → "|------ " etc.
 * More precisely: prefix is "|" + "------" × depth + " "
 */

#define MAX_DASH_LEVELS 32

static void print_indent(int depth)
{
    /* depth 1 → "|-- "  (2 dashes per level seems to be "|--" + "----" per sub)
     * The spec example shows:
     *   |-- alpha               (depth 1 from root → "|-- ")
     *   |------ report.txt      (depth 2 → "|------ ")
     *   |------ sub1            (depth 2 → "|------ ")
     *   |-------------- repoort (depth 3 → "|-------------- ")
     * So: 2 base dashes + 4 extra dashes per level after 1.
     * depth 1 → 2 dashes → "|-- "
     * depth 2 → 6 dashes → "|------ "
     * depth 3 → 14 dashes → ... wait, that's 8 more.
     * Re-reading: "Each subdirectory level is indented by 6 dashes (------) relative to its parent."
     * depth 1 → "|-- " (root children, 2 dashes for base prefix)
     * depth 2 → "|------ " (6 dashes)
     * depth 3 → "|-------------- " (6+6=12 dashes... but spec shows 14)
     *
     * Looking at spec output literally:
     *   |-- alpha               → 2 dashes
     *   |------ report.txt      → 6 dashes
     *   |------ sub1            → 6 dashes
     *   |-------------- repoort → 14 dashes
     *
     * 2 → 6 = +4; 6 → 14 = +8? That's inconsistent.
     * Most natural reading: base is 2, then each additional level adds 6.
     * depth 1: 2
     * depth 2: 2+4=6  (first sub adds 4)
     * depth 3: 6+8=14?
     *
     * Simpler: the spec says "6 dashes relative to its parent."
     * So depth 1 = 2 dashes (minimal prefix before filename at root level),
     * depth 2 = 2+4=6? OR depth 1 = 2, depth 2 = 2+6=8?
     *
     * Taking spec literally at face value:
     *   depth 1 → 2 dashes
     *   depth 2 → 6 dashes   (= 2 + 4)
     *   depth 3 → 14 dashes  (= 6 + 8)?
     *
     * Most consistent interpretation matching the example exactly:
     *   dashes = 2 + (depth-1)*4  → 2,6,10,14,...
     *   depth1=2, depth2=6, depth3=10, depth4=14 → matches "|-------------- " for depth3? No.
     *
     * Let me just count characters in the spec example:
     *   "|-- alpha"         → 2 dashes
     *   "|------ report.txt"→ 6 dashes
     *   "|------ sub1"      → 6 dashes  (sub1 is at same depth as report.txt: both direct children of alpha)
     *   "|-------------- repoort.txt" → 14 dashes
     *
     * depth(alpha)=1 → 2 dashes
     * depth(report.txt inside alpha)=2 → 6 dashes
     * depth(sub1 inside alpha)=2 → 6 dashes
     * depth(repoort.txt inside sub1)=3 → 14 dashes
     *
     * Pattern: 2, 6, 14 → differences: 4, 8 → each level doubles the increment?
     * OR: 2 = 2*1, 6 = 2*3, 14 = 2*7 → 1,3,7 → 2^n - 1 pattern.
     *
     * Most natural consistent reading that matches all three data points:
     *   dashes(depth) = 2 * (2^depth - 1)
     *   depth=1: 2*(2-1)=2  ✓
     *   depth=2: 2*(4-1)=6  ✓
     *   depth=3: 2*(8-1)=14 ✓
     *   depth=4: 2*(16-1)=30
     *
     * That fits perfectly.
     */
    int dashes;
    if (depth <= 0) depth = 1;
    /* dashes = 2*(2^depth - 1) */
    int power = 1;
    for (int i = 0; i < depth; i++) power *= 2;
    dashes = 2 * (power - 1);

    printf("|");
    for (int i = 0; i < dashes; i++)
        printf("-");
    printf(" ");
}

/*
 * Extract the component at depth 'level' (1-based) from a path
 * that is relative to root_dir.
 * Writes into buf (max buflen chars).
 * Returns 1 on success, 0 if level exceeds path depth.
 */
static int get_component(const char *rel_path, int level, char *buf, size_t buflen)
{
    const char *p = rel_path;
    int cur = 0;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len;
        if (slash)
            len = (size_t)(slash - p);
        else
            len = strlen(p);

        cur++;
        if (cur == level) {
            if (len >= buflen) len = buflen - 1;
            memcpy(buf, p, len);
            buf[len] = '\0';
            return 1;
        }

        if (!slash) break;
        p = slash + 1;
    }
    return 0;
}

/*
 * Returns number of '/' separators in rel_path + 1 (= total depth).
 */
static int path_depth(const char *rel_path)
{
    int d = 1;
    for (const char *p = rel_path; *p; p++)
        if (*p == '/') d++;
    return d;
}

/*
 * Comparison function for sorting MatchEntry by path.
 */
static int entry_cmp(const void *a, const void *b)
{
    const MatchEntry *ea = (const MatchEntry *)a;
    const MatchEntry *eb = (const MatchEntry *)b;
    return strcmp(ea->path, eb->path);
}

void print_tree(const char *root_dir,
                MatchEntry *entries,
                int         num_entries)
{
    printf("%s\n", root_dir);

    if (num_entries == 0) {
        printf("No matching files found.\n");
        return;
    }

    /* Sort entries by path for a consistent tree traversal */
    qsort(entries, (size_t)num_entries, sizeof(MatchEntry), entry_cmp);

    /*
     * We track which directory path segments we have already printed
     * to avoid printing a directory header more than once.
     */
    char printed[64][4096];
    int  num_printed = 0;

    for (int i = 0; i < num_entries; i++) {
        const char *rel = relative_path(root_dir, entries[i].path);
        int total_depth = path_depth(rel);

        /* Print every intermediate directory component not yet printed */
        for (int d = 1; d < total_depth; d++) {
            char component[256];
            if (!get_component(rel, d, component, sizeof(component)))
                break;

            /* Build the partial path up to this depth */
            char partial[4096];
            partial[0] = '\0';
            for (int k = 1; k <= d; k++) {
                char comp[256];
                if (!get_component(rel, k, comp, sizeof(comp))) break;
                if (k > 1) strncat(partial, "/", sizeof(partial) - strlen(partial) - 1);
                strncat(partial, comp, sizeof(partial) - strlen(partial) - 1);
            }

            /* Check if already printed */
            int already = 0;
            for (int j = 0; j < num_printed; j++) {
                if (strcmp(printed[j], partial) == 0) {
                    already = 1;
                    break;
                }
            }
            if (!already) {
                print_indent(d);
                printf("%s\n", component);
                if (num_printed < 64)
                    strncpy(printed[num_printed++], partial, 4095);
            }
        }

        /* Print the file itself */
        char filename[256];
        if (get_component(rel, total_depth, filename, sizeof(filename))) {
            print_indent(total_depth);
            printf("%s (%ld bytes) [Worker %d]\n",
                   filename,
                   entries[i].size,
                   (int)entries[i].worker_pid);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Summary                                                             */
/* ------------------------------------------------------------------ */

void print_summary(int           num_workers,
                   int           total_scanned,
                   int           total_matches,
                   WorkerResult *results,
                   int           num_results)
{
    printf("\n--- Summary ---\n");
    printf("Total workers used  : %d\n", num_workers);
    printf("Total files scanned : %d\n", total_scanned);
    printf("Total matches found : %d\n", total_matches);
    for (int i = 0; i < num_results; i++) {
        printf("Worker PID %d : %d %s\n",
               (int)results[i].worker_pid,
               results[i].match_count,
               results[i].match_count == 1 ? "match" : "matches");
    }
}
