/* Unit tests of the shared per-peer budget module alone (src/net_budget.h, src/net/net_budget.c): no game headers. Built by tests/test_net_budget.py. */
#include <stdio.h>
#include "net_budget.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

/* a peer that sends one 100 B payload each tick it may; samples one-way delay `delay` ticks each tick; returns the ticks sent */
static unsigned drive(net_budget_peer_t *p, uint32_t *tick, unsigned ticks, int32_t offset, int32_t delay) {
    unsigned sent = 0;
    for (unsigned i = 0; i < ticks; i++, (*tick)++) {
        net_budget_owd_sample(p, offset + delay);
        if (net_budget_may_send(p, *tick, 0)) { net_budget_sent(p, 100); sent++; }
    }
    return sent;
}

int main(void) {
    net_budget_peer_t p;
    /* sizes and init clamps */
    CHECK(sizeof p == 64);
    net_budget_init(&p, 100, 600, 8000, 50, 1, 0); CHECK(p.rate == 600);
    net_budget_init(&p, 9000, 600, 8000, 50, 1, 0); CHECK(p.rate == 8000);
    net_budget_init(&p, 1000, 2000, 1500, 50, 1, 0); CHECK(p.floor == 1500 && p.rate == 1500);

    /* fixed bucket: debt, must, refill, cap, stale ticks, next tick */
    net_budget_init(&p, 1200, 1200, 1200, 0, 0, 100);
    CHECK(net_budget_may_send(&p, 100, 0));
    net_budget_sent_line(&p, 1200u * 2u);                    /* two seconds' worth: debt floors at one second */
    CHECK(p.credit == -1200 * 30);
    CHECK(!net_budget_may_send(&p, 104, 0) && p.limited);    /* in debt: refused, the window is marked limited */
    CHECK(net_budget_may_send(&p, 104, 1));                  /* a must still goes */
    CHECK(net_budget_next_tick(&p, 104) == 130);             /* 26 more ticks of refill */
    CHECK(net_budget_may_send(&p, 130, 0));                  /* paid */
    CHECK(net_budget_may_send(&p, 2000, 0) && p.credit == 1200 * (int)NET_BUDGET_CAP_TICKS);   /* capped: no burst after a quiet spell */
    CHECK(net_budget_may_send(&p, 1900, 0) && p.credit == 1200 * (int)NET_BUDGET_CAP_TICKS);   /* a stale tick refills nothing */
    CHECK(net_budget_next_tick(&p, 2000) == 2000);
    net_budget_sent(&p, 100);                                /* payload + 46 on the line */
    CHECK(p.credit == 1200 * (int)NET_BUDGET_CAP_TICKS - 146 * 30);

    /* the bucket holds a fixed rate: 100 B payloads every tick against 1200 B/s for 60 s send about 1200/146 a second */
    net_budget_init(&p, 1200, 1200, 1200, 0, 0, 0);
    uint32_t tick = 0;
    unsigned sent = drive(&p, &tick, 1800, 0, 0);
    CHECK(sent * 146u <= 1200u * 60u + 1200u && sent * 146u >= 1200u * 60u - 1200u);
    CHECK(p.rate == 1200 && p.queue_ms == 0);               /* adapt 0: measured, never moved */

    /* adaptive, quiet line, the budget binding: + step a window up to ceil; a clock offset of a million ticks changes nothing */
    for (int k = 0; k < 2; k++) {
        net_budget_init(&p, 1200, 600, 2000, 100, 1, 0); tick = 0;
        (void)drive(&p, &tick, 300, k ? 1000000 : 0, 3);
        CHECK(p.rate == 2000 && p.queue_ms == 0);            /* 1200 + 8 x 100 caps at 2000 within 10 s */
    }
    /* a standing queue (delay up 10 ticks = 333 ms) cuts it x 3/4 a window to the floor; the queue reads back */
    tick = 0; net_budget_init(&p, 2000, 600, 8000, 100, 1, 0);
    (void)drive(&p, &tick, 60, 50, 0);
    (void)drive(&p, &tick, 31, 50, 10);
    CHECK(p.rate == 1650 && p.queue_ms == 333);           /* two quiet windows raised it to 2200, then x 3/4 */
    (void)drive(&p, &tick, 300, 50, 10);
    CHECK(p.rate == 600);                                   /* 2000 x 3/4^n reaches the floor */
    /* between the thresholds (100 ms): hold */
    net_budget_init(&p, 1500, 600, 8000, 100, 1, 0); tick = 0;
    (void)drive(&p, &tick, 60, 0, 0); (void)drive(&p, &tick, 120, 0, 3);
    CHECK(p.rate == 1700 && p.queue_ms == 100);             /* two quiet windows raised it, then held */
    /* no samples: hold */
    net_budget_init(&p, 1500, 600, 8000, 100, 1, 0);
    for (tick = 0; tick < 600; tick++) if (net_budget_may_send(&p, tick, 0)) net_budget_sent(&p, 400);
    CHECK(p.rate == 1500);
    /* not limited (the sender wants less than the budget): no climb */
    net_budget_init(&p, 1500, 600, 8000, 100, 1, 0);
    for (tick = 0; tick < 600; tick++) { net_budget_owd_sample(&p, 7); if (tick % 30 == 0 && net_budget_may_send(&p, tick, 0)) net_budget_sent(&p, 100); else (void)net_budget_may_send(&p, tick, 1); }
    CHECK(p.rate == 1500);
    /* the base ages: a route that got slower for good (+10 ticks) stops reading as a queue within 20 s */
    net_budget_init(&p, 2000, 600, 8000, 100, 1, 0); tick = 0;
    (void)drive(&p, &tick, 120, 0, 0); (void)drive(&p, &tick, 700, 0, 10);
    CHECK(p.queue_ms == 0);
    /* round-trip samples when there is no one-way sample; one-way wins when both */
    net_budget_init(&p, 2000, 600, 8000, 100, 1, 0);
    for (tick = 0; tick < 31; tick++) { net_budget_rtt_sample(&p, 150); (void)net_budget_may_send(&p, tick, 0); }
    for (; tick < 62; tick++) { net_budget_rtt_sample(&p, 450); (void)net_budget_may_send(&p, tick, 0); }
    CHECK(p.queue_ms == 300 && p.rate == 1500);
    for (; tick < 93; tick++) { net_budget_rtt_sample(&p, 450); net_budget_owd_sample(&p, 5); (void)net_budget_may_send(&p, tick, 0); }
    CHECK(p.queue_ms == 0);
    /* tick wrap */
    net_budget_init(&p, 1200, 1200, 1200, 0, 0, 0xFFFFFFF0u);
    CHECK(net_budget_may_send(&p, 0xFFFFFFF0u, 0)); net_budget_sent_line(&p, 1200);
    CHECK(!net_budget_may_send(&p, 0xFFFFFFFAu, 0) && net_budget_may_send(&p, 14u, 0));
    printf("net_budget_unit: %d failures\n", failures);
    return failures != 0;
}
