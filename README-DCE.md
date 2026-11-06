# OpenCE with the Halo Dreamcast Edition server patch

This is [OpenCE](https://github.com/OpenCommunityEdition/OpenCE) at commit 899219cc, with one patch on top. It is the source of the Halo Dreamcast Edition official server, published to meet OpenCE's licence (see `LICENSE`).

The OpenCE authors did not write or review this patch, and do not support it.

## What changed, by the Halo Dreamcast Edition project, 2026-11-06

- **Fixed identity:** `HALO_DCE_HOST_SEED` keeps the server's key across restarts, and `HALO_DCE_INVITE_TOKEN` keeps its invite token. Unset, both stay random as in OpenCE. `port/linux/src/p2p.c`.

The history below that commit is OpenCE's, unchanged.

## Build

As for OpenCE, on Linux:

```
python3 configure.py --release --portable
ninja linux
```

## Run

Run it as an OpenCE server, with the two settings in its environment if you want a fixed identity. Generate your own values and keep them private: anyone with the seed can act as that server.
