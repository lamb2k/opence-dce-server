# Proposal: an opt-in "56k" server setting for 2 to 4 player servers

For OpenCE maintainers. Written like a PR description so it can be reviewed in one sitting.
Nothing here asks you to take our branch as is. It asks whether you would take a smaller,
negotiated version of it, split into the PRs in section 5.

Fork: <https://github.com/lamb2k/opence-dce-server> (AGPL-3.0, public).
Branch under review: [`dce-compact-v42-adaptive`](https://github.com/lamb2k/opence-dce-server/tree/dce-compact-v42-adaptive)
at [`c3806178`](https://github.com/lamb2k/opence-dce-server/commit/c3806178), based on OpenCE
[`899219cc`](https://github.com/lamb2k/opence-dce-server/commit/899219cc).
Full diff: [`899219cc...dce-compact-v42-adaptive`](https://github.com/lamb2k/opence-dce-server/compare/899219cc...dce-compact-v42-adaptive).

## 1. TL;DR

- **The line.** A Dreamcast on a 33.6k modem gets about 3,360 B/s raw and about 2,400 B/s usable
  downstream; on a bad call or at 14.4k, 1,400 B/s or less. Every packet pays about 59 B of
  tunnel, IP and UDP on top.
- **Stock OpenCE overruns it.** A broadband client gets state 30 times a second (about 7 KB/s).
  On a 3,360 B/s shaped line a stock host offered 93 KB in the first second of the join, 90 KB were
  dropped and the session died in about 2 s. In a match the measured wire to one modem was 2,417 B/s,
  with spawns and scores stuck behind 132 B object-change records on the one reliable stream.
- **What we built** (server side, per client, only for clients flagged as modem lines):
  - a wire byte budget per client (production: 1,400 B/s fixed) that sets its state period (2 to 10 ticks);
  - batching: one datagram per period, KCP pieces sent together every 100 ms;
  - a tunnel bucket and a slow-line KCP profile;
  - compact message forms: a 132 B object change becomes 5 B (delete) or about 28 B (create),
    and states go as per-machine delta chains;
  - join fixes so a slow line survives the 13 KB settings download.
- **Result.** In-match wire fell from about 2.4 KB/s (line full, resends) to a median of about 1.0 to
  1.1 KB/s at 3 updates a second. The owner's verdict on a real modem: "night and day".
- **The problem.** We bumped the network version 41 to 42 and send the compact forms to every client,
  so DCE and stock clients and servers no longer mix.
- **The ask.** An opt-in server setting, off by default, for servers capped at 2 to 4 players. A client
  asks for low bandwidth at join, and only clients that ask get the budget and the compact forms.
  Stock clients and network version 41 stay unchanged.

## 2. What is in our fork today

38 commits on top of `899219cc` (plus [`c3806178`](https://github.com/lamb2k/opence-dce-server/commit/c3806178)).
Sizes are added and removed lines summed over the group's commits, ignoring a CRLF churn of the
vendored KCP files (see the note under the table).

| # | Change | Why | Files (key lines) | Commits | 56k only? | Size |
|---|---|---|---|---|---|---|
| A1 | Per-client byte budget and state period (2 to 10 ticks, starting at 3), metered on every datagram written to that client | A modem cannot take 30 Hz state | `port/linux/game/network_distributed.c:696-905` (`distributed_slow_adapt`), `:3808-3835` (plan players); `network_objects.c:1506-1771`; `transport_endpoint_winsock.c` (write observer) | [3b97b0d4](https://github.com/lamb2k/opence-dce-server/commit/3b97b0d4), [d81aded5](https://github.com/lamb2k/opence-dce-server/commit/d81aded5), [a770b53c](https://github.com/lamb2k/opence-dce-server/commit/a770b53c), [77196eec](https://github.com/lamb2k/opence-dce-server/commit/77196eec), [666bd3b4](https://github.com/lamb2k/opence-dce-server/commit/666bd3b4), [ab589c5f](https://github.com/lamb2k/opence-dce-server/commit/ab589c5f) | 56k | +228 / -19 |
| A2 | Tunnel side: meter each peer's real wire bytes, a 2,400 B/s bucket per modem peer, slow-line KCP profile (window 8, 100 ms flush, RTO 1 to 3 s, no fast resend, dead link 60, 500 B pieces), stream pulls from the game only while the bucket has room, unthrottled outside match ticks | The game's own count missed the KCP stream and resends, half the bytes; turbo KCP sent each small message as its own packet | `port/linux/src/p2p.c:128-142`, `:950-1000`, `:2439`, `:2507-2529` (`stream_tune`), `:4351-4530` | [d8ed4d11](https://github.com/lamb2k/opence-dce-server/commit/d8ed4d11), [92c8d2b4](https://github.com/lamb2k/opence-dce-server/commit/92c8d2b4), [4a6c3561](https://github.com/lamb2k/opence-dce-server/commit/4a6c3561), [72faed0a](https://github.com/lamb2k/opence-dce-server/commit/72faed0a), [7dba7acd](https://github.com/lamb2k/opence-dce-server/commit/7dba7acd), [d41c34f6](https://github.com/lamb2k/opence-dce-server/commit/d41c34f6) | 56k | +253 / -25 |
| A3 | Compact message forms, network version 42: compact object changes (96) and game state diff (98), compact kills (97); unit states, object states and relayed actions as per-machine delta chains (99, 100, 101) with a resync bit in the input pad; modem objects paced behind units | 86% of reliable bytes were 132 B object-change records; a delete needs 5 B | new `port/linux/game/dce_compact.c` (1,450), `dce_compact.h` (201), `network_reliable.c` (783); `network_distributed.c`, `network_objects.c`; `network_server_message_handler.c:954-966`; `halo_port_limits.h:69`; `network_format.txt`; `NETCODE.md` | [d0c56a64](https://github.com/lamb2k/opence-dce-server/commit/d0c56a64), [14575b88](https://github.com/lamb2k/opence-dce-server/commit/14575b88), [d85fd019](https://github.com/lamb2k/opence-dce-server/commit/d85fd019), [54d7fe9d](https://github.com/lamb2k/opence-dce-server/commit/54d7fe9d), [9e974740](https://github.com/lamb2k/opence-dce-server/commit/9e974740), [269a3232](https://github.com/lamb2k/opence-dce-server/commit/269a3232) | 56k motive, but sent to **every** client today | +3,073 / -76 |
| A4 | Settings during join: a refused copy goes again only to the machine that missed it; a pregame retry waits while that machine's stream still has 2,400 B queued; start and new round never wait | A full resend of 4 x 3,594 B to every machine, every interval, buried a 1.7 KB/s line | `source/networking/network_server_message_handler.c:1133-1250` | [9e48d977](https://github.com/lamb2k/opence-dce-server/commit/9e48d977) (general), [27b921ff](https://github.com/lamb2k/opence-dce-server/commit/27b921ff), [137d846d](https://github.com/lamb2k/opence-dce-server/commit/137d846d) (56k) | Mixed | +83 / -31 |
| A5 | A machine with no player yet holds its slot 60 s from its join, not 15 s | Settings take about 8 s at 33.6k and about 18 to 21 s at 14.4k; the stock 15 s refused the console | `source/networking/network_server_manager.c:892-916` | [729a5af1](https://github.com/lamb2k/opence-dce-server/commit/729a5af1) | General | +23 / -4 |
| A6 | KCP: a cap on a stream's retransmission wait (`rx_maxrto`) | A console that stops reading during a map load pushed RTO to 60 s, one window a minute | `port/third_party/kcp/ikcp.c:280,571,1139`, `ikcp.h`; `p2p.c` | [889c45a5](https://github.com/lamb2k/opence-dce-server/commit/889c45a5) | General | +11 / -2 |
| A7 | Adaptive budget (`HALO_DCE_MODEM_ADAPT`), measured from KCP ack round trips | Fixed budgets were either laggy or over-careful | new `dce_modem_adapt.c/.h`, `port/third_party/net_budget/` (copied from our Dreamcast client), tests | [9d36151e](https://github.com/lamb2k/opence-dce-server/commit/9d36151e) | 56k, **off in production** | +911 / -0 |
| A8 | Wire log (`HALO_DCE_WIRE_LOG=1`): tunnel packets by kind, KCP resends, reliable messages by type, large writes, per-player state | How we found A1 to A3 | `p2p.c:938-1000,1419`; `network_distributed.c:1360`; `network_server_message_handler.c:720`; `network_reliable.c:123` | [f0180977](https://github.com/lamb2k/opence-dce-server/commit/f0180977), [d37f473f](https://github.com/lamb2k/opence-dce-server/commit/d37f473f), [12b2a115](https://github.com/lamb2k/opence-dce-server/commit/12b2a115), [4b4aab52](https://github.com/lamb2k/opence-dce-server/commit/4b4aab52), [972db753](https://github.com/lamb2k/opence-dce-server/commit/972db753), [793daffa](https://github.com/lamb2k/opence-dce-server/commit/793daffa) | General diagnostics, off by default | +214 / -7 |
| A9 | Test hooks: `HALO_DCE_TEST_KILL` (timed kills), `HALO_DCE_LEGACY_DGRAM` (wide datagrams for A/B); `damage_kill_object_for_player` accepts no killer | Respawn tests on a modem without a second player | `network_distributed.c:3036-3100`; `network_damage.c:416`; `source/objects/damage.c:2815-2830` | [30acd341](https://github.com/lamb2k/opence-dce-server/commit/30acd341), [e3e54416](https://github.com/lamb2k/opence-dce-server/commit/e3e54416), [8187b9d4](https://github.com/lamb2k/opence-dce-server/commit/8187b9d4), [14d0dc1b](https://github.com/lamb2k/opence-dce-server/commit/14d0dc1b), [b7ead1cc](https://github.com/lamb2k/opence-dce-server/commit/b7ead1cc) | Test only | +138 / -26 |
| B1 | Dreamcast glue: a client is a modem line if its build's mods name `dce-modem`, or if its tunnel streams offer a receive window of 8 or less with a round trip of 120 ms or more for 4 s | The console only sends `client_build` after its map loads, too late for the join | `network_distributed.c:726-733`; `p2p.c:4398-4450` (`p2p_peer_is_modem`) | in A1/A2 commits ([4a6c3561](https://github.com/lamb2k/opence-dce-server/commit/4a6c3561), [d41c34f6](https://github.com/lamb2k/opence-dce-server/commit/d41c34f6)) | Dreamcast only | (in A1/A2) |
| C1 | Fixed server identity: optional fixed key and invite token from the environment | Our listed servers keep one invite across restarts | `p2p.c:652-660,3481-3490`; `README-DCE.md` | [0800fdf2](https://github.com/lamb2k/opence-dce-server/commit/0800fdf2) | Ops | +52 / -2 |
| C2 | `network.game_port` / `HALO_NET_GAME_PORT`: more than one server per machine | Three modem servers on one VPS | `p2p.c`; `port_config.c:602`; `network_client_manager.c`, `network_server_manager.c`, `network_game_protocol.h` | [8ce65171](https://github.com/lamb2k/opence-dce-server/commit/8ce65171) | Ops, generally useful | +37 / -7 |
| C3 | Per-player `latency:` log line every 30 s and `players[].link` in `/api/status`; `client:` line with client kind | Measuring real lines | `server_status.c:174`; new `server_latency.h`, `server_client_kind.h`; tests | [5621ba7a](https://github.com/lamb2k/opence-dce-server/commit/5621ba7a), [c3806178](https://github.com/lamb2k/opence-dce-server/commit/c3806178) | Ops, log only | +475 / -2 |

Notes:

- C1 to C3 are the same patches as on our network-version-41 server
  ([`v41-build343-gameport`](https://github.com/lamb2k/opence-dce-server/tree/v41-build343-gameport):
  stock `build-343` `fc9deb0b` plus 4 commits). They are not part of this proposal.
- Commit `3b97b0d4` is titled "PARKED, not shipped", but the commits after it build on its code.
  That code is live.
- [`889c45a5`](https://github.com/lamb2k/opence-dce-server/commit/889c45a5) also rewrote the line endings of
  `ikcp.c` and `ikcp.h`. That is about 2,700 lines of churn for an 11-line change, and an upstream PR would
  not carry it.
- Totals against `899219cc`: 37 files, +5,384 / -87 ignoring CR, of which about 2,400 lines are the new
  compact codec files.
- The branch merges cleanly into today's OpenCE `main` (`26f77606`, checked with `git merge-tree`).

## 3. The problem with how we did it, and a proposed upstream shape

### What went wrong

- `HALO_PORT_NETWORK_VERSION` is 42 (`port/linux/include/halo_port_limits.h:69`). Every stock build
  refuses our servers at the join, and ours refuse stock hosts.
- The compact forms (A3) go to every client, PC included. That keeps one wire format, but it forces
  the bump.
- So the community now has two incompatible server pools, and we maintain a fork of the whole netcode.
  Our broadband server stays on version 41 (stock `build-343` plus ops patches) so it still mixes with stock clients.

### Proposed shape

**1. A server setting, off by default.** For example:

```toml
# Hosting: serve clients that ask for low bandwidth (modems, 33.6k and below).
# "auto": a client that asks at join gets a per-client byte budget and compact
#         messages; everyone else is unchanged. "off": never (stock behaviour).
network.low_bandwidth = "off"
# Wire bytes a second per low-bandwidth client (IP, UDP and tunnel included).
network.low_bandwidth_budget = 1400
# While low_bandwidth is "auto", the server takes at most this many players (2 to 4).
network.low_bandwidth_max_players = 4
```

- The server advertises the setting with one bit in the advertisement's flags byte (the reserved
  bytes already carry version, flags and kind), and in its internet listing. The browser can then
  show "56k OK".
- Why a 2 to 4 player cap: budget and period scale with players. We have 2-player measurements
  (section 4). 4 players is an estimate, about 1,100 B/s with compact forms, so the cap keeps the
  promise honest.

**2. A client capability flag, negotiated at join.**

- The flag must be known before the settings download, the first heavy phase.
  `_distributed_message_client_build` (92) is too late: it is sent after ready, and our console sends
  it only after its map has loaded. That is why we fell back to the window and round trip heuristic (B1).
- Two options, maintainers' choice:
  - one bit in the tunnel's join handshake (`p2p.c`), known before the game join;
  - one bit in a reserved byte of the game's join request.
- The server keeps a per-machine `low_bandwidth` flag. It is set only when the setting is `auto` and
  the client asked.
- An OpenCE PC or Android client could expose the same flag, e.g. `network.request_low_bandwidth`.
  That helps players on bad mobile links, and it gives you a test client without a Dreamcast (section 5).

**3. Per-client behaviour, only for flagged machines.**

| Path | Flagged client | Every other client |
|---|---|---|
| State period | Budget-driven, 2 to 10 ticks, all of its players and objects on tick 0 of the period (one datagram) | Unchanged |
| Reliable stream | Compact object changes and game-state diff (types 96, 98); non-unit objects paced behind units; settings retries wait for the queue | Unchanged wide messages |
| Datagrams | Compact unit/object states and relayed actions as delta chains (99, 100, 101); resync bit in the input pad | Unchanged types 2, 6, 15 |
| Tunnel/KCP | Slow-line profile, per-peer wire bucket, batched pieces, only during match ticks | Unchanged turbo profile |

- `network_reliable.c` already encodes per machine, and the delta chains are per machine, so making
  the compact path conditional on the flag is a small change to what exists.
- New message types (96 to 101) are sent only to clients that asked, so a stock client never sees one.

**4. The version question (yours to decide).**

- Our reading of the `halo_port_limits.h` comment ("raise it with any change to what the machines
  send each other") is that a negotiated, opt-in addition might not need a bump. Stock pairs exchange
  exactly the same bytes as today.
- If you prefer to bump anyway, one upstream bump that ships with the feature still ends the split:
  we would track your version instead of forking it.

**5. Dropped from the upstream version.**

- B1 Dreamcast glue: the `dce-modem` mods token and the window and RTT heuristic. The explicit flag
  replaces both.
- A7 adaptive budget. It is off in production: the owner's verdict on crossplay was that it "feels
  pretty bad". In the live log it also fell to its 600 B/s floor while the line carried about 1 KB/s
  (section 4). It also drags in `port/third_party/net_budget` from our client.
- A9 test hooks, unless you want a test-only build flag.
- C1 to C3 ops patches. We would offer C2 (game port) and C3 (latency line) separately if useful.

## 4. Measurements

Labels: **Live** = production journals of our three modem servers, clients the server flagged as modem lines
(real consoles; some Flycast tests in the same logs, not separated). **Flycast shaped** = Flycast's broadband
adapter with a byte-rate shaper and 100 ms each way, not PPP or real hardware. **Modelled/calculated** = arithmetic
or simulation, no wire.

### Before any of this (stock pacing)

| Scenario | Result | Kind |
|---|---|---|
| Stock host to a 3,360 B/s line, join | 93 KB offered in 1 s, 90 KB dropped, session lost in about 2 s | Flycast shaped |
| Same line, client window of 3 segments, join timeouts x4 | Joined, 0 drops, queue at most 3.5 KB | Flycast shaped |
| 2-player match, wire to one modem | 2,417 B/s: datagrams 1,327 + KCP push 921 + resend 105 + ack 64 | Live, wire log |
| Reliable stream mix | Object changes (type 5) were 86% of reliable bytes, 132 B each | Live, wire log |
| 2-player match, calculated, stock rate | 4,110 to 4,920 B/s down, 22% to 46% over 3,360 | Calculated |

### Our server, step by step (from the commit messages)

| Step | Result | Kind | Commit |
|---|---|---|---|
| Budget 2,400 (game bytes) | Line about 3.3 KB/s, 2 s queue | Flycast shaped, 3,360 B/s | [77196eec](https://github.com/lamb2k/opence-dce-server/commit/77196eec) |
| Budget 1,700 | Period settles at 8 to 9 ticks, about 1.8 KB/s | Flycast shaped, 3,360 B/s | [77196eec](https://github.com/lamb2k/opence-dce-server/commit/77196eec) |
| Game-level metering only | Wire stayed about 3.1 KB/s while game writes fell to 1.1 KB/s (KCP turbo: 5 to 7 packets a second, resend bursts to 5.5 KB) | Flycast shaped | [d8ed4d11](https://github.com/lamb2k/opence-dce-server/commit/d8ed4d11) |
| Settings at 14.4k | Each 14 KB copy about 8 s; copies queued and the client timed out | Flycast shaped | [27b921ff](https://github.com/lamb2k/opence-dce-server/commit/27b921ff) |
| Join with 60 s playerless hold | 3,360 B/s: 7.9 s join to first settings; 1,440 B/s: 20.7 s; both admitted | Flycast shaped, local and live server | [729a5af1](https://github.com/lamb2k/opence-dce-server/commit/729a5af1) |
| Same console, three nights | 1,355 to 1,474 B/s at period 4 to 5; KCP resends at 1,400 to 1,580 (budget 2,000); clean at 1,400 but 3 updates a second | Live | [9d36151e](https://github.com/lamb2k/opence-dce-server/commit/9d36151e) |

### Production journals, 2026-11-08 to 11-10

Each `dce-modem machine N sent X B/s, period P ticks (budget B)` row is one 1-second window, logged every
10 s (the adaptive rows are logged on each change instead). The first window of each match is the
unthrottled join and settings download (92 to 224 KB/s) and is left out.
"Held back" is the tunnel's "line is full" counter. It is logged at 1, 101, 201 and so on, so the figures
are lower bounds.

| Date (UTC) | Server | Budget | Windows | Median B/s | p90 | Max | Usual period | Held back by the 2,400 B/s bucket |
|---|---|---|---:|---:|---:|---:|---|---|
| 11-08 | Modem 1 | 2,000 fixed | 120 | 1,930 | 3,706 | 8,241 | 10, 9, 8 | 301+ |
| 11-09 | Modem 1 | 2,000 fixed | 109 | 1,326 | 1,675 | 2,675 | 5, 3, 7 | 201+ |
| 11-09 | Modem 2 | 2,000 fixed | 30 | 1,443 | 1,608 | 2,358 | 5, 4, 6 | 101+ |
| 11-09 | Modem 2 | adaptive from 1,400 | 49 | 1,040 | 1,396 | 2,218 | 10 | 1 |
| 11-09 | Modem 2 | 1,400 fixed | 30 | 991 | 1,186 | 2,087 | 10 | 0 |
| 11-10 | Modem 1 | 1,400 fixed | 30 | 1,046 | 2,442 | 2,730 | 10 | 201+ |
| 11-10 | Modem 2 | 1,400 fixed | 15 | 1,072 | 1,940 | 2,729 | 10 | 101+ |

How to read it:

- At 1,400 B/s the steady state is about 1.0 to 1.1 KB/s, a third of a 33.6k line, at 3 state updates a second.
- The spikes above budget are reliable-stream bursts (object creates, scores). They are what still trips the bucket.
- At 1,400 the fixed controller stays at its 10-tick ceiling. It jumps up on any second over budget and steps
  down only under 60% of budget (`network_distributed.c:880-889`). An upstream version should smooth over
  more than one second. We also think the budget has headroom to raise the update rate.
- In the adaptive run (under 2 minutes), the budget climbed from 1,400 to 2,000 in 7 s, then fell to its 600
  floor about 7 s later and stayed at 600 to 900. Ack round trips read up to 1.3 s while the line carried about
  0.8 to 1.2 KB/s. That is one reason it is off.
- Owner on real hardware (Dreamcast, modem, DreamPi): the fixed budget is "night and day" against stock
  pacing. Adaptive "feels pretty bad on crossplay".

### Latency lines (new, `5621ba7a`)

- The format is `latency: slot S machine M "NAME": rtt R ms, jitter J ms, heard H ms ago; tunnel ping P ms
  (best B), packets N, missing K (L%); kcp srtt S ms, rttvar V ms, resends X (1 stream)`.
- So far only probe clients have produced rows: rtt about 250 ms, 0% loss.
- No real modem session has run since the deploy, so we have no modem latency rows yet. We will add them
  here when we do.

## 5. Proposed PR split and testing plan

Each PR is reviewable on its own, and stock behaviour is unchanged until the setting is turned on.

1. **KCP retransmission cap** (A6, about 11 lines, no line-ending churn). General: helps any client that stalls on a map load.
2. **Playerless join hold** (A5, about 25 lines). 15 s idle becomes 60 s from the join, or a setting. General: any slow joiner.
3. **Settings resend only to the machine that missed it** (A4 part 1, `9e48d977`). General bug fix.
4. **Optional wire log** (A8 trimmed), off by default. Helps you debug any reported lag.
5. **Negotiation and setting.** The capability bit, `network.low_bandwidth*`, the player cap and the
   advertised flag. No behaviour change by itself.
6. **Per-client pacing for flagged clients** (A1, A2, A4 parts 2 to 3), with the Dreamcast heuristic replaced by the flag.
7. **Compact reliable messages for flagged clients** (A3 step 1: types 96 to 98) with the round-trip unit test.
8. **Compact datagram delta chains for flagged clients** (A3 step 2: types 99 to 101, resync bit).

Testing:

- **Unit tests.** Codec round trips (`dce_compact`, including the 3-byte unchanged delta). `network_format.txt`
  stays version 41, and the stock entries are unchanged.
- **Stock identity.** With the setting off, and with it on but no client asking, the wire bytes a stock
  client receives in a scripted match are identical to `main`'s. We can supply the harness.
- **Mixed lobby.** One flagged client, one stock PC client, one stock dedicated server, same match. Check the
  same kills, scores, respawns and game end on every client, and that the stock client's traffic is unchanged.
- **Shaped line, no Dreamcast needed.**
  - Setup: a PC client with `network.request_low_bandwidth` behind `tc netem` (3,360 and 1,440 B/s, 100 ms each way).
  - Gates: join within 60 s; a respawn within 3 s at the busiest moment; match end to the next lobby
    without a drop; 15-minute soak with no "line is full" growth.
- **Real hardware.** We run each step on Dreamcasts over modem and DreamPi, post the journals, and add the
  latency rows to this document.

## 6. Links and license

- Fork: <https://github.com/lamb2k/opence-dce-server> (AGPL-3.0 as upstream, public; every commit above is pushed).
- Modem branch: <https://github.com/lamb2k/opence-dce-server/tree/dce-compact-v42-adaptive>
- Version-41 server (stock `build-343` plus ops patches only): <https://github.com/lamb2k/opence-dce-server/tree/v41-build343-gameport>
- Diff against the OpenCE base: <https://github.com/lamb2k/opence-dce-server/compare/899219cc...dce-compact-v42-adaptive>
- Wire format notes: [`port/linux/NETCODE.md`](https://github.com/lamb2k/opence-dce-server/blob/dce-compact-v42-adaptive/port/linux/NETCODE.md) (version 42 section)
- Ops patches: [`README-DCE.md`](https://github.com/lamb2k/opence-dce-server/blob/dce-compact-v42-adaptive/README-DCE.md)
- License: everything here is already public under the AGPL. Take any part of it, as is or rewritten.
  We will do the split PRs ourselves against `main` if you say which shape you want.
