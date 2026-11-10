/*
SERVER_CLIENT_KIND_TEST.C (Halo Dreamcast Edition server patch)

Tests of the client kind (port/linux/src/server_client_kind.h):
cc -std=c99 -Wall -Wextra -Werror port/linux/tests/server_client_kind_test.c && ./a.out; exits nonzero on a failure.
*/

#include "../src/server_client_kind.h"

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
	/* the Dreamcast client's join: 32 ASCII zeros */
	CHECK(!strcmp(server_client_kind("00000000000000000000000000000000"), "Dreamcast"));
	/* an OpenCE build's hashed id */
	CHECK(!strcmp(server_client_kind("9f3a01c4b2e85d7760aa13fe0c4d9b21"), "OpenCE"));
	/* zeros of another length, or zeros then a digit, are not the Dreamcast's */
	CHECK(!strcmp(server_client_kind("0000000000000000"), "OpenCE"));
	CHECK(!strcmp(server_client_kind("00000000000000000000000000000001"), "OpenCE"));
	/* none */
	CHECK(!strcmp(server_client_kind(""), "unknown"));
	CHECK(!strcmp(server_client_kind(NULL), "unknown"));
	printf("server_client_kind: %d failure%s\n", failures, failures == 1 ? "" : "s");
	return failures != 0;
}
