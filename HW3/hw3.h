#ifndef HW3_H
#define HW3_H

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_ANON
#define MAP_ANON 0x1000
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define MAX_WORDS      256
#define MAX_WORD_LEN   64
#define MAX_FLOORS     32
#define MAX_CARRIERS   512
#define MAX_SORTERS    512
#define MAX_WCARRIERS  512
#define MAX_REQUESTS   2048
#define MAX_CHILDREN   2048

/* ── Types ───────────────────────────────────────────────────────────── */

typedef struct {
    int  num_floors, word_carriers_per_floor, letter_carriers_per_floor;
    int  sorting_processes_per_floor, max_words_per_floor;
    int  delivery_capacity, reposition_capacity;
    char input_path[PATH_MAX], output_path[PATH_MAX];
} Config;

typedef struct {
    int  word_id, sorting_floor, arrival_floor, length;
    int  claimed, admitted, completed, active_reservation, claim_owner;
    char text[MAX_WORD_LEN];
    char sorting_area[MAX_WORD_LEN];
    int  sorting_meta[MAX_WORD_LEN];
    int  occupied[MAX_WORD_LEN], fixed[MAX_WORD_LEN];
    int  char_claimed[MAX_WORD_LEN], char_delivered[MAX_WORD_LEN];
    int  char_in_transit[MAX_WORD_LEN], char_arrived[MAX_WORD_LEN];
    sem_t *mutex, *sorter_lock;
} SharedWord;

typedef struct { int active_words, carrier_count; sem_t *mutex; } FloorState;

typedef struct {
    int  used, carrier_slot, from_floor, to_floor, word_index, char_index;
    char ch;
} DeliveryRequest;

typedef struct { int used, carrier_slot, from_floor, to_floor; } RepositionRequest;

typedef struct {
    pid_t pid;
    int   current_floor, active, deliveries, repositions, direct_placements;
    sem_t *event_sem;
} CarrierState;

typedef struct { pid_t pid; int floor, tasks; }               SorterState;
typedef struct { pid_t pid; int floor, admissions, retries; } WCarrierState;

typedef struct {
    Config        cfg;
    int           total_words, completed_words, retries, chars_transported;
    int           delivery_ops, reposition_ops, output_generated, shutdown;
    int           rr_cursor;
    int           next_carrier_slot, next_sorter_slot, next_wcarrier_slot;
    int           delivery_direction, delivery_floor;
    int           reposition_direction, reposition_floor;
    int           respawn_pending[MAX_FLOORS];
    pid_t         parent_pid;
    SharedWord    words[MAX_WORDS];
    FloorState    floors[MAX_FLOORS];
    CarrierState  carriers[MAX_CARRIERS];
    SorterState   sorters[MAX_SORTERS];
    WCarrierState wcarriers[MAX_WCARRIERS];
    DeliveryRequest   delivery_requests[MAX_REQUESTS];
    RepositionRequest reposition_requests[MAX_REQUESTS];
    sem_t *global_mutex, *print_mutex;
    sem_t *delivery_mutex, *delivery_items;
    sem_t *reposition_mutex, *reposition_items;
    sem_t *parent_event, *word_available_sem;
    sem_t *floor_delivery_sem[MAX_FLOORS];
} SharedState;

/* ── Globals (defined in shared_memory.c) ───────────────────────────── */
extern volatile sig_atomic_t g_stop;
extern SharedState          *g_state;
extern char   g_sem_names[4096][64];
extern sem_t *g_sem_ptrs[4096];
extern int    g_sem_count;

/* ── shared_memory.c ─────────────────────────────────────────────────── */
void  fail(const char *fmt, ...);
void  check_err(int rc, const char *what);
void  lock_sem(sem_t *sem, const char *what);
void  unlock_sem(sem_t *sem, const char *what);
void  init_semaphore(sem_t **sem, unsigned value);
void  cleanup_named_sems(void);
void  msleep(long ms);
unsigned make_seed(int salt);
void  safe_log(const char *fmt, ...);
int   wait_event(sem_t *sem, long timeout_ms);
void  init_shared_state(SharedState *st, const Config *cfg);
int   reserve_wcarrier_slot(SharedState *st, int floor);
int   reserve_sorter_slot(SharedState *st, int floor);
int   reserve_carrier_slot(SharedState *st, int floor);
void  move_carrier_between_floors(SharedState *st, CarrierState *carrier, int to_floor);
int   place_char_in_sorting_area(SharedState *st, int wi, int ci);
int   deliver_char(SharedState *st, int wi, int ci);
int   try_complete_word(SharedState *st, int wi);
void  load_input(SharedState *st);
void  write_output(SharedState *st);
void  print_summary(SharedState *st);
pid_t do_fork(const char *role);

/* ── word_carrier.c ──────────────────────────────────────────────────── */
void word_carrier_loop(SharedState *st, int floor, int slot);

/* ── letter_carrier.c ────────────────────────────────────────────────── */
void enqueue_delivery(SharedState *st, int cs, int from, int to, int wi, int ci, char ch);
void enqueue_reposition(SharedState *st, int cs, int from, int to);
void letter_carrier_loop(SharedState *st, int initial_floor, int slot);
void spawn_letter_carrier(SharedState *st, int floor, pid_t *ch, int *cnt);
void maybe_spawn_more_carriers(SharedState *st, pid_t *children, int *count);

/* ── sorting.c ───────────────────────────────────────────────────────── */
void sorter_loop(SharedState *st, int floor, int slot);

/* ── elevator.c ──────────────────────────────────────────────────────── */
void delivery_elevator_loop(SharedState *st);
void reposition_elevator_loop(SharedState *st);

/* ── hw3.c ───────────────────────────────────────────────────────────── */
void parse_args(int argc, char **argv, Config *cfg);

#endif /* HW3_H */
