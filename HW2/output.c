#define _POSIX_C_SOURCE 200809L

#include "output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const char *relative_path(const char *root_dir, const char *full_path)
{
    size_t root_len = strlen(root_dir);
    const char *rel_ptr = full_path + root_len;
    if (*rel_ptr == '/') {
        rel_ptr++;
    }
    return rel_ptr;
}

static void print_indent(int depth)
{
    int num_dashes;
    if (depth <= 0) {
        depth = 1;
    }
    int power = 1;
    for (int i = 0; i < depth; i++) {
        power *= 2;
    }
    num_dashes = 2 * (power - 1);

    printf("|");
    for (int i = 0; i < num_dashes; i++) {
        printf("-");
    }
    printf(" ");
}


static int get_component(const char *rel_path, int target_level, char *buf, size_t buflen)
{
    const char *scan_ptr = rel_path;
    int cur_level = 0;
    while (*scan_ptr) {
        const char *next_slash = strchr(scan_ptr, '/');
        size_t component_len;
        if (next_slash) {
            component_len = (size_t)(next_slash - scan_ptr);
        } else {
            component_len = strlen(scan_ptr);
        }

        cur_level++;
        if (cur_level == target_level) {
            if (component_len >= buflen) { component_len = buflen - 1; }
            memcpy(buf, scan_ptr, component_len);
            buf[component_len] = '\0';
            return 1;
        }

        if (!next_slash) { break; }
        scan_ptr = next_slash + 1;
    }
    return 0;
}


static int path_depth(const char *rel_path)
{
    int depth = 1;
    for (const char *scan_ptr = rel_path; *scan_ptr; scan_ptr++) {
        if (*scan_ptr == '/') { depth++; }
    }
    return depth;
}


static int entry_cmp(const void *a, const void *b)
{
    const MatchEntry *entry_a = (const MatchEntry *)a;
    const MatchEntry *entry_b = (const MatchEntry *)b;
    return strcmp(entry_a->path, entry_b->path);
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

    qsort(entries, (size_t)num_entries, sizeof(MatchEntry), entry_cmp);

    char printed_dirs[64][4096];
    int  num_printed_dirs = 0;

    for (int i = 0; i < num_entries; i++) {
        const char *rel_path = relative_path(root_dir, entries[i].path);
        int total_depth = path_depth(rel_path);

        for (int depth = 1; depth < total_depth; depth++) {
            char dir_name[256];
            if (!get_component(rel_path, depth, dir_name, sizeof(dir_name))) {
                break;
            }

            char partial_path[4096];
            partial_path[0] = '\0';
            for (int level = 1; level <= depth; level++) {
                char part_name[256];
                if (!get_component(rel_path, level, part_name, sizeof(part_name))) { break; }
                if (level > 1) strncat(partial_path, "/", sizeof(partial_path) - strlen(partial_path) - 1);
                strncat(partial_path, part_name, sizeof(partial_path) - strlen(partial_path) - 1);
            }

            int already_printed = 0;
            for (int j = 0; j < num_printed_dirs; j++) {
                if (strcmp(printed_dirs[j], partial_path) == 0) {
                    already_printed = 1;
                    break;
                }
            }
            if (!already_printed) {
                print_indent(depth);
                printf("%s\n", dir_name);
                if (num_printed_dirs < 64) {
                    strncpy(printed_dirs[num_printed_dirs++], partial_path, 4095);
                }
            }
        }

        char filename[256];
        if (get_component(rel_path, total_depth, filename, sizeof(filename))) {
            print_indent(total_depth);
            printf("%s (%ld bytes) [Worker %d]\n",
                   filename,
                   entries[i].size,
                   (int)entries[i].worker_pid);
        }
    }
}

void print_summary(int num_workers, int total_scanned, int total_matches, WorkerResult *results, int num_results)
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
