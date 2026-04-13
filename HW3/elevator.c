#include "hw3.h"

/* ── SCAN direction selection ────────────────────────────────────────── */
static int first_delivery_idx(SharedState *st)
{
    int best = -1, dir = st->delivery_direction;
    for (int pass = 0; pass < 2 && best == -1; ++pass) {
        for (int i = 0; i < MAX_REQUESTS; ++i) {
            if (!st->delivery_requests[i].used) continue;
            int diff = st->delivery_requests[i].from_floor - st->delivery_floor;
            if ((dir > 0 && diff >= 0) || (dir < 0 && diff <= 0)) { best = i; break; }
        }
        dir *= -1;
    }
    return best;
}

static int first_reposition_idx(SharedState *st)
{
    int best = -1, dir = st->reposition_direction;
    for (int pass = 0; pass < 2 && best == -1; ++pass) {
        for (int i = 0; i < MAX_REQUESTS; ++i) {
            if (!st->reposition_requests[i].used) continue;
            int diff = st->reposition_requests[i].from_floor - st->reposition_floor;
            if ((dir > 0 && diff >= 0) || (dir < 0 && diff <= 0)) { best = i; break; }
        }
        dir *= -1;
    }
    return best;
}

/* ── Batch collection ────────────────────────────────────────────────── */
static int collect_delivery_batch(SharedState *st, DeliveryRequest *batch, int cap)
{
    int fi = first_delivery_idx(st);
    if (fi == -1) return 0;

    DeliveryRequest first = st->delivery_requests[fi];
    st->delivery_requests[fi].used = 0;
    batch[0] = first; int count = 1;
    int dir = (first.to_floor >= first.from_floor) ? 1 : -1;
    st->delivery_direction = dir;

    for (int i = 0; i < MAX_REQUESTS && count < cap; ++i) {
        if (!st->delivery_requests[i].used) continue;
        DeliveryRequest *r = &st->delivery_requests[i];
        int rdir = (r->to_floor >= r->from_floor) ? 1 : -1;
        if (r->from_floor == first.from_floor && rdir == dir) {
            batch[count++] = *r; r->used = 0;
        }
    }

    /* sort by destination in scan direction (bubble) */
    for (int i = 0; i < count; ++i)
        for (int j = i + 1; j < count; ++j) {
            int sw = (dir > 0) ? (batch[j].to_floor < batch[i].to_floor)
                               : (batch[j].to_floor > batch[i].to_floor);
            if (sw) { DeliveryRequest tmp = batch[i]; batch[i] = batch[j]; batch[j] = tmp; }
        }
    return count;
}

static int collect_reposition_batch(SharedState *st, RepositionRequest *batch, int cap)
{
    int fi = first_reposition_idx(st);
    if (fi == -1) return 0;

    RepositionRequest first = st->reposition_requests[fi];
    st->reposition_requests[fi].used = 0;
    batch[0] = first; int count = 1;
    int dir = (first.to_floor >= first.from_floor) ? 1 : -1;
    st->reposition_direction = dir;

    for (int i = 0; i < MAX_REQUESTS && count < cap; ++i) {
        if (!st->reposition_requests[i].used) continue;
        RepositionRequest *r = &st->reposition_requests[i];
        if (r->from_floor == first.from_floor) {
            batch[count++] = *r; r->used = 0;
        }
    }
    return count;
}

/* ── Delivery elevator process ───────────────────────────────────────── */
void delivery_elevator_loop(SharedState *st)
{
    safe_log("[PID:%d] Delivery elevator process started\n", (int)getpid());
    while (!st->shutdown && !g_stop) {
        if (wait_event(st->delivery_items, -1) != 0) continue;

        lock_sem(st->delivery_mutex, "sem_wait(delivery_mutex)");
        DeliveryRequest batch[64];
        int cap = st->cfg.delivery_capacity < 64 ? st->cfg.delivery_capacity : 64;
        int cnt = collect_delivery_batch(st, batch, cap);
        unlock_sem(st->delivery_mutex, "sem_post(delivery_mutex)");
        if (!cnt) continue;

        DeliveryRequest *first = &batch[0];
        if (first->from_floor > st->delivery_floor)      st->delivery_direction =  1;
        else if (first->from_floor < st->delivery_floor) st->delivery_direction = -1;
        st->delivery_floor = first->from_floor;
        safe_log("[PID:%d] Delivery elevator arrived at floor %d\n",
                 (int)getpid(), first->from_floor);
        safe_log("[PID:%d] Delivery elevator moving %s\n",
                 (int)getpid(), st->delivery_direction > 0 ? "UP" : "DOWN");

        for (int i = 0; i < cnt; ++i) {
            DeliveryRequest *req = &batch[i];
            st->delivery_floor = req->to_floor;
            st->delivery_ops++;
            SharedWord *w = &st->words[req->word_index];
            lock_sem(w->mutex, "sem_wait(word_mutex)");
            int ok = deliver_char(st, req->word_index, req->char_index);
            unlock_sem(w->mutex, "sem_post(word_mutex)");
            CarrierState *carrier = &st->carriers[req->carrier_slot];
            move_carrier_between_floors(st, carrier, req->to_floor);
            carrier->deliveries++;
            if (ok)
                safe_log("[PID:%d] Letter-carrier-process_%d brought char '%c' of word %d to floor %d\n",
                         (int)carrier->pid, req->carrier_slot, req->ch, w->word_id, req->to_floor);
            unlock_sem(carrier->event_sem, "sem_post(carrier_event)");
        }
    }
    _exit(0);
}

/* ── Reposition elevator process ─────────────────────────────────────── */
void reposition_elevator_loop(SharedState *st)
{
    safe_log("[PID:%d] Reposition elevator process started\n", (int)getpid());
    while (!st->shutdown && !g_stop) {
        if (wait_event(st->reposition_items, -1) != 0) continue;

        lock_sem(st->reposition_mutex, "sem_wait(reposition_mutex)");
        RepositionRequest batch[64];
        int cap = st->cfg.reposition_capacity < 64 ? st->cfg.reposition_capacity : 64;
        int cnt = collect_reposition_batch(st, batch, cap);
        unlock_sem(st->reposition_mutex, "sem_post(reposition_mutex)");
        if (!cnt) continue;

        safe_log("[PID:%d] Reposition elevator arrived at floor %d\n",
                 (int)getpid(), batch[0].from_floor);

        for (int i = 0; i < cnt; ++i) {
            RepositionRequest *req = &batch[i];
            st->reposition_floor = req->to_floor;
            st->reposition_ops++;
            CarrierState *carrier = &st->carriers[req->carrier_slot];
            move_carrier_between_floors(st, carrier, req->to_floor);
            carrier->repositions++;
            safe_log("[PID:%d] Letter-carrier-process_%d resumed work on floor %d\n",
                     (int)carrier->pid, req->carrier_slot, req->to_floor);
            unlock_sem(carrier->event_sem, "sem_post(carrier_event)");
        }
    }
    _exit(0);
}
