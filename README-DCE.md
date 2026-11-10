# OpenCE with the Halo Dreamcast Edition server patch

This is [OpenCE](https://github.com/OpenCommunityEdition/OpenCE) at commit 899219cc, with the Halo Dreamcast Edition server patches on top. It is the source of the Halo Dreamcast Edition official server, published to meet OpenCE's licence (see `LICENSE`).

The OpenCE authors did not write or review this patch, and do not support it.

## What changed, by the Halo Dreamcast Edition project, 2026-11-06

- **Fixed identity:** `HALO_DCE_HOST_SEED` keeps the server's key across restarts, and `HALO_DCE_INVITE_TOKEN` keeps its invite token. Unset, both stay random as in OpenCE. `port/linux/src/p2p.c`.
- **Per-player latency:** a `latency` log line per player every 30 s while a game runs, and `players[].link` in `/api/status`. See below.

The history below that commit is OpenCE's, unchanged.

## Build

As for OpenCE, on Linux:

```
python3 configure.py --release --portable
ninja linux
```

## Run

Run it as an OpenCE server, with the two settings in its environment if you want a fixed identity. Generate your own values and keep them private: anyone with the seed can act as that server.

## Per-player latency (Halo Dreamcast Edition server patch)

While a game runs, the server logs one `latency` line per connected player every 30 s:

    INFO latency: slot 1 machine 2 "Name": rtt 85 ms, jitter 12 ms, heard 20 ms ago; tunnel ping 80 ms (best 71),
    packets 1234, missing 3 (0.2%); kcp srtt 90 ms, rttvar 10 ms, resends 2 (1 stream)

- `rtt`, `jitter`: the netcode's own round trip (the latency meter's, network_distributed.c), smoothed; jitter is its
  smoothed deviation, measured in ticks (33 ms steps).
- `tunnel ping`: the internet play tunnel's ping (one a second, p2p.c). `packets` / `missing`: tunnel packets received
  and numbers skipped that never came (loss).
- `kcp`: the peer's open reliable streams (the game's TCP connections): the worst one's smoothed round trip and
  variation, and their resends.

The same fields are in `/api/status` (and the status file) as `players[].link`. Code: port/linux/src/server_latency.h
(tested by port/linux/tests/server_latency_test.c), p2p.c, network_distributed.c, server_status.c.
