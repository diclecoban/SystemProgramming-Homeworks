#include "hw3.h"

/* ── Globals ─────────────────────────────────────────────────────────── */
volatile sig_atomic_t g_stop    = 0;
SharedState          *g_state   = NULL;
char   g_sem_names[4096][64];
sem_t *g_sem_ptrs[4096];
int    g_sem_count = 0;

/* ── Error helpers ───────────────────────────────────────────────────── */
void fail(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap); exit(EXIT_FAILURE);
}

void check_err(int rc, const char *what)
{
    if (rc == -1) fail("%s failed: %s", what, strerror(errno));
}

/* ── Semaphore wrappers ──────────────────────────────────────────────── */
void lock_sem(sem_t *sem, const char *what)
{
    while (sem_wait(sem) == -1) {
        if (errno == EINTR) { if (g_stop) return; continue; }
        fail("%s: %s", what, strerror(errno));
    }
}

void unlock_sem(sem_t *sem, const char *what)
{
    if (sem_post(sem) == -1) fail("%s: %s", what, strerror(errno));
}

void init_semaphore(sem_t **sem, unsigned value)
{
    if (g_sem_count >= 4096) fail("Too many semaphores");
    char name[64];
    snprintf(name, sizeof(name), "/hw3_%d_%d", (int)getpid(), g_sem_count);
    sem_unlink(name);
    sem_t *s = sem_open(name, O_CREAT | O_EXCL, 0600, value);
    if (s == SEM_FAILED) fail("sem_open(%s): %s", name, strerror(errno));
    strncpy(g_sem_names[g_sem_count], name, 63);
    g_sem_ptrs[g_sem_count] = s;
    *sem = s;
    g_sem_count++;
}

void cleanup_named_sems(void)
{
    for (int i = g_sem_count - 1; i >= 0; --i) {
        if (g_sem_ptrs[i] && g_sem_ptrs[i] != SEM_FAILED)
            sem_close(g_sem_ptrs[i]);
        if (g_sem_names[i][0]) sem_unlink(g_sem_names[i]);
    }
}

/* ── Misc helpers ────────────────────────────────────────────────────── */
void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        if (g_stop) break;
}

unsigned make_seed(int salt)
{
    return (unsigned)(time(NULL) ^ getpid() ^ (salt * 2654435761u));
}

void safe_log(const char *fmt, ...)
{
    if (!g_state) return;
    lock_sem(g_state->print_mutex, "sem_wait(print_mutex)");
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap); va_end(ap); fflush(stdout);
    unlock_sem(g_state->print_mutex, "sem_post(print_mutex)");
}

/* ── Semaphore wait (macOS-safe) ─────────────────────────────────────── */
static int timed_wait_event(sem_t *sem, long timeout_ms)
{
    long waited = 0;
    while (waited <= timeout_ms) {
        if (sem_trywait(sem) == 0) return 0;
        if (errno == EINTR) { if (g_stop) return -1; continue; }
        if (errno != EAGAIN) return -1;
        msleep(10); waited += 10;
    }
    return 1;
}

int wait_event(sem_t *sem, long timeout_ms)
{
#ifdef __APPLE__
    if (timeout_ms < 0) {
        for (;;) {
            if (sem_wait(sem) == 0) return 0;
            if (errno == EINTR && !g_stop) continue;
            return -1;
        }
    }
    return timed_wait_event(sem, timeout_ms);
#else
    if (timeout_ms < 0) {
        for (;;) {
            if (sem_wait(sem) == 0) return 0;
            if (errno == EINTR && !g_stop) continue;
            return -1;
        }
    }
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) return -1;
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    for (;;) {
        if (sem_timedwait(sem, &ts) == 0) return 0;
        if (errno == EINTR && !g_stop) continue;
        if (errno == ETIMEDOUT) return 1;
        return -1;
    }
#endif
}

/* ── Shared state init ───────────────────────────────────────────────── */
void init_shared_state(SharedState *st, const Config *cfg)
{
    memset(st, 0, sizeof(*st));
    st->cfg = *cfg; st->parent_pid = getpid();
    st->delivery_direction = 1; st->reposition_direction = 1;
    init_semaphore(&st->global_mutex, 1);
    init_semaphore(&st->print_mutex, 1);
    init_semaphore(&st->delivery_mutex, 1);
    init_semaphore(&st->delivery_items, 0);
    init_semaphore(&st->reposition_mutex, 1);
    init_semaphore(&st->reposition_items, 0);
    init_semaphore(&st->parent_event, 0);
    init_semaphore(&st->word_available_sem, 0);
    for (int i = 0; i < cfg->num_floors; ++i) {
        init_semaphore(&st->floors[i].mutex, 1);
        init_semaphore(&st->floor_delivery_sem[i], 0);
    }
    for (int i = 0; i < MAX_CARRIERS; ++i)
        init_semaphore(&st->carriers[i].event_sem, 0);
}

/* ── Slot reservation ────────────────────────────────────────────────── */
int reserve_wcarrier_slot(SharedState *st, int floor)
{
    int slot = -1;
    lock_sem(st->global_mutex, "sem_wait(global_mutex)");
    if (st->next_wcarrier_slot < MAX_WCARRIERS) {
        slot = st->next_wcarrier_slot++;
        st->wcarriers[slot].floor = floor;
    }
    unlock_sem(st->global_mutex, "sem_post(global_mutex)");
    return slot;
}

int reserve_sorter_slot(SharedState *st, int floor)
{
    int slot = -1;
    lock_sem(st->global_mutex, "sem_wait(global_mutex)");
    if (st->next_sorter_slot < MAX_SORTERS) {
        slot = st->next_sorter_slot++;
        st->sorters[slot].floor = floor;
    }
    unlock_sem(st->global_mutex, "sem_post(global_mutex)");
    return slot;
}

int reserve_carrier_slot(SharedState *st, int floor)
{
    int slot = -1;
    lock_sem(st->global_mutex, "sem_wait(global_mutex)");
    if (st->next_carrier_slot < MAX_CARRIERS) {
        slot = st->next_carrier_slot++;
        CarrierState *c = &st->carriers[slot];
        c->current_floor = floor; c->active = 1;
        lock_sem(st->floors[floor].mutex, "sem_wait(floor_mutex)");
        st->floors[floor].carrier_count++;
        st->respawn_pending[floor] = 0;
        unlock_sem(st->floors[floor].mutex, "sem_post(floor_mutex)");
    }
    unlock_sem(st->global_mutex, "sem_post(global_mutex)");
    return slot;
}

/* ── Floor movement ──────────────────────────────────────────────────── */
void move_carrier_between_floors(SharedState *st, CarrierState *carrier, int to_floor)
{
    int old = carrier->current_floor;
    if (old == to_floor) return;

    lock_sem(st->floors[old].mutex, "sem_wait(floor_mutex)");
    st->floors[old].carrier_count--;
    int now_empty = (st->floors[old].carrier_count == 0);
    unlock_sem(st->floors[old].mutex, "sem_post(floor_mutex)");

    lock_sem(st->floors[to_floor].mutex, "sem_wait(floor_mutex)");
    st->floors[to_floor].carrier_count++;
    st->respawn_pending[to_floor] = 0;
    unlock_sem(st->floors[to_floor].mutex, "sem_post(floor_mutex)");

    carrier->current_floor = to_floor;

    if (now_empty && old >= 0 && old < st->cfg.num_floors) {
        lock_sem(st->floors[old].mutex, "sem_wait(floor_mutex)");
        if (st->floors[old].carrier_count == 0 && !st->respawn_pending[old]) {
            st->respawn_pending[old] = 1;
            unlock_sem(st->floors[old].mutex, "sem_post(floor_mutex)");
            unlock_sem(st->parent_event, "sem_post(parent_event)");
            return;
        }
        unlock_sem(st->floors[old].mutex, "sem_post(floor_mutex)");
    }
}

/* ── Character placement and delivery ───────────────────────────────── */
int place_char_in_sorting_area(SharedState *st, int wi, int ci)
{
    SharedWord *w = &st->words[wi];
    for (int i = 0; i < w->length; ++i) {
        if (!w->fixed[i] && !w->occupied[i]) {
            w->sorting_area[i] = w->text[ci];
            w->sorting_meta[i] = ci;
            w->occupied[i] = 1;
            w->char_arrived[ci] = 1;
            st->chars_transported++;
            return i;
        }
    }
    return -1;
}

/* Call with w->mutex held. Returns 1 on success. */
int deliver_char(SharedState *st, int wi, int ci)
{
    SharedWord *w = &st->words[wi];
    int sf = w->sorting_floor;
    if (place_char_in_sorting_area(st, wi, ci) >= 0) {
        w->char_delivered[ci] = 1;
        w->char_in_transit[ci] = 0;
        unlock_sem(st->floor_delivery_sem[sf], "sem_post(floor_delivery_sem)");
        return 1;
    }
    w->char_claimed[ci] = 0;
    w->char_in_transit[ci] = 0;
    return 0;
}

/* Call with w->mutex held. Returns 1 if word just completed. */
int try_complete_word(SharedState *st, int wi)
{
    SharedWord *w = &st->words[wi];
    for (int i = 0; i < w->length; ++i)
        if (!w->fixed[i]) return 0;
    if (w->completed) return 0;
    w->completed = 1;
    st->completed_words++;
    if (w->active_reservation) {
        int af = w->arrival_floor, sf = w->sorting_floor;
        int lo = af < sf ? af : sf, hi = af < sf ? sf : af;
        lock_sem(st->floors[lo].mutex, "sem_wait(floor_mutex)");
        if (hi != lo) lock_sem(st->floors[hi].mutex, "sem_wait(floor_mutex)");
        st->floors[af].active_words--;
        if (sf != af) st->floors[sf].active_words--;
        if (hi != lo) unlock_sem(st->floors[hi].mutex, "sem_post(floor_mutex)");
        unlock_sem(st->floors[lo].mutex, "sem_post(floor_mutex)");
        w->active_reservation = 0;
    }
    unlock_sem(st->word_available_sem, "sem_post(word_available_sem)");
    if (st->completed_words == st->total_words)
        unlock_sem(st->parent_event, "sem_post(parent_event)");
    return 1;
}

/* ── Input / output ──────────────────────────────────────────────────── */
static int is_lower_english(const char *s)
{
    for (size_t i = 0; s[i]; ++i)
        if (!(s[i] >= 'a' && s[i] <= 'z')) return 0;
    return 1;
}

void load_input(SharedState *st)
{
    FILE *fp = fopen(st->cfg.input_path, "r");
    if (!fp) fail("Cannot open input: %s", strerror(errno));
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (st->total_words >= MAX_WORDS) fail("Input exceeds MAX_WORDS (%d)", MAX_WORDS);
        int id, sf; char word[MAX_WORD_LEN];
        if (sscanf(line, "%d %63s %d", &id, word, &sf) != 3)
            fail("Invalid input line: %s", line);
        if (sf < 0 || sf >= st->cfg.num_floors)
            fail("Invalid sorting floor %d for word %d", sf, id);
        if (!is_lower_english(word)) fail("Non-lowercase word: %s", word);
        SharedWord *w = &st->words[st->total_words++];
        memset(w, 0, sizeof(*w));
        w->word_id = id; w->sorting_floor = sf;
        strncpy(w->text, word, sizeof(w->text) - 1);
        w->length = (int)strlen(w->text);
        for (int i = 0; i < w->length; ++i) w->sorting_meta[i] = -1;
        init_semaphore(&w->mutex, 1);
        init_semaphore(&w->sorter_lock, 1);
    }
    fclose(fp);
    if (!st->total_words) fail("Input file is empty");
}

static int cmp_word_ptrs(const void *a, const void *b)
{
    const SharedWord *wa = *(const SharedWord * const *)a;
    const SharedWord *wb = *(const SharedWord * const *)b;
    if (wa->sorting_floor != wb->sorting_floor) return wa->sorting_floor - wb->sorting_floor;
    return wa->word_id - wb->word_id;
}

void write_output(SharedState *st)
{
    SharedWord *ordered[MAX_WORDS];
    for (int i = 0; i < st->total_words; ++i) ordered[i] = &st->words[i];
    qsort(ordered, (size_t)st->total_words, sizeof(ordered[0]), cmp_word_ptrs);
    FILE *fp = fopen(st->cfg.output_path, "w");
    if (!fp) fail("Cannot create output: %s", strerror(errno));
    for (int i = 0; i < st->total_words; ++i)
        fprintf(fp, "%d %s %d\n",
                ordered[i]->word_id, ordered[i]->text, ordered[i]->sorting_floor);
    fclose(fp);
    st->output_generated = 1;
}

void print_summary(SharedState *st)
{
    safe_log("System Summary:\n");
    safe_log("Total words: %d\n",                        st->total_words);
    safe_log("Completed words: %d\n",                    st->completed_words);
    safe_log("Retries: %d\n",                            st->retries);
    safe_log("Characters transported: %d\n",             st->chars_transported);
    safe_log("Delivery elevator operations: %d\n",       st->delivery_ops);
    safe_log("Reposition elevator operations: %d\n",     st->reposition_ops);
    for (int i = 0; i < st->next_wcarrier_slot; ++i)
        if (st->wcarriers[i].pid > 0)
            safe_log("Word-carrier-process_%d stats: admissions=%d retries=%d\n",
                     i, st->wcarriers[i].admissions, st->wcarriers[i].retries);
    for (int i = 0; i < st->next_carrier_slot; ++i)
        if (st->carriers[i].pid > 0)
            safe_log("Letter-carrier-process_%d stats: deliveries=%d direct=%d repositions=%d current_floor=%d\n",
                     i, st->carriers[i].deliveries, st->carriers[i].direct_placements,
                     st->carriers[i].repositions, st->carriers[i].current_floor);
    for (int i = 0; i < st->next_sorter_slot; ++i)
        if (st->sorters[i].pid > 0)
            safe_log("Sorting-process_%d stats: tasks=%d floor=%d\n",
                     i, st->sorters[i].tasks, st->sorters[i].floor);
}

/* ── Fork helper ─────────────────────────────────────────────────────── */
pid_t do_fork(const char *role)
{
    pid_t pid = fork();
    if (pid < 0) fail("fork(%s) failed: %s", role, strerror(errno));
    return pid;
}
