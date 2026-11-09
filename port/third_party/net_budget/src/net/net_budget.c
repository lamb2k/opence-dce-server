/* Per-peer line measurement and byte budget (net_budget.h). Self-contained: no game headers, no heap, no globals. */
#include <string.h>
#include "../net_budget.h"

static void min_note(net_budget_min_t *m, int32_t v) {
    if (!m->have_cur || v < m->cur) { m->cur = v; m->have_cur = 1; }
    if (!m->have_win || v < m->win) { m->win = v; m->have_win = 1; }
}

/* the base: the smallest sample of the current and the previous base window (10..20 s of history) */
static int min_base(const net_budget_min_t *m, int32_t *out) {
    if (!m->have_cur && !m->have_prev) return 0;
    *out = !m->have_prev ? m->cur : !m->have_cur ? m->prev : (m->cur < m->prev ? m->cur : m->prev);
    return 1;
}

static void min_age(net_budget_min_t *m) { m->prev = m->cur; m->have_prev = m->have_cur; m->have_cur = 0; }

void net_budget_init(net_budget_peer_t *p, unsigned rate, unsigned floor, unsigned ceil, unsigned step, int adapt, uint32_t tick) {
    memset(p, 0, sizeof *p);
    if (ceil > 0xFFFFu) ceil = 0xFFFFu;
    if (floor > ceil) floor = ceil;
    if (rate < floor) rate = floor;
    if (rate > ceil) rate = ceil;
    p->rate = (uint16_t)rate; p->floor = (uint16_t)floor; p->ceil = (uint16_t)ceil; p->step = (uint16_t)(step > 0xFFFFu ? 0xFFFFu : step);
    p->adapt = adapt != 0; p->tick = p->win_tick = p->base_tick = tick; p->valid = 1;
}

void net_budget_sent_line(net_budget_peer_t *p, uint32_t line_bytes) {
    const int64_t floor = -(int64_t)p->rate * NET_BUDGET_DEBT_TICKS;
    const int64_t c = (int64_t)p->credit - (int64_t)line_bytes * 30;
    p->credit = (int32_t)(c < floor ? floor : c);
}

void net_budget_sent(net_budget_peer_t *p, unsigned payload_bytes) { net_budget_sent_line(p, payload_bytes + NET_BUDGET_WIRE_EXTRA); }

void net_budget_owd_sample(net_budget_peer_t *p, int32_t ticks) { min_note(&p->owd, ticks); }
void net_budget_rtt_sample(net_budget_peer_t *p, uint32_t ms) { min_note(&p->rtt, (int32_t)(ms > 0x7FFFFFFFu ? 0x7FFFFFFFu : ms)); }

/* One window closed: the standing queue is its smallest sample over the base; one-way samples first, round trips otherwise. */
static void net_budget_window(net_budget_peer_t *p) {
    int32_t base;
    int have = 0;
    int64_t q = 0;
    if (p->owd.have_win && min_base(&p->owd, &base)) { q = ((int64_t)p->owd.win - base) * 1000 / 30; have = 1; }
    else if (p->rtt.have_win && min_base(&p->rtt, &base)) { q = (int64_t)p->rtt.win - base; have = 1; }
    if (have) {
        if (q < 0) q = 0;
        p->queue_ms = (uint16_t)(q > 0xFFFF ? 0xFFFF : q);
        if (p->adapt) {
            if (q >= NET_BUDGET_QUEUE_HIGH_MS) { const unsigned r = (unsigned)p->rate * 3u / 4u; p->rate = (uint16_t)(r < p->floor ? p->floor : r); }
            else if (q <= NET_BUDGET_QUEUE_LOW_MS && p->limited) { const unsigned r = (unsigned)p->rate + p->step; p->rate = (uint16_t)(r > p->ceil ? p->ceil : r); }
        }
    }
    p->owd.have_win = p->rtt.have_win = 0; p->limited = 0;
}

int net_budget_may_send(net_budget_peer_t *p, uint32_t tick, int must) {
    const uint32_t elapsed = tick - p->tick;
    if (elapsed < 0x80000000u) {
        const int64_t cap = (int64_t)p->rate * NET_BUDGET_CAP_TICKS;
        const int64_t c = (int64_t)p->credit + (int64_t)elapsed * p->rate;
        p->credit = (int32_t)(c > cap ? cap : c);
        p->tick = tick;
        if (tick - p->win_tick >= NET_BUDGET_WINDOW_TICKS) { net_budget_window(p); p->win_tick = tick; }
        if (tick - p->base_tick >= NET_BUDGET_BASE_TICKS) { min_age(&p->owd); min_age(&p->rtt); p->base_tick = tick; }
    }
    const int ok = must || p->credit >= 0;
    if (!ok) p->limited = 1;
    return ok;
}

uint32_t net_budget_next_tick(const net_budget_peer_t *p, uint32_t tick) {
    if (p->credit >= 0 || !p->rate) return tick;
    return tick + (uint32_t)((-(int64_t)p->credit + p->rate - 1) / p->rate);
}
