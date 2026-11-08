/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* DCE_COMPACT.C: see dce_compact.h */
#include "dce_compact.h"

#include <math.h>
#include <string.h>

#define POSITION_SCALE 256.0f /* 1/256 world unit in 3 bytes: the whole +-32768 range */
#define VELOCITY_SCALE 1024.0f
#define ANGULAR_SCALE 4096.0f
#define QUAT_SCALE (511.0f * 1.41421356f)

enum
{
	HEAD_DELETE = 1,
	HEAD_CARRIED = 1 << 1,
	HEAD_AT_REST = 1 << 2,
	HEAD_DEAD = 1 << 3,
	HEAD_SAME_DEFINITION = 1 << 4,
	HEAD_OWNER = 1 << 5,
	HEAD_EXTRA = 1 << 6,
	HEAD_VELOCITY = 1 << 7,

	EXTRA_COLORS = 1 << 0,
	EXTRA_REGIONS = 1 << 1,
	EXTRA_SHADER = 1 << 2,
	EXTRA_ANGULAR = 1 << 3
};

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(uint8_t const *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(uint8_t const *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static int32_t clamp_round(float value, float scale, int32_t limit)
{
	float v = value * scale;

	/* (not a number: zero) */
	if (!(v - v == 0.0f))
		return 0;
	v = v > (float)limit ? (float)limit : v < -(float)limit ? -(float)limit : v;
	return (int32_t)floorf(v + 0.5f);
}

static void put24(uint8_t *p, int32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
}

static int32_t get24(uint8_t const *p)
{
	int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));

	return v >= 0x800000 ? v - 0x1000000 : v;
}

size_t dce_put_varint(uint8_t *out, uint32_t value)
{
	size_t n = 0;

	while (value >= 0x80)
	{
		out[n++] = (uint8_t)(value | 0x80);
		value >>= 7;
	}
	out[n++] = (uint8_t)value;
	return n;
}

size_t dce_get_varint(uint8_t const *in, size_t size, uint32_t *value)
{
	uint32_t v = 0;
	size_t n;

	for (n = 0; n < size && n < 5; n++)
	{
		v |= (uint32_t)(in[n] & 0x7F) << (7 * n);
		if (!(in[n] & 0x80))
		{
			*value = v;
			return n + 1;
		}
	}
	return 0;
}

/* ---------- orientation: forward and up as a smallest-three quaternion in 4 bytes */

static float vlength(float const v[3])
{
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static uint32_t axes_pack(float const forward_in[3], float const up_in[3])
{
	float f[3], u[3], l[3], m[3][3], q[4], s, dot;
	int index, i, k, a[3];
	uint32_t packed;
	float length = vlength(forward_in);

	if (!(length > 1e-6f) || !(length - length == 0.0f))
	{
		f[0] = 1.0f;
		f[1] = 0.0f;
		f[2] = 0.0f;
	}
	else
	{
		f[0] = forward_in[0] / length;
		f[1] = forward_in[1] / length;
		f[2] = forward_in[2] / length;
	}
	/* (up square to forward) */
	dot = up_in[0] * f[0] + up_in[1] * f[1] + up_in[2] * f[2];
	u[0] = up_in[0] - dot * f[0];
	u[1] = up_in[1] - dot * f[1];
	u[2] = up_in[2] - dot * f[2];
	length = vlength(u);
	if (!(length > 1e-6f) || !(length - length == 0.0f))
	{
		/* (any square to forward) */
		float other[3] = {0.0f, 0.0f, 1.0f};

		if (fabsf(f[2]) > 0.9f)
		{
			other[0] = 1.0f;
			other[2] = 0.0f;
		}
		dot = other[0] * f[0] + other[1] * f[1] + other[2] * f[2];
		u[0] = other[0] - dot * f[0];
		u[1] = other[1] - dot * f[1];
		u[2] = other[2] - dot * f[2];
		length = vlength(u);
	}
	u[0] /= length;
	u[1] /= length;
	u[2] /= length;
	l[0] = u[1] * f[2] - u[2] * f[1];
	l[1] = u[2] * f[0] - u[0] * f[2];
	l[2] = u[0] * f[1] - u[1] * f[0];
	/* columns forward, left, up */
	for (i = 0; i < 3; i++)
	{
		m[i][0] = f[i];
		m[i][1] = l[i];
		m[i][2] = u[i];
	}
	if (m[0][0] + m[1][1] + m[2][2] > 0.0f)
	{
		s = sqrtf(m[0][0] + m[1][1] + m[2][2] + 1.0f) * 2.0f;
		q[0] = s / 4.0f;
		q[1] = (m[2][1] - m[1][2]) / s;
		q[2] = (m[0][2] - m[2][0]) / s;
		q[3] = (m[1][0] - m[0][1]) / s;
	}
	else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
	{
		s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
		q[0] = (m[2][1] - m[1][2]) / s;
		q[1] = s / 4.0f;
		q[2] = (m[0][1] + m[1][0]) / s;
		q[3] = (m[0][2] + m[2][0]) / s;
	}
	else if (m[1][1] > m[2][2])
	{
		s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
		q[0] = (m[0][2] - m[2][0]) / s;
		q[1] = (m[0][1] + m[1][0]) / s;
		q[2] = s / 4.0f;
		q[3] = (m[1][2] + m[2][1]) / s;
	}
	else
	{
		s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
		q[0] = (m[1][0] - m[0][1]) / s;
		q[1] = (m[0][2] + m[2][0]) / s;
		q[2] = (m[1][2] + m[2][1]) / s;
		q[3] = s / 4.0f;
	}
	index = 0;
	for (i = 1; i < 4; i++)
	{
		if (fabsf(q[i]) > fabsf(q[index]))
			index = i;
	}
	if (q[index] < 0.0f)
	{
		for (i = 0; i < 4; i++)
			q[i] = -q[i];
	}
	for (i = 0, k = 0; i < 4; i++)
	{
		if (i != index)
			a[k++] = (int)clamp_round(q[i], QUAT_SCALE, 511) + 512;
	}
	packed = ((uint32_t)index << 30) | ((uint32_t)a[0] << 20) | ((uint32_t)a[1] << 10) | (uint32_t)a[2];
	return packed;
}

static void axes_unpack(uint32_t packed, float forward[3], float up[3])
{
	float q[4], sum = 0.0f, m[3][3];
	int index = (int)(packed >> 30), i, k;
	int a[3];

	a[0] = (int)((packed >> 20) & 1023) - 512;
	a[1] = (int)((packed >> 10) & 1023) - 512;
	a[2] = (int)(packed & 1023) - 512;
	for (i = 0, k = 0; i < 4; i++)
	{
		if (i == index)
			continue;
		q[i] = (float)a[k++] / QUAT_SCALE;
		sum += q[i] * q[i];
	}
	q[index] = sum < 1.0f ? sqrtf(1.0f - sum) : 0.0f;
	{
		float w = q[0], x = q[1], y = q[2], z = q[3];

		m[0][0] = 1.0f - 2.0f * (y * y + z * z);
		m[0][2] = 2.0f * (x * z + w * y);
		m[1][0] = 2.0f * (x * y + w * z);
		m[1][2] = 2.0f * (y * z - w * x);
		m[2][0] = 2.0f * (x * z - w * y);
		m[2][2] = 1.0f - 2.0f * (x * x + y * y);
	}
	forward[0] = m[0][0];
	forward[1] = m[1][0];
	forward[2] = m[2][0];
	up[0] = m[0][2];
	up[1] = m[1][2];
	up[2] = m[2][2];
}

/* ---------- object changes */

void dce_change_context_reset(struct dce_change_context *context)
{
	memset(context, 0, sizeof(*context));
}

static void quantize_colors(float const colors[4][3], uint8_t out[12])
{
	int i;

	for (i = 0; i < 12; i++)
		out[i] = (uint8_t)clamp_round(colors[i / 3][i % 3], 255.0f, 255);
}

size_t dce_change_encode(struct dce_change_context *context, struct dce_change const *c, uint8_t *out, size_t cap)
{
	uint8_t *p = out;
	uint8_t head = 0, extra = 0;
	uint8_t colors[12];
	struct dce_change_entry *entry;
	int32_t velocity[3], angular[3];
	int owner, i;

	if (c->change)
	{
		if (cap < 5)
			return 0;
		p[0] = HEAD_DELETE;
		put32(p + 1, (uint32_t)c->object_index);
		return 5;
	}
	if (cap < DCE_CHANGE_MAX_BYTES)
		return 0;
	head = (uint8_t)((c->flags & 7) << 1);
	if (context->previous_valid && context->previous_definition == c->definition_index)
		head |= HEAD_SAME_DEFINITION;
	owner = c->owner_player != 0xFF || c->owner_team != -1 || c->variant != 0;
	if (owner)
		head |= HEAD_OWNER;
	for (i = 0; i < 3; i++)
	{
		velocity[i] = clamp_round(c->velocity[i], VELOCITY_SCALE, 32767);
		angular[i] = clamp_round(c->angular_velocity[i], ANGULAR_SCALE, 32767);
	}
	if (velocity[0] || velocity[1] || velocity[2])
		head |= HEAD_VELOCITY;
	quantize_colors(c->colors, colors);
	entry = &context->entries[(uint32_t)c->definition_index % DCE_CHANGE_CACHE_ENTRIES];
	if (!entry->valid || entry->definition_index != c->definition_index)
	{
		memset(entry, 0, sizeof(*entry));
		entry->definition_index = c->definition_index;
		entry->valid = 1;
		extra |= EXTRA_COLORS | EXTRA_REGIONS;
	}
	else
	{
		if (memcmp(entry->colors, colors, 12) != 0)
			extra |= EXTRA_COLORS;
		if (memcmp(entry->regions, c->regions, 8) != 0)
			extra |= EXTRA_REGIONS;
	}
	if (c->shader)
		extra |= EXTRA_SHADER;
	if (angular[0] || angular[1] || angular[2])
		extra |= EXTRA_ANGULAR;
	if (extra)
		head |= HEAD_EXTRA;
	*p++ = head;
	put32(p, (uint32_t)c->object_index);
	p += 4;
	if (!(head & HEAD_SAME_DEFINITION))
	{
		put32(p, (uint32_t)c->definition_index);
		p += 4;
	}
	for (i = 0; i < 3; i++)
	{
		put24(p, clamp_round(c->position[i], POSITION_SCALE, 8388607));
		p += 3;
	}
	put32(p, axes_pack(c->forward, c->up));
	p += 4;
	if (head & HEAD_VELOCITY)
	{
		for (i = 0; i < 3; i++)
		{
			put16(p, (uint16_t)(int16_t)velocity[i]);
			p += 2;
		}
	}
	if (owner)
	{
		*p++ = c->owner_player;
		*p++ = (uint8_t)((c->owner_team >= -1 && c->owner_team < 254) ? c->owner_team + 1 : 0);
		put16(p, (uint16_t)c->variant);
		p += 2;
	}
	if (extra)
	{
		*p++ = extra;
		if (extra & EXTRA_COLORS)
		{
			memcpy(p, colors, 12);
			p += 12;
			memcpy(entry->colors, colors, 12);
		}
		if (extra & EXTRA_REGIONS)
		{
			memcpy(p, c->regions, 8);
			p += 8;
			memcpy(entry->regions, c->regions, 8);
		}
		if (extra & EXTRA_SHADER)
			*p++ = c->shader;
		if (extra & EXTRA_ANGULAR)
		{
			for (i = 0; i < 3; i++)
			{
				put16(p, (uint16_t)(int16_t)angular[i]);
				p += 2;
			}
		}
	}
	context->previous_definition = c->definition_index;
	context->previous_valid = 1;
	return (size_t)(p - out);
}

size_t dce_change_decode(struct dce_change_context *context, uint8_t const *in, size_t size, struct dce_change *c)
{
	uint8_t const *p = in;
	uint8_t const *end = in + size;
	uint8_t head, extra = 0;
	struct dce_change_entry *entry;
	int i;

	memset(c, 0, sizeof(*c));
	c->owner_player = 0xFF;
	c->owner_team = -1;
	if (size < 1)
		return 0;
	head = *p++;
	if (head & HEAD_DELETE)
	{
		if (head != HEAD_DELETE || end - p < 4)
			return 0;
		c->change = 1;
		c->object_index = (int32_t)get32(p);
		return 5;
	}
	if (end - p < 4)
		return 0;
	c->object_index = (int32_t)get32(p);
	p += 4;
	c->flags = (uint8_t)((head >> 1) & 7);
	if (head & HEAD_SAME_DEFINITION)
	{
		if (!context->previous_valid)
			return 0;
		c->definition_index = context->previous_definition;
	}
	else
	{
		if (end - p < 4)
			return 0;
		c->definition_index = (int32_t)get32(p);
		p += 4;
	}
	if (end - p < 13)
		return 0;
	for (i = 0; i < 3; i++)
	{
		c->position[i] = (float)get24(p) / POSITION_SCALE;
		p += 3;
	}
	axes_unpack(get32(p), c->forward, c->up);
	p += 4;
	if (head & HEAD_VELOCITY)
	{
		if (end - p < 6)
			return 0;
		for (i = 0; i < 3; i++)
		{
			c->velocity[i] = (float)(int16_t)get16(p) / VELOCITY_SCALE;
			p += 2;
		}
	}
	if (head & HEAD_OWNER)
	{
		if (end - p < 4)
			return 0;
		c->owner_player = p[0];
		c->owner_team = (int16_t)((int)p[1] - 1);
		c->variant = (int16_t)get16(p + 2);
		p += 4;
	}
	entry = &context->entries[(uint32_t)c->definition_index % DCE_CHANGE_CACHE_ENTRIES];
	if (!entry->valid || entry->definition_index != c->definition_index)
	{
		memset(entry, 0, sizeof(*entry));
		entry->definition_index = c->definition_index;
		entry->valid = 1;
		/* (the encoder sends both when it has none) */
		extra = 0;
		if (!(head & HEAD_EXTRA))
			return 0;
	}
	if (head & HEAD_EXTRA)
	{
		if (end - p < 1)
			return 0;
		extra = *p++;
		if (extra & EXTRA_COLORS)
		{
			if (end - p < 12)
				return 0;
			memcpy(entry->colors, p, 12);
			p += 12;
		}
		if (extra & EXTRA_REGIONS)
		{
			if (end - p < 8)
				return 0;
			memcpy(entry->regions, p, 8);
			p += 8;
		}
		if (extra & EXTRA_SHADER)
		{
			if (end - p < 1)
				return 0;
			c->shader = *p++;
		}
		if (extra & EXTRA_ANGULAR)
		{
			if (end - p < 6)
				return 0;
			for (i = 0; i < 3; i++)
			{
				c->angular_velocity[i] = (float)(int16_t)get16(p) / ANGULAR_SCALE;
				p += 2;
			}
		}
	}
	for (i = 0; i < 12; i++)
		c->colors[i / 3][i % 3] = (float)entry->colors[i] / 255.0f;
	memcpy(c->regions, entry->regions, 8);
	context->previous_definition = c->definition_index;
	context->previous_valid = 1;
	return (size_t)(p - in);
}

/* ---------- game state */

size_t dce_state_encode(uint8_t const *previous, size_t previous_size, uint8_t const *state, size_t size, uint8_t *out,
	size_t cap)
{
	size_t whole = 3 + size;
	size_t n, i, run_count = 0, written;
	uint8_t *p;

	if (size > 0xFFFF || cap < whole)
		return 0;
	if (previous && previous_size == size && size > 0)
	{
		/* a diff, if it is smaller than the whole */
		uint8_t scratch[DCE_STATE_MAX + 64];
		size_t last_end = 0;

		if (size > DCE_STATE_MAX)
			goto whole_state;
		p = scratch;
		i = 0;
		while (i < size)
		{
			size_t start, end;

			if (previous[i] == state[i])
			{
				i++;
				continue;
			}
			start = i;
			end = i + 1;
			/* (a run takes the equal bytes between differing ones when they are fewer than a new run's header) */
			for (;;)
			{
				int more = 0;

				for (n = end; n < size && n < end + 4; n++)
				{
					if (previous[n] != state[n])
					{
						end = n + 1;
						more = 1;
						break;
					}
				}
				if (!more)
					break;
			}
			p += dce_put_varint(p, (uint32_t)(start - last_end));
			p += dce_put_varint(p, (uint32_t)(end - start));
			memcpy(p, state + start, end - start);
			p += end - start;
			last_end = end;
			i = end;
			run_count++;
			if ((size_t)(p - scratch) > size)
				goto whole_state;
		}
		{
			uint8_t count_bytes[8];
			size_t count_size = dce_put_varint(count_bytes, (uint32_t)run_count);

			written = 3 + count_size + (size_t)(p - scratch);
			if (written < whole && cap >= written)
			{
				out[0] = 1;
				put16(out + 1, (uint16_t)size);
				memcpy(out + 3, count_bytes, count_size);
				memcpy(out + 3 + count_size, scratch, (size_t)(p - scratch));
				return written;
			}
		}
	}
whole_state:
	out[0] = 0;
	put16(out + 1, (uint16_t)size);
	memcpy(out + 3, state, size);
	return whole;
}

int dce_state_decode(uint8_t *state, size_t *size, size_t capacity, uint8_t const *in, size_t in_size)
{
	size_t new_size;
	uint8_t const *p = in;
	uint8_t const *end = in + in_size;

	if (in_size < 3)
		return 0;
	new_size = get16(in + 1);
	p += 3;
	if (new_size > capacity)
		return 0;
	if (in[0] == 0)
	{
		if ((size_t)(end - p) != new_size)
			return 0;
		memcpy(state, p, new_size);
		*size = new_size;
		return 1;
	}
	if (in[0] != 1 || *size != new_size)
		return 0;
	{
		uint32_t runs, gap, length;
		size_t used, offset = 0;

		used = dce_get_varint(p, (size_t)(end - p), &runs);
		if (!used)
			return 0;
		p += used;
		while (runs--)
		{
			used = dce_get_varint(p, (size_t)(end - p), &gap);
			if (!used)
				return 0;
			p += used;
			used = dce_get_varint(p, (size_t)(end - p), &length);
			if (!used)
				return 0;
			p += used;
			if ((size_t)gap > new_size - offset || (size_t)length > new_size - offset - gap ||
				(size_t)(end - p) < length)
				return 0;
			offset += gap;
			memcpy(state + offset, p, length);
			offset += length;
			p += length;
		}
		return p == end;
	}
}

/* ---------- datagram states and actions */

uint32_t dce_axes_pack(float const forward[3], float const up[3])
{
	return axes_pack(forward, up);
}

void dce_axes_unpack(uint32_t axes, float forward[3], float up[3])
{
	axes_unpack(axes, forward, up);
}

int32_t dce_position_quantize(float value)
{
	return clamp_round(value, POSITION_SCALE, 8388607);
}

float dce_position_dequantize(int32_t value)
{
	return (float)value / POSITION_SCALE;
}

static void put_s16(uint8_t *p, int16_t v)
{
	put16(p, (uint16_t)v);
}

static int16_t get_s16(uint8_t const *p)
{
	return (int16_t)get16(p);
}

/* unit record heads: the player (0..127), with 0x80 for a delta */
enum
{
	UNIT_DELTA = 0x80,
	UD_POSITION = 1 << 0,
	UD_VELOCITY = 1 << 1,
	UD_FORWARD = 1 << 2,
	UD_UP = 1 << 3,
	UD_VITALITY = 1 << 4,
	UD_TAIL = 1 << 5,
	UD_PREDICTED = 1 << 6
};

static size_t unit_tail_size(uint8_t parts)
{
	return (size_t)((parts & DCE_PART_DAMAGE ? 8 : 0) + (parts & DCE_PART_POWERUPS ? 4 : 0) +
		(parts & DCE_PART_CAMOUFLAGE ? 2 : 0));
}

static uint8_t *unit_put_tail(uint8_t *p, struct dce_unit const *u)
{
	int i;

	if (u->parts & DCE_PART_DAMAGE)
	{
		for (i = 0; i < 4; i++, p += 2)
			put16(p, u->damage[i]);
	}
	if (u->parts & DCE_PART_POWERUPS)
	{
		for (i = 0; i < 2; i++, p += 2)
			put_s16(p, u->powerups[i]);
	}
	if (u->parts & DCE_PART_CAMOUFLAGE)
	{
		p[0] = u->unit_flags;
		p[1] = u->camouflage;
		p += 2;
	}
	return p;
}

static uint8_t const *unit_get_tail(uint8_t const *p, struct dce_unit *u)
{
	int i;

	if (u->parts & DCE_PART_DAMAGE)
	{
		for (i = 0; i < 4; i++, p += 2)
			u->damage[i] = get16(p);
	}
	else
		memset(u->damage, 0, sizeof(u->damage));
	if (u->parts & DCE_PART_POWERUPS)
	{
		for (i = 0; i < 2; i++, p += 2)
			u->powerups[i] = get_s16(p);
	}
	else
		memset(u->powerups, 0, sizeof(u->powerups));
	if (u->parts & DCE_PART_CAMOUFLAGE)
	{
		u->unit_flags = p[0];
		u->camouflage = p[1];
		p += 2;
	}
	else
	{
		u->unit_flags = 0;
		u->camouflage = 0;
	}
	return p;
}

static int unit_delta_ok(struct dce_unit_chain const *chain, struct dce_unit const *u)
{
	struct dce_unit const *b = &chain->state;
	int i;

	if (!chain->valid || chain->since_key >= DCE_KEY_INTERVAL)
		return 0;
	if (!(b->flags & DCE_UNIT_ALIVE) || !(u->flags & DCE_UNIT_ALIVE) || b->unit_abs != u->unit_abs ||
		((b->flags ^ u->flags) & DCE_UNIT_PLACED) || ((b->parts ^ u->parts) & (DCE_PART_RIDING | DCE_PART_UP)) ||
		((u->parts & DCE_PART_RIDING) && (b->vehicle_abs != u->vehicle_abs || b->seat != u->seat)))
		return 0;
	for (i = 0; i < 3; i++)
	{
		int32_t d = u->position[i] - b->position[i];

		if (d > 32767 || d < -32767)
			return 0;
	}
	return 1;
}

size_t dce_unit_encode(struct dce_unit_chain *chain, int force_key, struct dce_unit const *u, uint8_t *out, size_t cap)
{
	uint8_t *p = out;
	int i;

	if (cap < DCE_UNIT_MAX_BYTES || u->player >= 128)
		return 0;
	if (!force_key && unit_delta_ok(chain, u))
	{
		struct dce_unit const *b = &chain->state;
		uint8_t mask = 0;
		uint8_t *mask_at;

		*p++ = (uint8_t)(u->player | UNIT_DELTA);
		*p++ = chain->seq; /* the base */
		mask_at = p++;
		if (u->flags & DCE_UNIT_PLACED)
		{
			if (memcmp(u->position, b->position, sizeof(u->position)) != 0)
			{
				mask |= UD_POSITION;
				for (i = 0; i < 3; i++, p += 2)
					put_s16(p, (int16_t)(u->position[i] - b->position[i]));
			}
			if (memcmp(u->velocity, b->velocity, sizeof(u->velocity)) != 0)
			{
				mask |= UD_VELOCITY;
				for (i = 0; i < 3; i++, p += 2)
					put_s16(p, u->velocity[i]);
			}
			if (memcmp(u->forward, b->forward, sizeof(u->forward)) != 0)
			{
				mask |= UD_FORWARD;
				for (i = 0; i < 3; i++, p += 2)
					put_s16(p, u->forward[i]);
			}
			if ((u->parts & DCE_PART_UP) && memcmp(u->up, b->up, sizeof(u->up)) != 0)
			{
				mask |= UD_UP;
				for (i = 0; i < 3; i++, p += 2)
					put_s16(p, u->up[i]);
			}
		}
		if (u->body != b->body || u->shield != b->shield)
		{
			mask |= UD_VITALITY;
			put16(p, u->body);
			put16(p + 2, u->shield);
			p += 4;
		}
		if (u->flags != b->flags || u->parts != b->parts || memcmp(u->damage, b->damage, sizeof(u->damage)) != 0 ||
			memcmp(u->powerups, b->powerups, sizeof(u->powerups)) != 0 || u->unit_flags != b->unit_flags ||
			u->camouflage != b->camouflage)
		{
			mask |= UD_TAIL;
			*p++ = u->flags;
			*p++ = u->parts;
			p = unit_put_tail(p, u);
		}
		if ((u->flags & DCE_UNIT_PREDICTED) && u->predicted != b->predicted)
		{
			mask |= UD_PREDICTED;
			put_s16(p, u->predicted);
			p += 2;
		}
		*mask_at = mask;
		chain->seq = (uint8_t)(chain->seq + 1);
		chain->since_key++;
		chain->state = *u;
		return (size_t)(p - out);
	}
	/* a key */
	*p++ = u->player;
	chain->seq = (uint8_t)(chain->seq + 1);
	*p++ = chain->seq;
	*p++ = u->flags;
	*p++ = u->parts;
	if (u->flags & DCE_UNIT_ALIVE)
	{
		put16(p, u->unit_abs);
		p += 2;
		if (u->parts & DCE_PART_RIDING)
		{
			put16(p, u->vehicle_abs);
			put_s16(p + 2, u->seat);
			p += 4;
		}
		if (u->flags & DCE_UNIT_PLACED)
		{
			for (i = 0; i < 3; i++, p += 3)
				put24(p, u->position[i]);
			for (i = 0; i < 3; i++, p += 2)
				put_s16(p, u->velocity[i]);
			for (i = 0; i < 3; i++, p += 2)
				put_s16(p, u->forward[i]);
			if (u->parts & DCE_PART_UP)
			{
				for (i = 0; i < 3; i++, p += 2)
					put_s16(p, u->up[i]);
			}
		}
		if (u->flags & DCE_UNIT_PREDICTED)
		{
			put_s16(p, u->predicted);
			p += 2;
		}
		put16(p, u->body);
		put16(p + 2, u->shield);
		p += 4;
		p = unit_put_tail(p, u);
	}
	else if (u->parts & DCE_PART_KILLER)
		*p++ = u->killer;
	chain->since_key = 0;
	chain->valid = 1;
	chain->state = *u;
	return (size_t)(p - out);
}

size_t dce_unit_decode(struct dce_unit_chain *chains, size_t chain_count, uint8_t const *in, size_t size,
	struct dce_unit *unit, int *status)
{
	uint8_t const *p = in;
	uint8_t const *end = in + size;
	uint8_t head;
	struct dce_unit_chain *chain;
	int i;

	if (size < 4)
		return 0;
	head = *p++;
	chain = (size_t)(head & 0x7F) < chain_count ? &chains[head & 0x7F] : NULL;
	if (head & UNIT_DELTA)
	{
		uint8_t base = *p++;
		uint8_t mask = *p++;
		struct dce_unit u;
		int usable = chain && chain->valid && chain->seq == base;

		if (usable)
			u = chain->state;
		else
			memset(&u, 0, sizeof(u));
		if (mask & UD_POSITION)
		{
			if (end - p < 6)
				return 0;
			for (i = 0; i < 3; i++, p += 2)
				u.position[i] += get_s16(p);
		}
		if (mask & UD_VELOCITY)
		{
			if (end - p < 6)
				return 0;
			for (i = 0; i < 3; i++, p += 2)
				u.velocity[i] = get_s16(p);
		}
		if (mask & UD_FORWARD)
		{
			if (end - p < 6)
				return 0;
			for (i = 0; i < 3; i++, p += 2)
				u.forward[i] = get_s16(p);
		}
		if (mask & UD_UP)
		{
			if (end - p < 6)
				return 0;
			for (i = 0; i < 3; i++, p += 2)
				u.up[i] = get_s16(p);
		}
		if (mask & UD_VITALITY)
		{
			if (end - p < 4)
				return 0;
			u.body = get16(p);
			u.shield = get16(p + 2);
			p += 4;
		}
		if (mask & UD_TAIL)
		{
			if (end - p < 2)
				return 0;
			u.flags = p[0];
			u.parts = p[1];
			p += 2;
			if ((size_t)(end - p) < unit_tail_size(u.parts))
				return 0;
			p = unit_get_tail(p, &u);
		}
		if (mask & UD_PREDICTED)
		{
			if (end - p < 2)
				return 0;
			u.predicted = get_s16(p);
			p += 2;
		}
		if (!usable)
		{
			*status = 0;
			return (size_t)(p - in);
		}
		u.player = head & 0x7F;
		chain->seq = (uint8_t)(base + 1);
		chain->since_key++;
		chain->state = u;
		*unit = u;
		*status = 1;
		return (size_t)(p - in);
	}
	{
		struct dce_unit u;
		uint8_t seq;

		memset(&u, 0, sizeof(u));
		u.player = head;
		u.killer = 255;
		u.seat = -1;
		seq = *p++;
		u.flags = *p++;
		u.parts = *p++;
		if (u.flags & DCE_UNIT_ALIVE)
		{
			if (end - p < 2)
				return 0;
			u.unit_abs = get16(p);
			p += 2;
			if (u.parts & DCE_PART_RIDING)
			{
				if (end - p < 4)
					return 0;
				u.vehicle_abs = get16(p);
				u.seat = get_s16(p + 2);
				p += 4;
			}
			if (u.flags & DCE_UNIT_PLACED)
			{
				size_t need = 9 + 6 + 6 + ((u.parts & DCE_PART_UP) ? 6 : 0);

				if ((size_t)(end - p) < need)
					return 0;
				for (i = 0; i < 3; i++, p += 3)
					u.position[i] = get24(p);
				for (i = 0; i < 3; i++, p += 2)
					u.velocity[i] = get_s16(p);
				for (i = 0; i < 3; i++, p += 2)
					u.forward[i] = get_s16(p);
				if (u.parts & DCE_PART_UP)
				{
					for (i = 0; i < 3; i++, p += 2)
						u.up[i] = get_s16(p);
				}
			}
			if (!(u.parts & DCE_PART_UP))
				u.up[2] = 32767;
			if (u.flags & DCE_UNIT_PREDICTED)
			{
				if (end - p < 2)
					return 0;
				u.predicted = get_s16(p);
				p += 2;
			}
			if (end - p < 4)
				return 0;
			u.body = get16(p);
			u.shield = get16(p + 2);
			p += 4;
			if ((size_t)(end - p) < unit_tail_size(u.parts))
				return 0;
			p = unit_get_tail(p, &u);
		}
		else if (u.parts & DCE_PART_KILLER)
		{
			if (end - p < 1)
				return 0;
			u.killer = *p++;
		}
		if (chain)
		{
			chain->valid = 1;
			chain->seq = seq;
			chain->since_key = 0;
			chain->state = u;
		}
		*unit = u;
		*status = chain ? 1 : 0;
		return (size_t)(p - in);
	}
}

/* object state records: the index with 0x8000 for a delta */
enum
{
	OBJECT_DELTA = 0x8000,
	OK_VELOCITY = 1 << 0,
	OK_ANGULAR = 1 << 1,
	OK_TIME = 1 << 2,
	OD_POSITION = 1 << 0,
	OD_AXES = 1 << 1,
	OD_VELOCITY = 1 << 2,
	OD_ANGULAR = 1 << 3,
	OD_FLAGS = 1 << 4,
	OD_TIME = 1 << 5
};

int dce_object_peek(uint8_t const *in, size_t size, uint16_t *object_abs)
{
	if (size < 2)
		return 0;
	*object_abs = (uint16_t)(get16(in) & 0x7FFF);
	return 1;
}

static int object_delta_ok(struct dce_object_chain const *chain, struct dce_object const *o)
{
	int i;

	if (!chain->valid || chain->since_key >= DCE_KEY_INTERVAL)
		return 0;
	for (i = 0; i < 3; i++)
	{
		int32_t d = o->position[i] - chain->state.position[i];

		if (d > 32767 || d < -32767)
			return 0;
	}
	return 1;
}

size_t dce_object_encode(struct dce_object_chain *chain, int force_key, struct dce_object const *o, uint8_t *out,
	size_t cap)
{
	uint8_t *p = out;
	int i;

	if (cap < DCE_OBJECT_MAX_BYTES || o->object_abs >= 0x8000)
		return 0;
	if (!force_key && object_delta_ok(chain, o))
	{
		struct dce_object const *b = &chain->state;
		uint8_t mask = 0;
		uint8_t *mask_at;

		put16(p, (uint16_t)(o->object_abs | OBJECT_DELTA));
		p += 2;
		*p++ = chain->seq;
		mask_at = p++;
		if (memcmp(o->position, b->position, sizeof(o->position)) != 0)
		{
			mask |= OD_POSITION;
			for (i = 0; i < 3; i++, p += 2)
				put_s16(p, (int16_t)(o->position[i] - b->position[i]));
		}
		if (o->axes != b->axes)
		{
			mask |= OD_AXES;
			put32(p, o->axes);
			p += 4;
		}
		if (memcmp(o->velocity, b->velocity, sizeof(o->velocity)) != 0)
		{
			mask |= OD_VELOCITY;
			for (i = 0; i < 3; i++, p += 2)
				put_s16(p, o->velocity[i]);
		}
		if (memcmp(o->angular_velocity, b->angular_velocity, sizeof(o->angular_velocity)) != 0)
		{
			mask |= OD_ANGULAR;
			for (i = 0; i < 3; i++, p += 2)
				put_s16(p, o->angular_velocity[i]);
		}
		if (o->flags != b->flags)
		{
			mask |= OD_FLAGS;
			*p++ = o->flags;
		}
		if (o->time != b->time)
		{
			mask |= OD_TIME;
			put_s16(p, o->time);
			p += 2;
		}
		*mask_at = mask;
		chain->seq = (uint8_t)(chain->seq + 1);
		chain->since_key++;
		chain->state = *o;
		return (size_t)(p - out);
	}
	{
		uint8_t kmask = 0;
		uint8_t *kmask_at;

		put16(p, o->object_abs);
		p += 2;
		chain->seq = (uint8_t)(chain->seq + 1);
		*p++ = chain->seq;
		kmask_at = p++;
		*p++ = o->flags;
		for (i = 0; i < 3; i++, p += 3)
			put24(p, o->position[i]);
		put32(p, o->axes);
		p += 4;
		if (o->velocity[0] || o->velocity[1] || o->velocity[2])
		{
			kmask |= OK_VELOCITY;
			for (i = 0; i < 3; i++, p += 2)
				put_s16(p, o->velocity[i]);
		}
		if (o->angular_velocity[0] || o->angular_velocity[1] || o->angular_velocity[2])
		{
			kmask |= OK_ANGULAR;
			for (i = 0; i < 3; i++, p += 2)
				put_s16(p, o->angular_velocity[i]);
		}
		if (o->time)
		{
			kmask |= OK_TIME;
			put_s16(p, o->time);
			p += 2;
		}
		*kmask_at = kmask;
		chain->since_key = 0;
		chain->valid = 1;
		chain->state = *o;
		return (size_t)(p - out);
	}
}

size_t dce_object_decode(struct dce_object_chain *chain, uint8_t const *in, size_t size, struct dce_object *object,
	int *status)
{
	uint8_t const *p = in;
	uint8_t const *end = in + size;
	uint16_t head;
	int i;

	if (size < 4)
		return 0;
	head = get16(p);
	p += 2;
	if (head & OBJECT_DELTA)
	{
		uint8_t base = *p++;
		uint8_t mask = *p++;
		struct dce_object o;
		int usable = chain && chain->valid && chain->seq == base;

		if (usable)
			o = chain->state;
		else
			memset(&o, 0, sizeof(o));
		if (mask & OD_POSITION)
		{
			if (end - p < 6)
				return 0;
			for (i = 0; i < 3; i++, p += 2)
				o.position[i] += get_s16(p);
		}
		if (mask & OD_AXES)
		{
			if (end - p < 4)
				return 0;
			o.axes = get32(p);
			p += 4;
		}
		if (mask & OD_VELOCITY)
		{
			if (end - p < 6)
				return 0;
			for (i = 0; i < 3; i++, p += 2)
				o.velocity[i] = get_s16(p);
		}
		if (mask & OD_ANGULAR)
		{
			if (end - p < 6)
				return 0;
			for (i = 0; i < 3; i++, p += 2)
				o.angular_velocity[i] = get_s16(p);
		}
		if (mask & OD_FLAGS)
		{
			if (end - p < 1)
				return 0;
			o.flags = *p++;
		}
		if (mask & OD_TIME)
		{
			if (end - p < 2)
				return 0;
			o.time = get_s16(p);
			p += 2;
		}
		if (!usable)
		{
			*status = 0;
			return (size_t)(p - in);
		}
		o.object_abs = (uint16_t)(head & 0x7FFF);
		chain->seq = (uint8_t)(base + 1);
		chain->since_key++;
		chain->state = o;
		*object = o;
		*status = 1;
		return (size_t)(p - in);
	}
	{
		struct dce_object o;
		uint8_t seq, kmask;

		memset(&o, 0, sizeof(o));
		if (end - p < 1 + 1 + 1 + 13)
			return 0;
		o.object_abs = head;
		seq = *p++;
		kmask = *p++;
		o.flags = *p++;
		for (i = 0; i < 3; i++, p += 3)
			o.position[i] = get24(p);
		o.axes = get32(p);
		p += 4;
		if (kmask & OK_VELOCITY)
		{
			if (end - p < 6)
				return 0;
			for (i = 0; i < 3; i++, p += 2)
				o.velocity[i] = get_s16(p);
		}
		if (kmask & OK_ANGULAR)
		{
			if (end - p < 6)
				return 0;
			for (i = 0; i < 3; i++, p += 2)
				o.angular_velocity[i] = get_s16(p);
		}
		if (kmask & OK_TIME)
		{
			if (end - p < 2)
				return 0;
			o.time = get_s16(p);
			p += 2;
		}
		if (chain)
		{
			chain->valid = 1;
			chain->seq = seq;
			chain->since_key = 0;
			chain->state = o;
		}
		*object = o;
		*status = 1;
		return (size_t)(p - in);
	}
}

/* relayed actions */
enum
{
	AM_FLAGS = 1 << 0,
	AM_THROTTLE = 1 << 1,
	AM_TRIGGER = 1 << 2,
	AM_CHOICES = 1 << 3,
	AM_OLDER = 4 /* bits 4 to 6: the older ticks' buttons that differ from the tick after */
};

size_t dce_action_encode(struct dce_action const *a, uint8_t *out, size_t cap)
{
	uint8_t *p = out;
	uint8_t mask = 0;
	uint8_t *mask_at;
	int h;

	if (cap < DCE_ACTION_MAX_BYTES || a->player >= 128)
		return 0;
	*p++ = a->player;
	mask_at = p++;
	put_s16(p, a->yaw);
	put_s16(p + 2, a->pitch);
	p += 4;
	if (a->flags[0])
	{
		mask |= AM_FLAGS;
		put16(p, a->flags[0]);
		p += 2;
	}
	if (a->throttle_i || a->throttle_j)
	{
		mask |= AM_THROTTLE;
		*p++ = (uint8_t)a->throttle_i;
		*p++ = (uint8_t)a->throttle_j;
	}
	if (a->trigger)
	{
		mask |= AM_TRIGGER;
		*p++ = a->trigger;
	}
	if (a->weapon != -1 || a->grenade != -1 || a->zoom != -1)
	{
		mask |= AM_CHOICES;
		*p++ = (uint8_t)a->weapon;
		*p++ = (uint8_t)a->grenade;
		*p++ = (uint8_t)a->zoom;
	}
	for (h = 1; h < 4; h++)
	{
		if (a->flags[h] != a->flags[h - 1])
		{
			mask |= (uint8_t)(1 << (AM_OLDER + h - 1));
			put16(p, a->flags[h]);
			p += 2;
		}
	}
	*mask_at = mask;
	return (size_t)(p - out);
}

size_t dce_action_decode(uint8_t const *in, size_t size, struct dce_action *a)
{
	uint8_t const *p = in;
	uint8_t const *end = in + size;
	uint8_t mask;
	int h;

	memset(a, 0, sizeof(*a));
	a->weapon = a->grenade = a->zoom = -1;
	if (size < 6)
		return 0;
	a->player = *p++;
	mask = *p++;
	a->yaw = get_s16(p);
	a->pitch = get_s16(p + 2);
	p += 4;
	if (mask & AM_FLAGS)
	{
		if (end - p < 2)
			return 0;
		a->flags[0] = get16(p);
		p += 2;
	}
	if (mask & AM_THROTTLE)
	{
		if (end - p < 2)
			return 0;
		a->throttle_i = (int8_t)p[0];
		a->throttle_j = (int8_t)p[1];
		p += 2;
	}
	if (mask & AM_TRIGGER)
	{
		if (end - p < 1)
			return 0;
		a->trigger = *p++;
	}
	if (mask & AM_CHOICES)
	{
		if (end - p < 3)
			return 0;
		a->weapon = (int8_t)p[0];
		a->grenade = (int8_t)p[1];
		a->zoom = (int8_t)p[2];
		p += 3;
	}
	for (h = 1; h < 4; h++)
	{
		a->flags[h] = a->flags[h - 1];
		if (mask & (1 << (AM_OLDER + h - 1)))
		{
			if (end - p < 2)
				return 0;
			a->flags[h] = get16(p);
			p += 2;
		}
	}
	return (size_t)(p - in);
}
