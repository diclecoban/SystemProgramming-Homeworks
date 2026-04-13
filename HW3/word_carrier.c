#include "hw3.h"

void word_carrier_loop(SharedState *st, int floor, int slot)
{
    unsigned seed = make_seed(floor + slot);
    st->wcarriers[slot].pid = getpid();
    safe_log("[PID:%d] Word-carrier-process_%d initialized on floor %d\n",
             (int)getpid(), slot, floor);

    while (!st->shutdown && !g_stop) {
        /* round-robin scan for an unclaimed word */
        int ci = -1;
        lock_sem(st->global_mutex, "sem_wait(global_mutex)");
        for (int seen = 0; seen < st->total_words; ++seen) {
            int idx = (st->rr_cursor + seen) % st->total_words;
            SharedWord *w = &st->words[idx];
            if (!w->claimed && !w->admitted && !w->completed) {
                w->claimed = 1; w->claim_owner = slot;
                st->rr_cursor = (idx + 1) % st->total_words;
                ci = idx; break;
            }
        }
        unlock_sem(st->global_mutex, "sem_post(global_mutex)");

        if (ci == -1) {
            wait_event(st->word_available_sem, 500);
            continue;
        }

        SharedWord *w = &st->words[ci];
        safe_log("[PID:%d] Word-carrier-process_%d claimed word %d\n",
                 (int)getpid(), slot, w->word_id);

        /* atomic all-or-nothing capacity check */
        int af = floor, sf = w->sorting_floor;
        int lo = af < sf ? af : sf, hi = af < sf ? sf : af;
        lock_sem(st->floors[lo].mutex, "sem_wait(floor_mutex)");
        if (hi != lo) lock_sem(st->floors[hi].mutex, "sem_wait(floor_mutex)");
        int same = (af == sf);
        int ok = st->floors[af].active_words < st->cfg.max_words_per_floor &&
                 (same || st->floors[sf].active_words < st->cfg.max_words_per_floor);
        if (ok) { st->floors[af].active_words++; if (!same) st->floors[sf].active_words++; }
        if (hi != lo) unlock_sem(st->floors[hi].mutex, "sem_post(floor_mutex)");
        unlock_sem(st->floors[lo].mutex, "sem_post(floor_mutex)");

        lock_sem(w->mutex, "sem_wait(word_mutex)");
        if (ok) {
            w->admitted = 1; w->arrival_floor = af; w->active_reservation = 1;
            st->wcarriers[slot].admissions++;
            safe_log("[PID:%d] Word %d admitted to floor %d (sorting floor: %d)\n",
                     (int)getpid(), w->word_id, af, sf);
        } else {
            w->claimed = 0; w->claim_owner = -1;
            st->retries++; st->wcarriers[slot].retries++;
            unlock_sem(st->word_available_sem, "sem_post(word_available_sem)");
        }
        unlock_sem(w->mutex, "sem_post(word_mutex)");

        if (!ok) msleep(25 + (rand_r(&seed) % 25));
    }
    _exit(0);
}
