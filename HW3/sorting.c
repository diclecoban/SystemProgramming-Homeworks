#include "hw3.h"

void sorter_loop(SharedState *st, int floor, int slot)
{
    unsigned seed = make_seed(floor + slot * 17);
    st->sorters[slot].pid = getpid();
    safe_log("[PID:%d] Sorting-process_%d initialized on floor %d\n",
             (int)getpid(), slot, floor);

    while (!st->shutdown && !g_stop) {
        int progress = 0;
        for (int off = 0; off < st->total_words; ++off) {
            int wi = (off + rand_r(&seed) % (st->total_words ? st->total_words : 1))
                     % st->total_words;
            SharedWord *w = &st->words[wi];
            if (!w->admitted || w->completed || w->sorting_floor != floor) continue;

            /* try to acquire exclusive access to this word's sort state */
            if (sem_trywait(w->sorter_lock) == -1) {
                if (errno != EAGAIN) fail("sem_trywait sorter_lock: %s", strerror(errno));
                continue;
            }
            lock_sem(w->mutex, "sem_wait(word_mutex)");
            if (!w->admitted || w->completed || w->sorting_floor != floor) {
                unlock_sem(w->mutex, "sem_post(word_mutex)");
                unlock_sem(w->sorter_lock, "sem_post(sorter_lock)");
                continue;
            }

            /* apply sorting rules to every occupied, non-fixed slot */
            for (int i = 0; i < w->length; ++i) {
                if (!w->occupied[i] || w->fixed[i]) continue;
                int tgt = w->sorting_meta[i];

                if (tgt == i) {
                    /* Case 3: already in correct position → fix */
                    w->fixed[i] = 1; progress = 1;

                } else if (tgt >= 0 && tgt < w->length && !w->fixed[tgt]) {
                    if (!w->occupied[tgt]) {
                        /* Case 4a: target empty → move */
                        w->sorting_area[tgt] = w->sorting_area[i];
                        w->sorting_meta[tgt] = w->sorting_meta[i];
                        w->occupied[tgt] = 1; w->occupied[i] = 0;
                        w->sorting_area[i] = '\0'; w->sorting_meta[i] = -1;
                        if (w->sorting_meta[tgt] == tgt) w->fixed[tgt] = 1;
                    } else {
                        /* Case 4b: target occupied and not fixed → swap */
                        char tc = w->sorting_area[tgt]; int tm = w->sorting_meta[tgt];
                        w->sorting_area[tgt] = w->sorting_area[i];
                        w->sorting_meta[tgt] = w->sorting_meta[i];
                        w->sorting_area[i] = tc; w->sorting_meta[i] = tm;
                        if (w->sorting_meta[tgt] == tgt) w->fixed[tgt] = 1;
                        if (w->sorting_meta[i]   == i)   w->fixed[i]   = 1;
                    }
                    progress = 1;
                }
                /* Case 4c: target fixed → do nothing */
            }

            if (try_complete_word(st, wi))
                safe_log("[PID:%d] Word %d COMPLETED\n", (int)getpid(), w->word_id);
            unlock_sem(w->mutex, "sem_post(word_mutex)");
            unlock_sem(w->sorter_lock, "sem_post(sorter_lock)");

            if (progress) {
                st->sorters[slot].tasks++;
                /* wake sibling sorters waiting on this floor */
                unlock_sem(st->floor_delivery_sem[floor], "sem_post(floor_delivery_sem)");
                break;
            }
        }

        if (!progress)
            wait_event(st->floor_delivery_sem[floor], 200);
    }
    _exit(0);
}
