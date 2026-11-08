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
