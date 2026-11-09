/* A model of a modem client's line under the server's adaptive budget and period (port/linux/game/dce_modem_adapt.c), no game:
built and run by tools/test_dce_modem_adapt.py. Modelled, not measured on a console.

The server, per modem client: a fixed part of its bytes (the reliable stream, acks, the client's own state: FIXED B/s,
in bursts) and a state send each period tick (VARIABLE / 30 bytes each, +-20 %), so a period P costs FIXED + VARIABLE / P
B/s: 1000 at 10 ticks, 1400 at 4, 2067 at 2 (fitted to the official server's rows: 969-1030 B/s at period 10, 1355-1474
at 4-5). The line: a FIFO drained at its rate; a reliable piece every 10 ticks, its acknowledgement back after the
base round trip, the queue it waited behind and up to 60 ms of the client's own batching; a piece out longer than
KCP's 1 s minimum RTO is sent again (a resend, bytes on the line too). */
#include <stdio.h>
#include <string.h>
#include "../game/dce_modem_adapt.h"

#define FIXED 733
#define VARIABLE 2667
#define BASE_RTT 180
#define RTO 1000
#define START 1400
#define CEIL 2400

static unsigned long seed;
/* a reliable piece every ACK_EVERY ticks; the client's own batching adds up to JITTER ms to its acknowledgement */
static long ack_every = 10, jitter = 60;
static long rnd(long n) { seed = seed * 1103515245ul + 12345ul; return (long)((seed >> 8) % (unsigned long)n); }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct pending { long due; long rtt; };
struct run
{
	/* per second: B/s sent, queue delay at the second's end (ms), period, budget */
	long rate[600], queue[600], budget[600];
	short period[600];
	long resends_after[600];
	long seconds;
};

/* capacity(s): the line's B/s in second s; samples: whether acknowledgements are fed */
static void simulate(struct run *run, long seconds, long (*capacity)(long), int samples, unsigned long s)
{
	struct dce_modem_adapt adapt;
	struct pending acks[256];
	long ack_count = 0;
	double queue = 0.0;
	long window = 0, tick, resends = 0, total_resends = 0;
	short period = 3; /* the server's first period for a modem client */

	seed = s;
	memset(run, 0, sizeof(*run));
	dce_modem_adapt_init(&adapt, START, DCE_MODEM_ADAPT_FLOOR, CEIL);
	for (tick = 0; tick < seconds * 30; tick++)
	{
		long second = tick / 30;
		double c = (double)capacity(second);
		long bytes = 0, index, rtt_min = -1;

		/* the fixed part comes in bursts of 3 ticks' worth, the state each period tick */
		if (tick % 3 == 0)
			bytes += FIXED / 10 + rnd(FIXED / 20) - FIXED / 40;
		if (tick % period == 0)
			bytes += VARIABLE / 30 + rnd(VARIABLE / 75) - VARIABLE / 150;
		/* a reliable piece every 10 ticks: its acknowledgement's round trip is what the queue makes it */
		if (tick % ack_every == 0 && ack_count < 256)
		{
			long rtt = BASE_RTT + (long)(queue / c * 1000.0) + rnd(jitter);

			if (rtt > RTO)
			{
				/* (KCP sends it again before the acknowledgement can come) */
				resends += 1 + (rtt - RTO) / RTO;
				bytes += 120 * (1 + (rtt - RTO) / RTO);
			}
			acks[ack_count].due = tick + rtt * 30 / 1000;
			acks[ack_count++].rtt = rtt;
		}
		queue += bytes;
		window += bytes;
		queue -= c / 30.0;
		if (queue < 0.0)
			queue = 0.0;
		for (index = 0; index < ack_count;)
		{
			if (acks[index].due <= tick)
			{
				if (rtt_min < 0 || acks[index].rtt < rtt_min)
					rtt_min = acks[index].rtt;
				acks[index] = acks[--ack_count];
			}
			else
				index++;
		}
		dce_modem_adapt_note(&adapt, samples ? rtt_min : -1, resends);
		total_resends += resends;
		resends = 0;
		if (tick % 30 == 29)
		{
			run->rate[second] = window;
			run->queue[second] = (long)(queue / c * 1000.0);
			dce_modem_adapt_window(&adapt, window, &period);
			run->period[second] = period;
			run->budget[second] = dce_modem_adapt_budget(&adapt);
			run->resends_after[second] = total_resends;
			window = 0;
		}
	}
	run->seconds = seconds;
}

static double mean(const long *values, long from, long to)
{
	double sum = 0.0;
	long index;

	for (index = from; index < to; index++)
		sum += (double)values[index];
	return sum / (double)(to - from);
}
static long largest(const long *values, long from, long to)
{
	long most = values[from], index;

	for (index = from; index < to; index++)
		if (values[index] > most)
			most = values[index];
	return most;
}

static long line_1440(long second) { (void)second; return 1440; }
static long line_2200(long second) { (void)second; return 2200; }
static long line_3360(long second) { (void)second; return 3360; }
static long line_drop(long second) { return second < 90 ? 2200 : 1440; }
static long line_rise(long second) { return second < 90 ? 1440 : 2200; }

static void summary(const char *name, const struct run *run, long from, long to)
{
	long index, at_min = 0;

	for (index = from; index < to; index++)
		at_min += run->period[index] == DCE_MODEM_ADAPT_PERIOD_MIN;
	printf("%-11s s%3ld-%3ld: sent %6.0f B/s, queue mean %5.0f ms max %5ld ms, budget %6.0f, period 2 %3ld%%, resends %ld\n", name,
		from, to, mean(run->rate, from, to), mean(run->queue, from, to), largest(run->queue, from, to), mean(run->budget, from, to),
		at_min * 100 / (to - from), run->resends_after[to - 1] - (from ? run->resends_after[from - 1] : 0));
}

int main(void)
{
	static struct run run;
	unsigned long s;

	for (s = 1; s <= 5; s++)
	{
		long second, first_under = -1, first_drained = -1;

		/* a 1440 B/s line (11-09's bad day): close to its speed, no queue that grows, no resends once settled */
		simulate(&run, 300, line_1440, 1, s);
		if (s == 1)
			summary("1440", &run, 30, 300);
		CHECK(mean(run.rate, 30, 300) >= 1440 * 0.75 && mean(run.rate, 30, 300) <= 1440, "seed %lu: %.0f B/s", s, mean(run.rate, 30, 300));
		CHECK(mean(run.queue, 30, 300) < 250.0, "seed %lu: queue %.0f ms", s, mean(run.queue, 30, 300));
		CHECK(largest(run.queue, 30, 300) < 900, "seed %lu: queue max %ld ms", s, largest(run.queue, 30, 300));
		CHECK(mean(run.queue, 200, 300) <= mean(run.queue, 30, 130) + 50.0, "seed %lu: queue grows %.0f -> %.0f ms", s,
			mean(run.queue, 30, 130), mean(run.queue, 200, 300));
		CHECK(run.resends_after[299] - run.resends_after[29] == 0, "seed %lu: %ld resends", s, run.resends_after[299] - run.resends_after[29]);

		/* 2200 B/s (a good call): the spare capacity used, period 2 (2067 B/s) most of the time, no queue */
		simulate(&run, 300, line_2200, 1, s);
		if (s == 1)
			summary("2200", &run, 30, 300);
		CHECK(mean(run.rate, 30, 300) >= 1900, "seed %lu: %.0f B/s", s, mean(run.rate, 30, 300));
		CHECK(mean(run.queue, 30, 300) < 100.0, "seed %lu: queue %.0f ms", s, mean(run.queue, 30, 300));

		/* 3360 B/s (33.6k): the same, the period the limit */
		simulate(&run, 300, line_3360, 1, s);
		if (s == 1)
			summary("3360", &run, 30, 300);
		CHECK(mean(run.rate, 30, 300) >= 1950, "seed %lu: %.0f B/s", s, mean(run.rate, 30, 300));
		CHECK(mean(run.queue, 30, 300) < 50.0, "seed %lu: queue %.0f ms", s, mean(run.queue, 30, 300));

		/* the line drops from 2200 to 1440 at 90 s: under its new speed within seconds, the queue gone soon after */
		simulate(&run, 300, line_drop, 1, s);
		for (second = 90; second < 300; second++)
		{
			if (first_under < 0 && run.rate[second] <= 1440)
				first_under = second;
			if (first_drained < 0 && first_under >= 0 && run.queue[second] < 200)
				first_drained = second;
		}
		if (s == 1)
		{
			summary("drop", &run, 30, 90);
			summary("drop", &run, 90, 120);
			summary("drop", &run, 120, 300);
			printf("drop      seed 1: under 1440 B/s %ld s after the drop, queue under 200 ms %ld s after\n", first_under - 90,
				first_drained - 90);
		}
		CHECK(first_under >= 0 && first_under - 90 <= 5, "seed %lu: under after %ld s", s, first_under - 90);
		CHECK(first_drained >= 0 && first_drained - 90 <= 15, "seed %lu: drained after %ld s", s, first_drained - 90);
		CHECK(mean(run.queue, 120, 300) < 250.0, "seed %lu: queue %.0f ms", s, mean(run.queue, 120, 300));

		/* and back up from 1440 to 2200: the budget climbs to period 2 within 20 s */
		simulate(&run, 200, line_rise, 1, s);
		if (s == 1)
			summary("rise", &run, 90, 200);
		CHECK(mean(run.rate, 110, 200) >= 1900, "seed %lu: %.0f B/s", s, mean(run.rate, 110, 200));

		/* one acknowledgement a second, up to 150 ms of batching on the client: still climbs on a good line, still no
		queue that grows on a bad one */
		ack_every = 30, jitter = 150;
		simulate(&run, 300, line_2200, 1, s);
		if (s == 1)
			summary("sparse2200", &run, 30, 300);
		CHECK(mean(run.rate, 30, 300) >= 1800, "seed %lu: %.0f B/s", s, mean(run.rate, 30, 300));
		simulate(&run, 300, line_1440, 1, s);
		if (s == 1)
			summary("sparse1440", &run, 30, 300);
		CHECK(mean(run.queue, 30, 300) < 300.0 && mean(run.queue, 200, 300) <= mean(run.queue, 30, 130) + 50.0,
			"seed %lu: queue %.0f ms", s, mean(run.queue, 30, 300));
		ack_every = 10, jitter = 60;

		/* no acknowledgement samples at all: the budget holds where it started, the period the shortest within it */
		simulate(&run, 120, line_3360, 0, s);
		if (s == 1)
			summary("nosample", &run, 30, 120);
		CHECK(run.budget[119] == START, "seed %lu: budget %ld", s, run.budget[119]);
		CHECK(mean(run.rate, 30, 120) <= START * 1.05 && mean(run.rate, 30, 120) >= START * 0.85, "seed %lu: %.0f B/s", s,
			mean(run.rate, 30, 120));
	}
	printf("dce_modem_adapt: %d failures\n", failures);
	return failures != 0;
}
