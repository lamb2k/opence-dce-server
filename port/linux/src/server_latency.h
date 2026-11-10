/*
SERVER_LATENCY.H (Halo Dreamcast Edition server patch)

A connected player's link, for the server's latency log (a line per player
every 30 s while a game runs) and /api/status's players[].link: the
distributed netcode's round trip and its jitter (network_distributed.c, the
latency meter's), and for an internet play peer the tunnel's own ping, its
packets and the numbers that never came (p2p.c), and its KCP streams' round
trip and resends. Pure; tested by port/linux/tests/server_latency_test.c.
*/

#ifndef SERVER_LATENCY_H
#define SERVER_LATENCY_H

#include <stdio.h>

struct server_latency
{
	/* the netcode's smoothed round trip and jitter, ms; since last heard, ms; -1 unknown */
	long rtt, jitter, heard;
	/* an internet play peer (the tunnel's figures below are its) */
	int tunnel;
	/* the tunnel's last ping and its shortest, ms, -1 none answered */
	long tunnel_ping, tunnel_best;
	/* tunnel packets received, and numbers skipped that never came */
	unsigned long packets, missing;
	/* the peer's open KCP streams; the worst one's smoothed round trip and
	its variation, ms (-1 with none); their resends */
	int streams;
	long kcp_srtt, kcp_rttvar;
	unsigned long kcp_resends;
};

/* the packets missing, per mille of those expected */
static inline unsigned long server_latency_loss_permille(const struct server_latency *latency)
{
	unsigned long expected = latency->packets + latency->missing;

	return expected ? (latency->missing * 1000UL + expected / 2) / expected : 0;
}

static inline const char *server_latency_number(char *text, int size, long value)
{
	if (value < 0)
		snprintf(text, (size_t)size, "?");
	else
		snprintf(text, (size_t)size, "%ld", value);
	return text;
}

/* "slot 1 machine 2 \"Name\": rtt 85 ms, jitter 12 ms, heard 20 ms ago;
tunnel ping 80 ms (best 71), packets 1234, missing 3 (0.2%); kcp srtt 90 ms,
rttvar 10 ms, resends 2 (1 stream)"; the length written */
static inline int server_latency_line(char *text, int size, long slot, long machine, const char *name,
	const struct server_latency *latency)
{
	char a[16], b[16], c[16];
	int length;

	if (!text || size <= 0)
		return 0;
	length = snprintf(text, (size_t)size, "slot %ld machine %ld \"%s\": rtt %s ms, jitter %s ms, heard %s ms ago", slot,
		machine, name ? name : "", server_latency_number(a, sizeof(a), latency->rtt),
		server_latency_number(b, sizeof(b), latency->jitter), server_latency_number(c, sizeof(c), latency->heard));
	if (length < 0 || length >= size)
		return length < 0 ? 0 : size - 1;
	if (!latency->tunnel)
	{
		length += snprintf(text + length, (size_t)(size - length), "; not a tunnel peer");
	}
	else
	{
		unsigned long loss = server_latency_loss_permille(latency);

		length += snprintf(text + length, (size_t)(size - length),
			"; tunnel ping %s ms (best %s), packets %lu, missing %lu (%lu.%lu%%)",
			server_latency_number(a, sizeof(a), latency->tunnel_ping),
			server_latency_number(b, sizeof(b), latency->tunnel_best), latency->packets, latency->missing, loss / 10,
			loss % 10);
		if (length < size)
		{
			if (latency->streams > 0)
			{
				length += snprintf(text + length, (size_t)(size - length),
					"; kcp srtt %s ms, rttvar %s ms, resends %lu (%d stream%s)",
					server_latency_number(a, sizeof(a), latency->kcp_srtt),
					server_latency_number(b, sizeof(b), latency->kcp_rttvar), latency->kcp_resends, latency->streams,
					latency->streams == 1 ? "" : "s");
			}
			else
			{
				length += snprintf(text + length, (size_t)(size - length), "; kcp none open");
			}
		}
	}
	return length >= size ? size - 1 : length;
}

#endif
