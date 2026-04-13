#include "hw3.h"

/* ── Elevator enqueue helpers ────────────────────────────────────────── */
void enqueue_delivery(SharedState *st, int cs, int from, int to, int wi, int ci, char ch)
{
    lock_sem(st->delivery_mutex, "sem_wait(delivery_mutex)");
    for (int i = 0; i < MAX_REQUESTS; ++i) {
        if (!st->delivery_requests[i].used) {
            st->delivery_requests[i] = (DeliveryRequest){1, cs, from, to, wi, ci, ch};
            unlock_sem(st->delivery_mutex, "sem_post(delivery_mutex)");
            unlock_sem(st->delivery_items, "sem_post(delivery_items)");
            return;
        }
    }
    unlock_sem(st->delivery_mutex, "sem_post(delivery_mutex)");
    fail("Delivery queue overflow");
}

void enqueue_reposition(SharedState *st, int cs, int from, int to)
{
    lock_sem(st->reposition_mutex, "sem_wait(reposition_mutex)");
    for (int i = 0; i < MAX_REQUESTS; ++i) {
        if (!st->reposition_requests[i].used) {
            st->reposition_requests[i] = (RepositionRequest){1, cs, from, to};
            unlock_sem(st->reposition_mutex, "sem_post(reposition_mutex)");
            unlock_sem(st->reposition_items, "sem_post(reposition_items)");
            return;
        }
    }
    unlock_sem(st->reposition_mutex, "sem_post(reposition_mutex)");
    fail("Reposition queue overflow");
}

/* ── Letter-carrier process loop ─────────────────────────────────────── */
void letter_carrier_loop(SharedState *st, int initial_floor, int slot)
{
    unsigned seed = make_seed(initial_floor + slot * 31);
    CarrierState *carrier = &st->carriers[slot];
    carrier->pid = getpid();
    safe_log("[PID:%d] Letter-carrier-process_%d initialized on floor %d\n",
             (int)getpid(), slot, initial_floor);

    while (!st->shutdown && !g_stop) {
        int floor = carrier->current_floor;
        int fw = -1, fc = -1;

        for (int pass = 0; pass < st->total_words && fw == -1; ++pass) {
            int wi = (pass + rand_r(&seed) % (st->total_words ? st->total_words : 1))
                     % st->total_words;
            SharedWord *w = &st->words[wi];
            if (!w->admitted || w->completed || w->arrival_floor != floor) continue;
            lock_sem(w->mutex, "sem_wait(word_mutex)");
            if (w->admitted && !w->completed && w->arrival_floor == floor) {
                for (int j = 0; j < w->length; ++j) {
                    int ci = (j + rand_r(&seed) % w->length) % w->length;
                    if (!w->char_claimed[ci] && !w->char_delivered[ci] && !w->char_in_transit[ci]) {
                        w->char_claimed[ci] = 1; w->char_in_transit[ci] = 1;
                        fw = wi; fc = ci; break;
                    }
                }
            }
            unlock_sem(w->mutex, "sem_post(word_mutex)");
        }

        if (fw == -1) {
            if (st->completed_words >= st->total_words || st->shutdown || g_stop) break;
            int tf = rand_r(&seed) % st->cfg.num_floors;
            if (tf == floor && st->cfg.num_floors > 1) tf = (tf + 1) % st->cfg.num_floors;
            safe_log("[PID:%d] Letter-carrier-process_%d found no available task on floor %d\n",
                     (int)getpid(), slot, floor);
            safe_log("[PID:%d] Letter-carrier-process_%d requested reposition elevator from floor %d\n",
                     (int)getpid(), slot, floor);
            enqueue_reposition(st, slot, floor, tf);
            wait_event(carrier->event_sem, -1);
            continue;
        }

        SharedWord *w = &st->words[fw];
        char ch = w->text[fc]; int dest = w->sorting_floor;
        safe_log("[PID:%d] Letter-carrier-process_%d selected char '%c' of word %d from floor %d\n",
                 (int)getpid(), slot, ch, w->word_id, floor);

        if (dest == floor) {
            lock_sem(w->mutex, "sem_wait(word_mutex)");
            if (deliver_char(st, fw, fc)) carrier->direct_placements++;
            unlock_sem(w->mutex, "sem_post(word_mutex)");
            safe_log("[PID:%d] Destination is same floor -> direct placement\n", (int)getpid());
            safe_log("[PID:%d] Letter-carrier-process_%d brought char '%c' of word %d to floor %d\n",
                     (int)getpid(), slot, ch, w->word_id, floor);
            continue;
        }

        safe_log("[PID:%d] Letter-carrier-process_%d requested delivery elevator from floor %d to floor %d\n",
                 (int)getpid(), slot, floor, dest);
        enqueue_delivery(st, slot, floor, dest, fw, fc, ch);
        wait_event(carrier->event_sem, -1);
    }
    _exit(0);
}

/* ── Spawn / respawn helpers (called from parent) ────────────────────── */
void spawn_letter_carrier(SharedState *st, int floor, pid_t *ch, int *cnt)
{
    int slot = reserve_carrier_slot(st, floor);
    if (slot < 0) fail("Too many letter carriers");
    pid_t pid = do_fork("letter-carrier");
    if (pid == 0) letter_carrier_loop(st, floor, slot);
    st->carriers[slot].pid = pid;
    ch[(*cnt)++] = pid;
}

void maybe_spawn_more_carriers(SharedState *st, pid_t *children, int *count)
{
    for (int f = 0; f < st->cfg.num_floors; ++f) {
        lock_sem(st->floors[f].mutex, "sem_wait(floor_mutex)");
        int empty = (st->floors[f].carrier_count == 0);
        int need  = st->respawn_pending[f];
        unlock_sem(st->floors[f].mutex, "sem_post(floor_mutex)");
        if (empty && need && !st->shutdown && st->completed_words < st->total_words)
            for (int i = 0; i < st->cfg.letter_carriers_per_floor; ++i)
                spawn_letter_carrier(st, f, children, count);
    }
}
