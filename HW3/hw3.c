#include "hw3.h"

/* ── Argument parsing ────────────────────────────────────────────────── */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -f <floors> -w <word_carriers> -l <letter_carriers> "
        "-s <sorters> -c <capacity> -d <delivery_cap> -r <reposition_cap> "
        "-i <input> -o <output>\n", prog);
}

static int parse_positive(const char *arg, const char *name)
{
    char *end = NULL;
    long v = strtol(arg, &end, 10);
    if (end == arg || *end || v < 1 || v > 2147483647L) fail("Invalid %s: %s", name, arg);
    return (int)v;
}

void parse_args(int argc, char **argv, Config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    int opt;
    while ((opt = getopt(argc, argv, "f:w:l:s:c:d:r:i:o:")) != -1) {
        switch (opt) {
            case 'f': cfg->num_floors                  = parse_positive(optarg, "num_floors"); break;
            case 'w': cfg->word_carriers_per_floor     = parse_positive(optarg, "word_carriers_per_floor"); break;
            case 'l': cfg->letter_carriers_per_floor   = parse_positive(optarg, "letter_carriers_per_floor"); break;
            case 's': cfg->sorting_processes_per_floor = parse_positive(optarg, "sorting_processes_per_floor"); break;
            case 'c': cfg->max_words_per_floor         = parse_positive(optarg, "max_words_per_floor"); break;
            case 'd': cfg->delivery_capacity           = parse_positive(optarg, "delivery_capacity"); break;
            case 'r': cfg->reposition_capacity         = parse_positive(optarg, "reposition_capacity"); break;
            case 'i': strncpy(cfg->input_path,  optarg, PATH_MAX - 1); break;
            case 'o': strncpy(cfg->output_path, optarg, PATH_MAX - 1); break;
            default:  usage(argv[0]); exit(EXIT_FAILURE);
        }
    }
    if (!cfg->num_floors || !cfg->word_carriers_per_floor ||
        !cfg->letter_carriers_per_floor || !cfg->sorting_processes_per_floor ||
        !cfg->max_words_per_floor || !cfg->delivery_capacity ||
        !cfg->reposition_capacity || !cfg->input_path[0] || !cfg->output_path[0]) {
        usage(argv[0]); exit(EXIT_FAILURE);
    }
    if (cfg->num_floors > MAX_FLOORS) fail("num_floors exceeds MAX_FLOORS (%d)", MAX_FLOORS);
    if (access(cfg->input_path, R_OK) == -1)
        fail("Input file not readable: %s", cfg->input_path);
}

/* ── Signal handler ──────────────────────────────────────────────────── */
static void signal_handler(int sig)
{
    (void)sig;
    g_stop = 1;
    if (g_state) {
        g_state->shutdown = 1;
        if (g_state->parent_event) sem_post(g_state->parent_event);
    }
}

/* ── Parent helpers ──────────────────────────────────────────────────── */
static void wake_all(SharedState *st)
{
    unlock_sem(st->parent_event, "sem_post(parent_event)");
    for (int i = 0; i < MAX_CARRIERS; ++i)
        unlock_sem(st->carriers[i].event_sem, "sem_post(carrier_event)");
    for (int i = 0; i < 16; ++i) {
        unlock_sem(st->delivery_items,     "sem_post(delivery_items)");
        unlock_sem(st->reposition_items,   "sem_post(reposition_items)");
        unlock_sem(st->word_available_sem, "sem_post(word_available_sem)");
    }
    for (int i = 0; i < st->cfg.num_floors; ++i)
        unlock_sem(st->floor_delivery_sem[i], "sem_post(floor_delivery_sem)");
}

static void terminate_children(pid_t *children, int count)
{
    for (int i = 0; i < count; ++i) if (children[i] > 0) kill(children[i], SIGTERM);
    for (int i = 0; i < count; ++i) if (children[i] > 0) waitpid(children[i], NULL, 0);
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    Config cfg; parse_args(argc, argv, &cfg);

    SharedState *st = mmap(NULL, sizeof(*st), PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (st == MAP_FAILED) fail("mmap: %s", strerror(errno));
    g_state = st;
    init_shared_state(st, &cfg);
    load_input(st);

    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler; sigemptyset(&sa.sa_mask);
    check_err(sigaction(SIGINT,  &sa, NULL), "sigaction(SIGINT)");
    check_err(sigaction(SIGTERM, &sa, NULL), "sigaction(SIGTERM)");

    printf("Program is starting...\n");
    printf("Input file is being read...\n");
    printf("Shared memory is initialized...\n");
    printf("Synchronization primitives are created...\n");
    printf("Processes are being created...\n");
    printf("[PID:%d] Parent process started\n", (int)getpid());

    pid_t children[MAX_CHILDREN]; int child_count = 0;

    for (int f = 0; f < cfg.num_floors; ++f) {
        printf("--- Initializing Floor %d ---\n", f); fflush(stdout);

        for (int i = 0; i < cfg.word_carriers_per_floor; ++i) {
            int slot = reserve_wcarrier_slot(st, f);
            if (slot < 0) fail("Too many word carriers");
            pid_t pid = do_fork("word-carrier");
            if (pid == 0) word_carrier_loop(st, f, slot);
            children[child_count++] = pid;
        }
        for (int i = 0; i < cfg.letter_carriers_per_floor; ++i)
            spawn_letter_carrier(st, f, children, &child_count);

        for (int i = 0; i < cfg.sorting_processes_per_floor; ++i) {
            int slot = reserve_sorter_slot(st, f);
            if (slot < 0) fail("Too many sorters");
            pid_t pid = do_fork("sorter");
            if (pid == 0) sorter_loop(st, f, slot);
            children[child_count++] = pid;
        }
    }

    { pid_t pid = do_fork("delivery-elevator");
      if (pid == 0) delivery_elevator_loop(st);
      children[child_count++] = pid; }

    { pid_t pid = do_fork("reposition-elevator");
      if (pid == 0) reposition_elevator_loop(st);
      children[child_count++] = pid; }

    /* seed word_available_sem so word carriers don't block on the first pass */
    for (int i = 0; i < st->total_words; ++i)
        unlock_sem(st->word_available_sem, "sem_post(word_available_sem)");

    while (!st->shutdown && !g_stop && st->completed_words < st->total_words) {
        int rc = wait_event(st->parent_event, 1000);
        if (rc == -1 && !st->shutdown && !g_stop) fail("parent wait failed");
        maybe_spawn_more_carriers(st, children, &child_count);
    }

    st->shutdown = 1;
    wake_all(st);

    if (st->completed_words == st->total_words && !g_stop) {
        printf("All words have been transported and sorted...\n");
        printf("Output file is being created...\n");
        write_output(st);
        print_summary(st);
        printf("Program terminated successfully.\n");
    } else {
        fprintf(stderr, "Program interrupted before normal completion.\n");
    }

    terminate_children(children, child_count);
    munmap(st, sizeof(*st));
    cleanup_named_sems();
    return 0;
}
