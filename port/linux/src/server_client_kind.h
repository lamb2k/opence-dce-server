/*
SERVER_CLIENT_KIND.H (Halo Dreamcast Edition server patch)

What kind of client a joined machine is, from what its join tells this host
(network_server_message_handler.c: message_client_join_game_request). The join
carries the machine's name, its token and its hardware id, and nothing that
names its platform: OpenCE's own builds (PC, Android, the web) send the hex of
their hardware id (p2p.c p2p_hardware_id), while the Halo Dreamcast Edition
client (dc-halo src/net/opence_session.c) sends 32 ASCII '0's, an id no hash
gives in practice. So: "Dreamcast" for that, "OpenCE" for any other id (its
platform not sent), "unknown" for none. Pure; tested by
port/linux/tests/server_client_kind_test.c.
*/

#ifndef SERVER_CLIENT_KIND_H
#define SERVER_CLIENT_KIND_H

#include <string.h>

static inline const char *server_client_kind(const char *hardware_id)
{
	size_t length, index;

	if (!hardware_id || !hardware_id[0])
		return "unknown";
	length = strlen(hardware_id);
	for (index = 0; index < length && hardware_id[index] == '0'; index++)
		;
	if (length == 32 && index == length)
		return "Dreamcast";
	return "OpenCE";
}

#endif
