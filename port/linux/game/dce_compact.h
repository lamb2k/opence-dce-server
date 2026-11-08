/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
DCE_COMPACT.H

Network version 42: the compact wire forms of the host's reliable object and game-state
messages (the audit: NET-AUDIT.md, 2026-11-08). Pure C99, no game headers, no allocation:
one source for the dedicated server, the PC builds (which both encode and decode) and the
Dreamcast client (which decodes into its own typed structs; src/net/dce_compact.c is a copy).

All numbers little endian. A compact message keeps the distributed message header (the game
header word, type byte, count byte, tick) so the datagram batch and the reliable framing
stay as they were. Only the entries after it differ.

The reliable stream is ordered and lossless, so these forms hold state across messages on
that one connection, on both ends, and rebuild it at the start of each game:
- object changes: a table of the last colors and region permutations sent for each tag
  (an object of a tag whose look is the same sends none), and the last tag sent;
- game state: a byte diff against the last state sent.
*/
#ifndef DCE_COMPACT_H
#define DCE_COMPACT_H

#include <stddef.h>
#include <stdint.h>

#define DCE_COMPACT_NETWORK_VERSION 42

/* the compact message types (after the last legacy one, 95) */
enum
{
	DCE_MESSAGE_OBJECT_CHANGES = 96, /* reliable: dce_change records */
	DCE_MESSAGE_KILLS = 97,          /* reliable: dce_kill records */
	DCE_MESSAGE_GAME_STATE = 98,     /* reliable: a game state, whole or as a diff */
	DCE_MESSAGE_UNIT_STATES = 99,    /* datagram: dce_unit records, delta coded */
	DCE_MESSAGE_OBJECT_STATES = 100, /* datagram: dce_object records, delta coded */
	DCE_MESSAGE_COMPACT_LAST = 100
};

/* ---------- object changes (reliable) */

#define DCE_CHANGE_CACHE_ENTRIES 32
#define DCE_CHANGE_MAX_BYTES 60 /* the largest record: delete 5, create at most 60 */

struct dce_change
{
	uint8_t change; /* 0 create, 1 delete */
	uint8_t flags; /* bit 0 carried, bit 1 at rest, bit 2 dead (the legacy flag bits) */
	uint8_t owner_player; /* 0xFF none */
	uint8_t shader;
	int32_t object_index;
	int32_t definition_index;
	int16_t owner_team; /* -1 none */
	int16_t variant;
	float position[3];
	float forward[3];
	float up[3];
	float velocity[3];
	float angular_velocity[3];
	float colors[4][3];
	uint8_t regions[8];
};

struct dce_change_entry
{
	int32_t definition_index;
	uint8_t valid;
	uint8_t colors[12];
	uint8_t regions[8];
};

/* one connection's, one way: both ends reset it at the start of a game, and run every record
through it in the order of the stream */
struct dce_change_context
{
	struct dce_change_entry entries[DCE_CHANGE_CACHE_ENTRIES];
	int32_t previous_definition;
	uint8_t previous_valid;
};

void dce_change_context_reset(struct dce_change_context *context);
/* the bytes written (at most DCE_CHANGE_MAX_BYTES), 0 if cap is too small */
size_t dce_change_encode(struct dce_change_context *context, struct dce_change const *change, uint8_t *out, size_t cap);
/* the bytes read, 0 if the record is malformed or cut off */
size_t dce_change_decode(struct dce_change_context *context, uint8_t const *in, size_t size, struct dce_change *change);

/* ---------- game state (reliable) */

#define DCE_STATE_MAX 4096

/* a state as a message's entries: 0 = whole, 1 = diff against `previous` (the last state sent
or had on this connection; previous_size 0: none). Returns the bytes written, 0 if cap too small. */
size_t dce_state_encode(uint8_t const *previous, size_t previous_size, uint8_t const *state, size_t size, uint8_t *out,
	size_t cap);
/* applies the entries to `state` (the last state, `*size` bytes, `capacity` available), which
becomes the new one. Returns 1, or 0 for a malformed diff (state is then unspecified) */
int dce_state_decode(uint8_t *state, size_t *size, size_t capacity, uint8_t const *in, size_t in_size);

/* ---------- small helpers shared with the Dreamcast client */

size_t dce_put_varint(uint8_t *out, uint32_t value);
/* bytes read, 0 on error */
size_t dce_get_varint(uint8_t const *in, size_t size, uint32_t *value);

#endif
