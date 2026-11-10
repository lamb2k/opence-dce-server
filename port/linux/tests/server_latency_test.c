/*
SERVER_LATENCY_TEST.C (Halo Dreamcast Edition server patch)

Tests of the server's latency line (port/linux/src/server_latency.h):
cc -std=c99 -Wall -Wextra -Werror port/linux/tests/server_latency_test.c && ./a.out; exits nonzero on a failure.
*/

#include "../src/server_latency.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			failures++; \
		} \
	} while (0)

int main(void)
{
	char text[512];
	struct server_latency latency = { 85, 12, 20, 1, 80, 71, 1234, 3, 1, 90, 10, 2 };
	struct server_latency unknown = { -1, -1, -1, 0, -1, -1, 0, 0, 0, -1, -1, 0 };
	int length;

	length = server_latency_line(text, sizeof(text), 1, 2, "LAMB2K", &latency);
	CHECK(!strcmp(text, "slot 1 machine 2 \"LAMB2K\": rtt 85 ms, jitter 12 ms, heard 20 ms ago; tunnel ping 80 ms "
		"(best 71), packets 1234, missing 3 (0.2%); kcp srtt 90 ms, rttvar 10 ms, resends 2 (1 stream)"));
	CHECK(length == (int)strlen(text));
	/* before anything is known, not a tunnel peer */
	server_latency_line(text, sizeof(text), 0, 1, "Player", &unknown);
	CHECK(!strcmp(text, "slot 0 machine 1 \"Player\": rtt ? ms, jitter ? ms, heard ? ms ago; not a tunnel peer"));
	/* a tunnel peer with no stream open, none answered */
	unknown.tunnel = 1;
	server_latency_line(text, sizeof(text), 3, 4, "BOT1", &unknown);
	CHECK(!strcmp(text, "slot 3 machine 4 \"BOT1\": rtt ? ms, jitter ? ms, heard ? ms ago; tunnel ping ? ms (best ?), "
		"packets 0, missing 0 (0.0%); kcp none open"));
	/* loss rounded per mille: 1 of 3 -> 33.3% */
	latency.packets = 2; latency.missing = 1; latency.streams = 2;
	CHECK(server_latency_loss_permille(&latency) == 333);
	server_latency_line(text, sizeof(text), 1, 2, "x", &latency);
	CHECK(strstr(text, "missing 1 (33.3%)") && strstr(text, "(2 streams)"));
	/* truncated, terminated, the length what was kept */
	length = server_latency_line(text, 24, 1, 2, "LAMB2K", &latency);
	CHECK(length == 23 && strlen(text) == 23);
	length = server_latency_line(text, 70, 1, 2, "LAMB2K", &latency);
	CHECK(length == 69 && strlen(text) == 69);
	CHECK(server_latency_line(NULL, 10, 0, 0, "", &latency) == 0);
	printf("server_latency: %d failure%s\n", failures, failures == 1 ? "" : "s");
	return failures != 0;
}
