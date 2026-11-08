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

/* ---------- datagram states and actions (step 2): what the host sends each tick, delta coded per
machine. A unit's or an object's record is a whole one (a key) or the difference from the last
record sent to that machine for it (a delta, naming that record's number), and the client holds
the last it took for each. A delta whose base the client does not have (a datagram lost, or an
object it has not made yet) is dropped, and the client asks for keys in its next input; the host
sends a key every DCE_KEY_INTERVAL records whatever it hears, so a loss costs a second at most.
Positions are 1/256 world unit, orientations the 4 byte form of the object changes. */

#define DCE_KEY_INTERVAL 10
#ifndef DCE_UNIT_CHAINS
#define DCE_UNIT_CHAINS 128 /* player indices the codec follows */
#endif

/* the legacy unit state's flag and part bits */
enum
{
	DCE_UNIT_ALIVE = 1,
	DCE_UNIT_PLACED = 2,
	DCE_UNIT_PREDICTED = 128,
	DCE_PART_RIDING = 1,
	DCE_PART_UP = 2,
	DCE_PART_DAMAGE = 4,
	DCE_PART_POWERUPS = 8,
	DCE_PART_CAMOUFLAGE = 16,
	DCE_PART_KILLER = 32
};

struct dce_unit
{
	uint8_t player, flags, parts, killer, unit_flags, camouflage;
	uint16_t unit_abs; /* the absolute index of the unit's object: the receiver has the full index */
	uint16_t vehicle_abs;
	int16_t seat;
	int32_t position[3]; /* 1/256 world unit */
	int16_t velocity[3], forward[3], up[3];
	int16_t predicted;
	uint16_t body, shield, damage[4];
	int16_t powerups[2];
};

struct dce_unit_chain
{
	uint8_t valid, seq, since_key;
	struct dce_unit state;
};

/* the bytes written (at most DCE_UNIT_MAX_BYTES) */
#define DCE_UNIT_MAX_BYTES 64
size_t dce_unit_encode(struct dce_unit_chain *chain, int force_key, struct dce_unit const *unit, uint8_t *out, size_t cap);
/* the bytes read, 0 if malformed. *status: 1 = `unit` is the state, 0 = a delta whose base is not held (dropped; ask for keys) */
size_t dce_unit_decode(struct dce_unit_chain *chains, size_t chain_count, uint8_t const *in, size_t size,
	struct dce_unit *unit, int *status);

struct dce_object
{
	uint16_t object_abs;
	uint8_t flags;
	int16_t time;
	int32_t position[3]; /* 1/256 world unit */
	uint32_t axes; /* forward and up in 4 bytes */
	int16_t velocity[3], angular_velocity[3];
};

struct dce_object_chain
{
	uint8_t valid, seq, since_key;
	struct dce_object state;
};

#define DCE_OBJECT_MAX_BYTES 40
size_t dce_object_encode(struct dce_object_chain *chain, int force_key, struct dce_object const *object, uint8_t *out,
	size_t cap);
/* the chain is looked up by the caller from the record's index: call dce_object_peek first */
int dce_object_peek(uint8_t const *in, size_t size, uint16_t *object_abs);
size_t dce_object_decode(struct dce_object_chain *chain, uint8_t const *in, size_t size, struct dce_object *object,
	int *status);

/* axes: forward and up (unit vectors, any length) as the 4 bytes, and back */
uint32_t dce_axes_pack(float const forward[3], float const up[3]);
void dce_axes_unpack(uint32_t axes, float forward[3], float up[3]);
int32_t dce_position_quantize(float value);
float dce_position_dequantize(int32_t value);

/* a relayed action (no chain: each is whole) */
struct dce_action
{
	uint8_t player;
	int8_t weapon, grenade, zoom, throttle_i, throttle_j;
	uint8_t trigger;
	int16_t yaw, pitch;
	uint16_t flags[4];
};

#define DCE_ACTION_MAX_BYTES 20
size_t dce_action_encode(struct dce_action const *action, uint8_t *out, size_t cap);
size_t dce_action_decode(uint8_t const *in, size_t size, struct dce_action *action);

#endif
