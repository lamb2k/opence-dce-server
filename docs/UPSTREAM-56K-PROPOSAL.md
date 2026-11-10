# RFC: low-bandwidth mode for small servers (Dreamcast 56k)

## Summary

We'd like to add an opt-in server setting that lets clients on very slow links (dial-up, roughly
1.4 to 3.3 KB/s) play on small OpenCE servers. When enabled, a client that asks for low bandwidth at
join gets a per-client byte budget, batched sends and smaller encodings for the busiest messages.
Every other client, and the network protocol as stock clients see it, stays exactly as it is today.

We already run this on a fork for Dreamcast players on modems, and it works well. The problem is
that we had to bump the network version to 42 to do it, which splits our servers from stock ones.
We'd rather upstream it in a negotiated form and stop maintaining a fork.

## Motivation

The Dreamcast port of Halo (Halo DCE) plays against OpenCE servers. Over broadband this already
works with stock OpenCE. Over the Dreamcast's 33.6k modem it does not:

- The line carries about 3,360 B/s raw and about 2,400 B/s of usable downstream, less on a poor
  call. Each packet also pays about 59 bytes of PPP, IP, UDP and tunnel overhead.
- A stock host sends state 30 times a second. In a shaped test, a stock host offered 93 KB in the
  first second of the join, 90 KB was dropped and the session was lost after about 2 seconds.
- In a live 2-player match the wire to one modem client measured 2,417 B/s, with object-change
  messages (132 bytes each) making up 86% of the reliable stream. Spawns and score updates queued
  behind them.

## What we built

All of it is server-side and per client:

1. **A byte budget per client** (1,400 B/s in production) that sets that client's state period,
   between 2 and 10 ticks, and sends one datagram per period.
2. **Tunnel pacing:** metering of real wire bytes per peer, a token bucket, and a slow-line KCP
   profile (small window, 100 ms flush, no fast resend). Outside match ticks nothing is throttled.
3. **Compact messages** for the high-volume cases: a 132-byte object change becomes 5 bytes for a
   delete or about 28 bytes for a create, and unit and object states go as per-machine delta chains.
4. **Join fixes** so a slow line survives the 13 KB settings download: a resend goes only to the
   machine that missed it, and a machine without a player keeps its slot for 60 seconds instead of 15.

With a 1,400 B/s budget the in-match wire settles at about 1.0 to 1.1 KB/s, three state updates a
second, with no resends. On real Dreamcast hardware over a modem the difference from stock pacing
is night and day: matches went from unplayable to comfortably playable.

## Why it can't go upstream as it is

We raised `HALO_PORT_NETWORK_VERSION` to 42 and sent the compact messages to every client. That
kept one wire format, but stock clients now refuse our servers and our clients refuse stock
servers. It also means we carry a fork of the netcode.

## Proposal

**A server setting, off by default.**

```toml
# Serve clients that ask for low bandwidth (modems, 33.6k and below).
# "auto": a client that asks at join gets a byte budget and compact messages;
#         everyone else is unchanged. "off": stock behaviour.
network.low_bandwidth = "off"
network.low_bandwidth_budget = 1400      # wire bytes per second per low-bandwidth client
network.low_bandwidth_max_players = 4    # player cap while enabled (2 to 4)
```

The server advertises it with one bit in the advertisement flags, so a server browser could show
something like "56k OK". The player cap keeps the promise honest: we have measured 2-player matches,
and estimate 4 players at about 1.1 KB/s with compact messages.

**A capability flag at join.** The client sets one bit saying it wants low bandwidth. It has to be
known before the settings download, so the existing `client_build` message is too late. Either a bit
in the tunnel handshake or a reserved bit in the game's join request would work; we'd follow your
preference. A PC or Android client could expose the same flag, which would also help players on poor
mobile connections and makes the feature testable without a Dreamcast.

**Per-client behaviour, only for flagged clients.** The budget, batching, slow-line KCP profile and
compact messages apply only to machines that asked. The reliable encoder and the delta chains are
already per machine, so making them conditional is a small change. New message types are never sent
to a client that didn't ask.

**Versioning is your call.** Stock pairs would exchange exactly the same bytes as today, so we read
the comment in `halo_port_limits.h` as allowing this without a bump. If you'd rather bump, a single
upstream bump that ships with the feature still ends the split, and we'd follow it.

**Left out of the upstream version:** our Dreamcast-specific detection (replaced by the flag), an
adaptive budget we tried and turned off, test hooks, and our hosting patches (fixed identity, game
port, latency logging). We can offer the last two separately if they're useful.

## Measurements

From our three modem servers, 8 to 10 November, one sample per 10 seconds, join phase excluded:

| Budget | Median | 90th percentile | State period |
|---|---:|---:|---|
| 2,000 B/s fixed | 1.3 to 1.9 KB/s | up to 3.7 KB/s | 3 to 10 ticks |
| 1,400 B/s fixed | 1.0 to 1.1 KB/s | 1.2 to 2.4 KB/s | 10 ticks |

At 1,400 B/s the controller sits at its 10-tick ceiling. It steps up on any second over budget and
only steps down below 60% of budget, so an upstream version should smooth over a longer window; we
think there's room to raise the update rate. The remaining spikes are reliable-stream bursts (creates
and scores), which still occasionally fill the tunnel bucket.

Shaped-line tests (Flycast's broadband adapter with a byte-rate shaper and 100 ms each way):

- Join at 3,360 B/s: 7.9 s to first settings. At 1,440 B/s: 20.7 s. Both admitted.
- Metering game writes alone wasn't enough: the wire stayed near 3.1 KB/s until the tunnel was paced too.

The servers now log per-player round trip, jitter and loss every 30 seconds; we'll add modem numbers
here as players come through.

## Suggested PR sequence

Each step is reviewable alone, and nothing changes for stock clients until the setting is on.

1. Cap KCP's retransmission wait, so a client that stalls during a map load doesn't back off to 60 s.
2. Hold a playerless machine's slot for 60 s from join (or make it a setting).
3. Resend settings only to the machine that missed them.
4. Optional wire log, off by default (useful for any lag report).
5. The capability flag, the setting, the player cap and the advertisement bit (no behaviour change yet).
6. Per-client pacing for flagged clients.
7. Compact reliable messages for flagged clients, with codec round-trip tests.
8. Compact datagram delta chains for flagged clients.

Steps 1 to 4 are general fixes that stand on their own.

## Testing

- **Codec unit tests** for every compact message, including the unchanged-delta case.
- **Stock identity:** with the setting off, or on with no client asking, a stock client's received
  bytes in a scripted match match `main` byte for byte. We can provide the harness.
- **Mixed match:** one flagged client, one stock client, same server; the same kills, scores,
  respawns and game end everywhere, and the stock client's traffic unchanged.
- **Shaped line without a Dreamcast:** a PC client with the flag behind `tc netem` at 3,360 and
  1,440 B/s; join within 60 s, respawn within 3 s under load, a clean match end, and a 15-minute soak.
- **Real hardware:** we'll run each step on Dreamcasts over modem and post the results.

## Code

Everything is public under the AGPL, the same license as OpenCE.

- Fork: <https://github.com/lamb2k/opence-dce-server>
- Modem branch: <https://github.com/lamb2k/opence-dce-server/tree/dce-compact-v42-adaptive>
- Diff against our OpenCE base: <https://github.com/lamb2k/opence-dce-server/compare/899219cc...dce-compact-v42-adaptive>
- Wire format notes: [`port/linux/NETCODE.md`](https://github.com/lamb2k/opence-dce-server/blob/dce-compact-v42-adaptive/port/linux/NETCODE.md)

The branch still merges cleanly into current `main`. We're happy to do the PRs ourselves in whatever
shape you prefer, or for you to take any part of it as is.
