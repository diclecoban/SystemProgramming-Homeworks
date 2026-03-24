#ifndef OUTPUT_H
#define OUTPUT_H

#include <sys/types.h>

/*
 * MatchEntry: records a single matched file for the final tree output.
 */
typedef struct {
    char  path[4096]; /* full path of matched file */
    long  size;       /* file size in bytes */
    pid_t worker_pid; /* PID of the worker that found it */
} MatchEntry;

/*
 * WorkerResult: statistics collected for one worker after it exits.
 */
typedef struct {
    pid_t worker_pid;
    int   match_count;
} WorkerResult;

/*
 * print_tree:
 *   Prints the tree-formatted results.
 *   root_dir    : root directory path
 *   entries     : array of MatchEntry
 *   num_entries : length of entries array
 */
void print_tree(const char   *root_dir,
                MatchEntry   *entries,
                int           num_entries);

/*
 * print_summary:
 *   Prints the --- Summary --- block.
 */
void print_summary(int           num_workers,
                   int           total_scanned,
                   int           total_matches,
                   WorkerResult *results,
                   int           num_results);

#endif /* OUTPUT_H */
