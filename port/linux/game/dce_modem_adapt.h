/*
DCE_MODEM_ADAPT.H

DCE official server: a modem client's measured, adaptive byte budget, and
the send period that keeps it (network_distributed.c distributed_slow_adapt,
HALO_DCE_MODEM_ADAPT, on unless set to 0).

The budget is the shared per-peer line module of the Dreamcast client
(port/third_party/net_budget, a verbatim copy of lamb2k/dc-halo
src/net_budget.h and src/net/net_budget.c at 022f93b5), fed with the round
trips of the client's acknowledgements of its reliable stream (the tunnel's
KCP, port/linux/src/p2p.c p2p_take_line_samples): once a second the
window's shortest round trip over the shortest of the last 10 to 20 s is the
queue on the line; 200 ms or more cuts the budget by a quarter, 70 ms or
less raises it 100 B/s while the budget was what held the period up (and
KCP resent nothing that second). HALO_DCE_MODEM_BUDGET is where it starts,
HALO_DCE_MODEM_BUDGET_MAX (else the tunnel's wire rate) its ceiling, 600
its floor.

The period: the shortest that keeps the client's bytes a second within the
budget, from what each period was measured to cost (an average of the
seconds spent at it) or, for a period not tried yet, the current cost
scaled by the period. Over the budget, or a queue on the line: longer at
once, as far as the costs say; under: one tick shorter a second, only when
that period's cost fits.

No game headers: a model of a line drives it in
port/linux/tests/dce_modem_adapt_test.c (tools/test_dce_modem_adapt.py).
*/

#ifndef DCE_MODEM_ADAPT_H
#define DCE_MODEM_ADAPT_H

#include "../../third_party/net_budget/src/net_budget.h"

#define DCE_MODEM_ADAPT_PERIOD_MIN 2
#define DCE_MODEM_ADAPT_PERIOD_MAX 10
#define DCE_MODEM_ADAPT_FLOOR 600 /* B/s, as the fixed budget's */
#define DCE_MODEM_ADAPT_STEP 100 /* B/s a quiet second */

struct dce_modem_adapt
{
	net_budget_peer_t budget;
	/* the module's own clock: 30 ticks a window, whatever the game's time does */
	uint32_t tick;
	/* each period's measured cost, bytes a second (0: not tried) */
	long period_rate[DCE_MODEM_ADAPT_PERIOD_MAX + 1];
	/* this window: round-trip samples, the shortest, KCP resends */
	long samples;
	long rtt_min;
	long resends;
	/* the last window's, for the log */
	long last_samples;
	long last_rtt_min;
	long last_resends;
	int ready;
};

/* start: bytes a second; floor..ceil: the budget's range */
void dce_modem_adapt_init(struct dce_modem_adapt *adapt, long start, long floor, long ceil);
/* each tick: the shortest acknowledgement round trip since the last call (ms, below 0: none) and the resends since */
void dce_modem_adapt_note(struct dce_modem_adapt *adapt, long rtt_ms, long resends);
/* once a second: the bytes a second the client was sent at *period; moves *period; nonzero when the budget or the
period changed */
int dce_modem_adapt_window(struct dce_modem_adapt *adapt, long rate, short *period);
static inline long dce_modem_adapt_budget(const struct dce_modem_adapt *adapt) { return (long)net_budget_rate(&adapt->budget); }
static inline long dce_modem_adapt_queue_ms(const struct dce_modem_adapt *adapt) { return (long)net_budget_queue_ms(&adapt->budget); }

#endif
