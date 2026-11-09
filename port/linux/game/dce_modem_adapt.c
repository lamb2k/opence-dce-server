/*
DCE_MODEM_ADAPT.C

A modem client's adaptive budget and send period (dce_modem_adapt.h).
*/

#include <stdlib.h>
#include <string.h>
#include "dce_modem_adapt.h"
/* the shared module, verbatim (its own include finds its header beside it) */
#include "../../third_party/net_budget/src/net/net_budget.c"

void dce_modem_adapt_init(struct dce_modem_adapt *adapt, long start, long floor, long ceil)
{
	memset(adapt, 0, sizeof(*adapt));
	if (floor < 1)
		floor = 1;
	if (ceil < floor)
		ceil = floor;
	if (start < floor)
		start = floor;
	net_budget_init(&adapt->budget, (unsigned)start, (unsigned)floor, (unsigned)ceil, DCE_MODEM_ADAPT_STEP, 1, 0);
	adapt->rtt_min = -1;
	adapt->last_rtt_min = -1;
	adapt->ready = 1;
}

void dce_modem_adapt_note(struct dce_modem_adapt *adapt, long rtt_ms, long resends)
{
	if (rtt_ms >= 0)
	{
		net_budget_rtt_sample(&adapt->budget, (uint32_t)rtt_ms);
		adapt->samples++;
		if (adapt->rtt_min < 0 || rtt_ms < adapt->rtt_min)
			adapt->rtt_min = rtt_ms;
	}
	if (resends > 0)
		adapt->resends += resends;
}

/* what period costs, bytes a second: measured; else, from the two measured periods nearest it, a fixed part and a
part a send (cost = fixed + each / period); else the cost at period now scaled */
static long period_cost(const struct dce_modem_adapt *adapt, short period, short now, long rate)
{
	short a = 0, b = 0, index;
	long each;

	if (adapt->period_rate[period] > 0)
		return adapt->period_rate[period];
	for (index = DCE_MODEM_ADAPT_PERIOD_MIN; index <= DCE_MODEM_ADAPT_PERIOD_MAX; index++)
	{
		if (index == period || adapt->period_rate[index] <= 0)
			continue;
		if (!a || abs(index - period) < abs(a - period))
			b = a, a = index;
		else if (!b || abs(index - period) < abs(b - period))
			b = index;
	}
	if (a && b)
	{
		/* (each = (cost(a) - cost(b)) / (1/a - 1/b), from costs falling with the period; a rise is noise) */
		each = (adapt->period_rate[a] - adapt->period_rate[b]) * a * b / (b - a);
		if (each > 0)
			return adapt->period_rate[a] - each / a + each / period;
	}
	return rate * now / period;
}

int dce_modem_adapt_window(struct dce_modem_adapt *adapt, long rate, short *period)
{
	short given = *period;
	short now = given;
	short next;
	unsigned before = net_budget_rate(&adapt->budget);
	long budget;
	long cost;
	int queue;

	if (now < DCE_MODEM_ADAPT_PERIOD_MIN)
		now = DCE_MODEM_ADAPT_PERIOD_MIN;
	if (now > DCE_MODEM_ADAPT_PERIOD_MAX)
		now = DCE_MODEM_ADAPT_PERIOD_MAX;
	if (rate < 0)
		rate = 0;
	/* (a second KCP resent in raises nothing: limited, set as the last window closed, is taken back) */
	if (adapt->resends)
		adapt->budget.limited = 0;
	net_budget_sent_line(&adapt->budget, (uint32_t)rate);
	adapt->tick += NET_BUDGET_WINDOW_TICKS;
	(void)net_budget_may_send(&adapt->budget, adapt->tick, 1);
	budget = (long)net_budget_rate(&adapt->budget);
	/* (the module keeps its last estimate through a second with no sample: only a fresh one is a queue now) */
	queue = adapt->samples > 0 && net_budget_queue_ms(&adapt->budget) >= NET_BUDGET_QUEUE_HIGH_MS;
	/* this period's cost: the seconds spent at it, the latest weighing half */
	adapt->period_rate[now] = adapt->period_rate[now] > 0 ? (adapt->period_rate[now] + rate) / 2 : rate;
	cost = adapt->period_rate[now];
	/* (a queue: what is sent comes down by a quarter now, whatever the budget, which may stand above it) */
	if (queue && budget > rate * 3 / 4)
		budget = rate * 3 / 4;
	next = now;
	if (cost > budget || queue)
	{
		/* over, or the line queues: longer at once, as far as the costs say, at least a tick */
		next = (short)(now + 1);
		while (next < DCE_MODEM_ADAPT_PERIOD_MAX && period_cost(adapt, next, now, cost) > budget)
			next++;
		if (next > DCE_MODEM_ADAPT_PERIOD_MAX)
			next = DCE_MODEM_ADAPT_PERIOD_MAX;
	}
	else if (now > DCE_MODEM_ADAPT_PERIOD_MIN && period_cost(adapt, (short)(now - 1), now, cost) <= budget)
		next = (short)(now - 1);
	/* the next second: the budget is what holds a shorter period back (unless KCP resends then) */
	adapt->budget.limited = next > DCE_MODEM_ADAPT_PERIOD_MIN;
	adapt->last_samples = adapt->samples;
	adapt->last_rtt_min = adapt->rtt_min;
	adapt->last_resends = adapt->resends;
	adapt->samples = 0;
	adapt->rtt_min = -1;
	adapt->resends = 0;
	*period = next;
	return next != given || net_budget_rate(&adapt->budget) != before;
}
