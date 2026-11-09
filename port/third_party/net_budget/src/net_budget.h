/* Per-peer line measurement and byte budget: a SHARED, self-contained module (net_budget.c). Plain C, no game state, no heap, no globals:
 * the caller owns one net_budget_peer_t per peer and per direction it sends. Time is the caller's 30 Hz tick.
 *
 * Users today (default off, mk/mp.mk): the DN v7 host's SNAP stream per slow-line guest (NET_SNAP_BUDGET fixed, NET_ADAPT adaptive) and
 * the DN v7 guest's INPUT stream (NET_ADAPT). Later, unchanged: the native crossplay client's uplink (src/net/opence_transport.c,
 * the modem profile's every-4th-tick send in src/net/opence_level_tick.inc) would own one peer for the server, charge every datagram
 * it writes with net_budget_sent, feed its reliable-stream acks as round-trip samples (net_budget_rtt_sample), and ask
 * net_budget_may_send before an optional state send; mandatory traffic is charged, never refused. The crossplay DOWNLINK is the
 * server fork's side (port/linux/game/network_distributed.c distributed_slow_adapt).
 *
 * Why (docs/archive/multiplayer/DC_MODEM_PACING_20261109.md): a DN v7 modem SNAP is 168 B on UDP (DN 10 + MP_AUTH_SNAP_BYTES(2) 111 +
 * vehicle trailer 11 + 36), 204 B on the line; 7.5 Hz is about 1530 B/s before RULES/ITEMS/control, over a 1440 B/s line; the guest's
 * 15 Hz INPUT (about 1640 B/s with control) overflows the other direction. A modem's buffer then queues for many seconds.
 *
 * Model. A token bucket in LINE bytes (payload + DN 10 + IPv4/UDP 28 + PPP 8) refills at `rate` B/s, credit capped at 8 ticks, debt
 * floored at 1 s. Adaptive mode measures the queue on this peer's path from delay samples: one-way samples (any constant clock offset;
 * only their rise over the running minimum counts) or round-trip samples. Every 30 ticks: the window's smallest sample minus the base
 * (smallest over the last 10 to 20 s) is the standing queue. Queue >= NET_BUDGET_QUEUE_HIGH_MS: rate x 3/4 (not below floor).
 * Queue <= NET_BUDGET_QUEUE_LOW_MS and the budget held something back this window: rate + step (not above ceil). No sample: hold.
 * OpenCE: the server fork's distributed_slow_adapt moves a peer's period once a second on the bytes sent against a configured budget
 * (default 2000 B/s); this module also measures the line, so a faster line keeps its full rate. ADAPT, approximate: delay based (a
 * LEDBAT-like minimum filter), not a bandwidth probe.
 * Memory: 64 B per peer and direction (static assert), no VRAM or AICA. */
#ifndef DCE_NET_BUDGET_H
#define DCE_NET_BUDGET_H
#include <stdint.h>

#ifndef DCE_NET_SNAP_BUDGET
#define DCE_NET_SNAP_BUDGET 0
#endif
#if DCE_NET_SNAP_BUDGET != 0 && (DCE_NET_SNAP_BUDGET < 600 || DCE_NET_SNAP_BUDGET > 8000)
#error "DCE_NET_SNAP_BUDGET must be 0 (off) or 600..8000 line bytes per second"
#endif
#ifndef DCE_NET_ADAPT
#define DCE_NET_ADAPT 0
#endif
#if DCE_NET_ADAPT != 0 && DCE_NET_ADAPT != 1
#error "DCE_NET_ADAPT must be 0 (off) or 1"
#endif
#if DCE_NET_ADAPT && DCE_NET_SNAP_BUDGET
#error "DCE_NET_ADAPT replaces DCE_NET_SNAP_BUDGET: set one"
#endif
#if DCE_NET_ADAPT && defined(DCE_NET_INPUT_PERIOD) && DCE_NET_INPUT_PERIOD
#error "DCE_NET_ADAPT replaces DCE_NET_INPUT_PERIOD: set one"
#endif

#define NET_BUDGET_WIRE_EXTRA 46u      /* per datagram: DN head 10 + IPv4/UDP 28 + PPP 8 */
#define NET_BUDGET_CAP_TICKS 8u        /* credit kept at most: no burst after a quiet spell */
#define NET_BUDGET_DEBT_TICKS 30u      /* debt kept at most: one second */
#define NET_BUDGET_KEY_TICKS 30u       /* the DN v7 host's vehicle live-mask beat: never refused (level.c) */
#define NET_BUDGET_WINDOW_TICKS 30u    /* one adaptation step a second */
#define NET_BUDGET_BASE_TICKS 300u     /* the base minimum keeps 10..20 s of history (clock drift and route changes age out) */
#define NET_BUDGET_QUEUE_HIGH_MS 200u
#define NET_BUDGET_QUEUE_LOW_MS 70u

typedef struct { int32_t cur, prev, win; uint8_t have_cur, have_prev, have_win, pad; } net_budget_min_t;
typedef struct {
    int32_t credit;                    /* bytes x 30 (byte-ticks): a refill of rate B/s is rate per tick */
    uint32_t tick, win_tick, base_tick;
    uint16_t rate, floor, ceil, step;  /* B/s */
    net_budget_min_t owd, rtt;         /* one-way delay samples (ticks), round-trip samples (ms) */
    uint16_t queue_ms;                 /* the last window's estimate */
    uint8_t valid, adapt, limited, pad;
} net_budget_peer_t;
_Static_assert(sizeof(net_budget_peer_t) == 64, "net_budget_peer_t budget");

/* rate: start; floor..ceil: the adaptive range (adapt 0: fixed at rate); step: B/s added a quiet window. tick: now. */
void net_budget_init(net_budget_peer_t *p, unsigned rate, unsigned floor, unsigned ceil, unsigned step, int adapt, uint32_t tick);
/* Charge what was sent: a datagram's payload (the module adds NET_BUDGET_WIRE_EXTRA), or bytes already counted on the line. */
void net_budget_sent(net_budget_peer_t *p, unsigned payload_bytes);
void net_budget_sent_line(net_budget_peer_t *p, uint32_t line_bytes);
/* Delay samples of this peer's path: one way in ticks (receiver's clock minus sender's stamp, any constant offset), or a round trip in ms. */
void net_budget_owd_sample(net_budget_peer_t *p, int32_t ticks);
void net_budget_rtt_sample(net_budget_peer_t *p, uint32_t ms);
/* Refill to tick and close any finished window (adaptation), then 1 = send now. must: send whatever the credit (a press, a keyframe),
 * still charged by the caller. A stale tick refills nothing. A refused optional send marks the window limited. */
int net_budget_may_send(net_budget_peer_t *p, uint32_t tick, int must);
/* The first tick, at or after tick (the last may_send's), when an optional send is allowed again at the current rate: a send is allowed
 * while the bucket is not in debt, and the send itself is then charged (so one datagram may take it below zero). */
uint32_t net_budget_next_tick(const net_budget_peer_t *p, uint32_t tick);
static inline unsigned net_budget_rate(const net_budget_peer_t *p) { return p->rate; }
static inline unsigned net_budget_queue_ms(const net_budget_peer_t *p) { return p->queue_ms; }
#endif
