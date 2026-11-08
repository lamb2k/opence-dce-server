/*
NETWORK_RELIABLE.C

DCE network version 42: what the host sends its clients over their streams (port/linux/NETCODE.md,
NET-AUDIT.md). The distributed modules build their messages in the one wide form they always had
(network_objects.c, network_distributed.c, network_damage.c); here, on the way to each machine,
they become the compact forms (dce_compact.h) and, for a client on a slow line (a modem), go
out by priority:

- immediately: the players' units (a spawn is one record, whatever else waits), the game
  type's state, kills, pickups, notices, everything but what is below;
- paced to the line: the other objects (weapons, grenades, projectiles, bodies: whatever
  is not a unit) and the word that they have all been sent. A modem is given them only
  while what it has not yet had of the stream is under a fraction of a second of its line
  (network.dce_modem_wire), so the stream is never more than that behind and an immediate
  message waits for that at most.

The client side (a PC build, here; the Dreamcast reads the same bytes in src/net) turns a
compact message back into the wide one the modules already read.

A compact message begins with the context generation of the stream it belongs to. The host
begins a new one for a machine at each game and each full object send; a client that
sees another generation than it holds begins afresh, so the two ends always agree on
what the tables and the last game state hold.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "network_distributed.h"
#include "dce_compact.h"

#include <stdlib.h>
#include <string.h>

void platform_log(const char *format, ...);
boolean network_distributed_server_write_machine_reliably(long machine_index, void *message, word size);
unsigned long network_distributed_machine_address(long machine_index);
long p2p_unsent_bytes(unsigned long game_address);
long p2p_slow_wire_rate(void);

enum
{
	QUEUE_ITEMS = 1024,
	/* records of a modem's paced objects in one message */
	PACED_RECORDS_PER_MESSAGE = 10,
	/* the wire each message costs besides its bytes: its header, KCP's, the tunnel's and IP/UDP */
	MESSAGE_WIRE_OVERHEAD = 8 + 24 + 36
};

/* the wide object change, as network_objects.c has it (132 bytes) */
struct legacy_change
{
	byte change;
	byte flags;
	byte owner_player_index;
	byte forced_shader_permutation_index;
	long object_index;
	long definition_index;
	short owner_team_index;
	short variant_number;
	float position[3];
	float forward[3];
	float up[3];
	float translational_velocity[3];
	float angular_velocity[3];
	float change_colors[4][3];
	byte region_permutations[8];
};

typedef char legacy_change_size_assert[sizeof(struct legacy_change) == 132 ? 1 : -1];

struct queue_item
{
	byte marker; /* the word that the host's objects have all been sent (a header alone) */
	struct legacy_change change;
};

struct machine_state
{
	byte generation;
	struct dce_change_context change;
	byte state[DCE_STATE_MAX];
	size_t state_size;
	/* whether the last record of this object's index was a unit's (sent at once) */
	byte unit_slot[MAXIMUM_TRACKED_OBJECTS];
	struct queue_item *queue;
	long queue_head;
	long queue_count;
	/* the datagram records' chains (dce_compact.h), made with the first record */
	struct dce_unit_chain *units;
	struct dce_object_chain *objects;
	/* an object whose creation is in the queue */
	byte *pending;
	long last_resync;
	/* what went out lately, for the wire log */
	long sent_messages;
	long sent_bytes;
	long paced_released;
	long queue_peak;
};

static struct machine_state *machine_states[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
static long reliable_log_time;

/* the client's: what the host's compact messages built up */
static struct
{
	boolean valid;
	boolean failed;
	byte generation;
	struct dce_change_context change;
	byte state[DCE_STATE_MAX];
	size_t state_size;
	struct dce_unit_chain units[DCE_UNIT_CHAINS];
} reliable_client;
/* (the client's object chains: large, so made when first needed) */
static struct dce_object_chain *reliable_client_objects;

static boolean reliable_wire_log(void)
{
	static int on = -1;

	if (on < 0)
	{
		char const *value = getenv("HALO_DCE_WIRE_LOG");

		on = value && value[0] == '1';
	}
	return on;
}

/* a new stream of contexts: the next message to this machine is whole */
static void reliable_begin_stream(struct machine_state *state)
{
	state->generation = (byte)(state->generation + 1);
	if (state->generation == 0)
		state->generation = 1;
	dce_change_context_reset(&state->change);
	state->state_size = 0;
}

static int reliable_chains(struct machine_state *state);

static struct machine_state *reliable_machine(long machine_index, boolean create)
{
	struct machine_state *state;

	if (machine_index < 0 || machine_index >= HALO_PORT_MAXIMUM_NETWORK_MACHINES)
		return NULL;
	state = machine_states[machine_index];
	if (!state && create)
	{
		state = (struct machine_state *)calloc(1, sizeof(*state));
		if (!state)
			return NULL;
		machine_states[machine_index] = state;
		reliable_chains(state);
		reliable_begin_stream(state);
	}
	return state;
}

/* a game begins (or a client has loaded and asks for the host's objects all over): every machine's
tables and queue start empty, and so does this client's */
void network_reliable_new_game(void)
{
	long machine_index;

	for (machine_index = 0; machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES; machine_index++)
	{
		struct machine_state *state = machine_states[machine_index];

		if (state)
		{
			state->queue_head = 0;
			state->queue_count = 0;
			csmemset(state->unit_slot, 0, sizeof(state->unit_slot));
			if (state->units)
				csmemset(state->units, 0, DCE_UNIT_CHAINS * sizeof(*state->units));
			if (state->objects)
				csmemset(state->objects, 0, MAXIMUM_TRACKED_OBJECTS * sizeof(*state->objects));
			if (state->pending)
				csmemset(state->pending, 0, MAXIMUM_TRACKED_OBJECTS);
			reliable_begin_stream(state);
		}
	}
	csmemset(&reliable_client, 0, sizeof(reliable_client));
	if (reliable_client_objects)
		csmemset(reliable_client_objects, 0, MAXIMUM_TRACKED_OBJECTS * sizeof(*reliable_client_objects));
}

void network_reliable_machine_sync(
	long machine_index)
{
	struct machine_state *state = reliable_machine(machine_index, TRUE);

	if (state)
		reliable_begin_stream(state);
}

static boolean reliable_definition_is_unit(long definition_index)
{
	return definition_index != NONE && tag_index_is_group(definition_index, OBJECT_DEFINITION_TAG) &&
		TEST_FLAG(_object_mask_unit, object_definition_get(definition_index)->object.type);
}

/* ---------- writing */

struct wire_message
{
	struct distributed_message_header header;
	byte data[MAXIMUM_MESSAGE_SIZE];
};

static boolean reliable_write(
	long machine_index,
	struct machine_state *state,
	byte type,
	byte count,
	byte const *entries,
	word entries_size)
{
	struct wire_message message;
	word size = (word)(sizeof(message.header) + entries_size);
	boolean result;

	if (entries_size > sizeof(message.data))
		return FALSE;
	message.header.type = type;
	message.header.count = count;
	message.header.game_time = game_time_get();
	message.header.header = 0;
	build_message_header(&message.header.header, size, 2, 0);
	if (entries_size)
		csmemcpy(message.data, entries, entries_size);
	result = network_distributed_server_write_machine_reliably(machine_index, &message, size);
	if (state)
	{
		state->sent_messages++;
		state->sent_bytes += size;
		if (!result)
		{
			/* (what the tables hold on either end is no longer known) */
			reliable_begin_stream(state);
		}
	}
	return result;
}

/* the records, whole or paced, as one compact message to the machine */
static boolean reliable_write_changes(
	long machine_index,
	struct machine_state *state,
	struct legacy_change const *changes,
	short count)
{
	byte data[1 + 64 * DCE_CHANGE_MAX_BYTES];
	size_t used = 1;
	short index;

	data[0] = state->generation;
	for (index = 0; index < count; index++)
	{
		struct dce_change change;
		struct legacy_change const *source = &changes[index];
		size_t n;

		csmemset(&change, 0, sizeof(change));
		change.change = source->change;
		change.flags = source->flags & 7;
		change.owner_player = source->owner_player_index;
		change.shader = source->forced_shader_permutation_index;
		change.object_index = source->object_index;
		change.definition_index = source->definition_index;
		change.owner_team = source->owner_team_index;
		change.variant = source->variant_number;
		csmemcpy(change.position, source->position, sizeof(change.position));
		csmemcpy(change.forward, source->forward, sizeof(change.forward));
		csmemcpy(change.up, source->up, sizeof(change.up));
		csmemcpy(change.velocity, source->translational_velocity, sizeof(change.velocity));
		csmemcpy(change.angular_velocity, source->angular_velocity, sizeof(change.angular_velocity));
		csmemcpy(change.colors, source->change_colors, sizeof(change.colors));
		csmemcpy(change.regions, source->region_permutations, sizeof(change.regions));
		n = dce_change_encode(&state->change, &change, data + used, sizeof(data) - used);
		if (!n)
			return FALSE;
		used += n;
	}
	return reliable_write(machine_index, state, DCE_MESSAGE_OBJECT_CHANGES, (byte)count, data, (word)used);
}

static boolean reliable_write_marker(long machine_index, struct machine_state *state)
{
	return reliable_write(machine_index, state, _distributed_message_objects_synchronized, 0, NULL, 0);
}

/* what a machine has not yet had of its stream, and how much it is to be given at once */
static long reliable_hold_bytes(void)
{
	long hold = p2p_slow_wire_rate() * 4 / 10;

	return hold < 300 ? 300 : hold > 1500 ? 1500 : hold;
}

static boolean reliable_queue_push(struct machine_state *state, struct queue_item const *item)
{
	if (!state->queue)
	{
		state->queue = (struct queue_item *)calloc(QUEUE_ITEMS, sizeof(struct queue_item));
		if (!state->queue)
			return FALSE;
	}
	if (state->queue_count >= QUEUE_ITEMS)
		return FALSE;
	state->queue[(state->queue_head + state->queue_count) % QUEUE_ITEMS] = *item;
	state->queue_count++;
	if (!item->marker && item->change.change == 0 && state->pending)
	{
		long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(item->change.object_index);

		if (absolute_index >= 0 && absolute_index < MAXIMUM_TRACKED_OBJECTS)
			state->pending[absolute_index] = 1;
	}
	if (state->queue_count > state->queue_peak)
		state->queue_peak = state->queue_count;
	return TRUE;
}

/* gives a machine its paced items while its stream has room (limit < 0: all of them) */
static void reliable_release(long machine_index, struct machine_state *state, boolean everything)
{
	long unsent = 0;
	long released = 0;
	long hold = reliable_hold_bytes();

	if (!state->queue_count)
		return;
	if (!everything)
	{
		long behind = p2p_unsent_bytes(network_distributed_machine_address(machine_index));

		/* (not on the tunnel: no stream of ours to be behind on) */
		unsent = behind < 0 ? 0 : behind;
		if (behind < 0)
			everything = TRUE;
	}
	while (state->queue_count && (everything || unsent + released < hold))
	{
		struct queue_item *head = &state->queue[state->queue_head];
		struct legacy_change batch[PACED_RECORDS_PER_MESSAGE];
		short count = 0;

		if (head->marker)
		{
			reliable_write_marker(machine_index, state);
			released += MESSAGE_WIRE_OVERHEAD;
			state->queue_head = (state->queue_head + 1) % QUEUE_ITEMS;
			state->queue_count--;
			continue;
		}
		while (state->queue_count && count < PACED_RECORDS_PER_MESSAGE &&
			!state->queue[state->queue_head].marker)
		{
			batch[count] = state->queue[state->queue_head].change;
			if (state->pending && batch[count].change == 0)
			{
				long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(batch[count].object_index);

				if (absolute_index >= 0 && absolute_index < MAXIMUM_TRACKED_OBJECTS)
					state->pending[absolute_index] = 0;
			}
			count++;
			state->queue_head = (state->queue_head + 1) % QUEUE_ITEMS;
			state->queue_count--;
		}
		{
			long before = state->sent_bytes;

			reliable_write_changes(machine_index, state, batch, count);
			released += state->sent_bytes - before + MESSAGE_WIRE_OVERHEAD - 8;
			state->paced_released += count;
		}
	}
}

/* once a tick, after the tick's messages: what is paced goes on out */
void network_reliable_pump(void)
{
	long machine_index;
	boolean log = FALSE;
	long now = game_time_get();

	if (reliable_wire_log() && (now - reliable_log_time >= 30 || now < reliable_log_time))
	{
		reliable_log_time = now;
		log = TRUE;
	}
	for (machine_index = 0; machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES; machine_index++)
	{
		struct machine_state *state = machine_states[machine_index];

		if (!state)
			continue;
		if (state->queue_count)
			reliable_release(machine_index, state, !distributed_machine_slow_line(machine_index));
		if (log && (state->sent_messages || state->queue_peak))
		{
			platform_log("network: reliable to machine %ld: %ld messages %ld B, %ld paced records out, queue %ld now (peak %ld)",
				machine_index, state->sent_messages, state->sent_bytes, state->paced_released, state->queue_count,
				state->queue_peak);
			state->sent_messages = 0;
			state->sent_bytes = 0;
			state->paced_released = 0;
			state->queue_peak = state->queue_count;
		}
	}
}

static boolean reliable_send_changes(
	long machine_index,
	struct machine_state *state,
	byte const *entries,
	short count)
{
	struct legacy_change const *changes = (struct legacy_change const *)entries;
	struct legacy_change immediate[64];
	short immediate_count = 0;
	boolean slow = distributed_machine_slow_line(machine_index);
	boolean result = TRUE;
	short index;

	if (!slow && state->queue_count)
		reliable_release(machine_index, state, TRUE);
	for (index = 0; index < count; index++)
	{
		struct legacy_change const *change = &changes[index];
		long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(change->object_index);
		boolean unit = FALSE;

		/* (a made or deleted object's next state is a whole one) */
		if (state->objects && absolute_index >= 0 && absolute_index < MAXIMUM_TRACKED_OBJECTS)
			state->objects[absolute_index].valid = 0;
		if (absolute_index >= 0 && absolute_index < MAXIMUM_TRACKED_OBJECTS)
		{
			if (change->change == 0)
				state->unit_slot[absolute_index] = (byte)(unit = reliable_definition_is_unit(change->definition_index));
			else
				unit = state->unit_slot[absolute_index];
		}
		/* (a unit goes out at once; so does everything to a machine whose line is not slow) */
		if (!slow || unit)
			immediate[immediate_count++] = *change;
		else
		{
			struct queue_item item;

			item.marker = 0;
			item.change = *change;
			/* (the queue full: sent now, past the pacing) */
			if (!reliable_queue_push(state, &item))
				immediate[immediate_count++] = *change;
		}
	}
	if (immediate_count)
		result = reliable_write_changes(machine_index, state, immediate, immediate_count);
	if (slow && state->queue_count)
		reliable_release(machine_index, state, FALSE);
	return result;
}

static boolean reliable_send_state(
	long machine_index,
	struct machine_state *state,
	byte const *entries,
	word size)
{
	byte data[1 + 3 + DCE_STATE_MAX];
	size_t n;
	boolean result;

	if (size > DCE_STATE_MAX)
		return FALSE;
	data[0] = state->generation;
	n = dce_state_encode(state->state_size ? state->state : NULL, state->state_size, entries, size, data + 1,
		sizeof(data) - 1);
	if (!n)
		return FALSE;
	result = reliable_write(machine_index, state, DCE_MESSAGE_GAME_STATE, 0, data, (word)(1 + n));
	if (result)
	{
		csmemcpy(state->state, entries, size);
		state->state_size = size;
	}
	return result;
}

/* a message of the host's (its header filled in) to a machine in the game, over its stream */
boolean network_reliable_send(
	long machine_index,
	void *message,
	word size)
{
	struct distributed_message_header header;
	struct machine_state *state;
	byte const *entries;
	boolean slow;

	if (size < sizeof(header))
		return network_distributed_server_write_machine_reliably(machine_index, message, size);
	csmemcpy(&header, message, sizeof(header));
	entries = (byte const *)message + sizeof(header);
	state = reliable_machine(machine_index, TRUE);
	if (!state)
		return network_distributed_server_write_machine_reliably(machine_index, message, size);
	slow = distributed_machine_slow_line(machine_index);
	switch (header.type)
	{
	case _distributed_message_object_changes:
		if (header.count >= 1 && size == sizeof(header) + header.count * sizeof(struct legacy_change))
			return reliable_send_changes(machine_index, state, entries, header.count);
		break;
	case _distributed_message_game_state:
		if (size > sizeof(header))
			return reliable_send_state(machine_index, state, entries, (word)(size - sizeof(header)));
		break;
	case _distributed_message_objects_synchronized:
		/* (after the objects it says have come, paced as they are) */
		if (state->queue_count)
		{
			struct queue_item item;

			item.marker = 1;
			csmemset(&item.change, 0, sizeof(item.change));
			if (reliable_queue_push(state, &item))
			{
				reliable_release(machine_index, state, !slow);
				return TRUE;
			}
			reliable_release(machine_index, state, TRUE);
		}
		break;
	}
	return network_distributed_server_write_machine_reliably(machine_index, message, size);
}

/* ---------- reading: the client */

word network_reliable_expand(
	void const *message,
	word size,
	void *out,
	word capacity)
{
	struct distributed_message_header header;
	byte const *entries;
	word entries_size;
	struct distributed_message_header *out_header = (struct distributed_message_header *)out;
	byte *out_data = (byte *)out + sizeof(header);
	word out_size;

	if (size < sizeof(header) + 1)
		return 0;
	csmemcpy(&header, message, sizeof(header));
	entries = (byte const *)message + sizeof(header);
	entries_size = (word)(size - sizeof(header));
	if (!reliable_client.valid || reliable_client.generation != entries[0])
	{
		if (reliable_client.failed && reliable_client.generation == entries[0])
			return 0;
		/* (the datagram records' chains are their own: a delta that does not fit is dropped) */
		reliable_client.failed = FALSE;
		reliable_client.state_size = 0;
		dce_change_context_reset(&reliable_client.change);
		reliable_client.generation = entries[0];
		reliable_client.valid = TRUE;
	}
	if (reliable_client.failed)
		return 0;
	switch (header.type)
	{
	case DCE_MESSAGE_OBJECT_CHANGES:
	{
		struct legacy_change *changes = (struct legacy_change *)out_data;
		byte const *p = entries + 1;
		word left = (word)(entries_size - 1);
		short index;

		if (sizeof(header) + header.count * sizeof(struct legacy_change) > capacity)
			return 0;
		for (index = 0; index < header.count; index++)
		{
			struct dce_change change;
			size_t n = dce_change_decode(&reliable_client.change, p, left, &change);
			struct legacy_change *target = &changes[index];

			if (!n)
			{
				reliable_client.failed = TRUE;
				error(_error_silent, "compact object change #%d of %d malformed; the host's objects are not followed", index,
					header.count);
				return 0;
			}
			p += n;
			left = (word)(left - n);
			csmemset(target, 0, sizeof(*target));
			target->change = change.change;
			target->flags = change.flags;
			target->owner_player_index = change.owner_player;
			target->forced_shader_permutation_index = change.shader;
			target->object_index = change.object_index;
			target->definition_index = change.definition_index;
			target->owner_team_index = change.owner_team;
			target->variant_number = change.variant;
			csmemcpy(target->position, change.position, sizeof(target->position));
			csmemcpy(target->forward, change.forward, sizeof(target->forward));
			csmemcpy(target->up, change.up, sizeof(target->up));
			csmemcpy(target->translational_velocity, change.velocity, sizeof(target->translational_velocity));
			csmemcpy(target->angular_velocity, change.angular_velocity, sizeof(target->angular_velocity));
			csmemcpy(target->change_colors, change.colors, sizeof(target->change_colors));
			csmemcpy(target->region_permutations, change.regions, sizeof(target->region_permutations));
		}
		out_header->type = _distributed_message_object_changes;
		out_header->count = header.count;
		out_size = (word)(sizeof(header) + header.count * sizeof(struct legacy_change));
		break;
	}
	case DCE_MESSAGE_GAME_STATE:
		if (!dce_state_decode(reliable_client.state, &reliable_client.state_size, sizeof(reliable_client.state), entries + 1,
				(size_t)entries_size - 1) ||
			sizeof(header) + reliable_client.state_size > capacity)
		{
			reliable_client.failed = TRUE;
			error(_error_silent, "compact game state malformed; the host's game state is not followed");
			return 0;
		}
		csmemcpy(out_data, reliable_client.state, reliable_client.state_size);
		out_header->type = _distributed_message_game_state;
		out_header->count = 0;
		out_size = (word)(sizeof(header) + reliable_client.state_size);
		break;
	default:
		return 0;
	}
	out_header->game_time = header.game_time;
	out_header->header = 0;
	build_message_header(&out_header->header, out_size, 2, 0);
	return out_size;
}

/* ---------- the datagram records: per machine chains */

static int reliable_chains(struct machine_state *state)
{
	if (!state->units)
		state->units = (struct dce_unit_chain *)calloc(DCE_UNIT_CHAINS, sizeof(*state->units));
	if (!state->objects)
		state->objects = (struct dce_object_chain *)calloc(MAXIMUM_TRACKED_OBJECTS, sizeof(*state->objects));
	if (!state->pending)
		state->pending = (byte *)calloc(MAXIMUM_TRACKED_OBJECTS, 1);
	return state->units && state->objects && state->pending;
}

size_t network_reliable_unit_encode(
	long machine_index,
	struct dce_unit const *unit,
	byte *out,
	size_t capacity)
{
	struct machine_state *state = reliable_machine(machine_index, TRUE);

	if (!state || !reliable_chains(state) || unit->player >= DCE_UNIT_CHAINS)
		return 0;
	return dce_unit_encode(&state->units[unit->player], FALSE, unit, out, capacity);
}

size_t network_reliable_object_encode(
	long machine_index,
	struct dce_object const *object,
	byte *out,
	size_t capacity)
{
	struct machine_state *state = reliable_machine(machine_index, TRUE);
	long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX((long)object->object_index);

	if (!state || !reliable_chains(state) || absolute_index < 0 || absolute_index >= MAXIMUM_TRACKED_OBJECTS)
		return 0;
	return dce_object_encode(&state->objects[absolute_index], FALSE, object, out, capacity);
}

boolean network_reliable_object_pending(
	long machine_index,
	long object_index)
{
	struct machine_state *state = reliable_machine(machine_index, FALSE);
	long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);

	return state && state->pending && absolute_index >= 0 && absolute_index < MAXIMUM_TRACKED_OBJECTS &&
		state->pending[absolute_index];
}

void network_reliable_resync(
	long machine_index)
{
	struct machine_state *state = reliable_machine(machine_index, FALSE);

	long now = game_time_get();

	if (!state || (state->last_resync && now >= state->last_resync && now - state->last_resync < 15))
		return;
	state->last_resync = now ? now : 1;
	if (state->units)
		csmemset(state->units, 0, DCE_UNIT_CHAINS * sizeof(*state->units));
	if (state->objects)
		csmemset(state->objects, 0, MAXIMUM_TRACKED_OBJECTS * sizeof(*state->objects));
}

/* HALO_DCE_WIRE_LOG=1, a client: the records it took and dropped, logged every few seconds */
static long reliable_client_records[2];
static long reliable_client_dropped[2];
static long reliable_client_log_time;

static void reliable_client_note(int kind, int status)
{
	long now = game_time_get();

	reliable_client_records[kind]++;
	if (!status)
		reliable_client_dropped[kind]++;
	if (reliable_wire_log() && (now - reliable_client_log_time >= 150 || now < reliable_client_log_time))
	{
		platform_log("network: compact records from the host: %ld unit (%ld dropped), %ld object (%ld dropped)",
			reliable_client_records[0], reliable_client_dropped[0], reliable_client_records[1], reliable_client_dropped[1]);
		reliable_client_log_time = now;
		csmemset(reliable_client_records, 0, sizeof(reliable_client_records));
		csmemset(reliable_client_dropped, 0, sizeof(reliable_client_dropped));
	}
}

size_t network_reliable_unit_decode(
	byte const *in,
	size_t size,
	struct dce_unit *unit,
	int *status)
{
	size_t n = dce_unit_decode(reliable_client.units, DCE_UNIT_CHAINS, in, size, unit, status);

	if (n)
		reliable_client_note(0, *status);
	return n;
}

size_t network_reliable_object_decode(
	byte const *in,
	size_t size,
	struct dce_object *object,
	int *status)
{
	uint16_t absolute_index;

	if (!dce_object_peek(in, size, &absolute_index) || absolute_index >= MAXIMUM_TRACKED_OBJECTS)
		return 0;
	if (!reliable_client_objects)
		reliable_client_objects = (struct dce_object_chain *)calloc(MAXIMUM_TRACKED_OBJECTS, sizeof(*reliable_client_objects));
	if (!reliable_client_objects)
		return 0;
	{
		size_t n = dce_object_decode(&reliable_client_objects[absolute_index], in, size, object, status);

		if (n)
			reliable_client_note(1, *status);
		return n;
	}
}
