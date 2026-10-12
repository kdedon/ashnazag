#include <sys/types.h>
#include <sys/time.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include "nativehost.h"
#include "lisa_hw.h"
#include "lisa_wire.h"
#include "lisa_keys.h"

extern int gettimeofday();

static long
signed32(n)
unsigned long n;
{
	return (long)(int)n;
}

static long
word(n)
unsigned long n;
{
	return (long)(short)n;
}

int
lisa_hw_clock(ms, us)
unsigned long *ms, *us;
{
	struct nh_clock clock;
	unsigned long hz, seconds, fraction;

	if (nh_clock_read(&clock) < 0)
		return -1;
	hz = clock.frequency;
	if (!hz || hz > 65535) {
		errno = EINVAL;
		return -1;
	}
	seconds = clock.ticks / hz;
	fraction = clock.ticks % hz;
	if (ms)
		*ms = (seconds * 1000 + fraction * 1000 / hz) & 0xffffffffUL;
	if (us)
		*us = (seconds * 1000000 + fraction * (1000000 / hz) +
			fraction * (1000000 % hz) / hz) & 0xffffffffUL;
	return 0;
}

static int
due(now, deadline)
unsigned long now, deadline;
{
	return signed32((now - deadline) & 0xffffffffUL) >= 0;
}

static void
cursor(h)
struct lisa_hw *h;
{
	unsigned char *p, *image;

	p = h->surface + LISA_CURSOR;
	image = h->flags & 4 ? h->busy : h->normal;
	memcpy(p, image, 160);
	lisa_put32(p, 1);
	lisa_put32(p + 16, h->x);
	lisa_put32(p + 20, h->y);
	lisa_put32(p + 24, h->depth);
	lisa_put32(p + 28, h->flags);
	p = h->surface + LISA_BUSY;
	memcpy(p, h->busy, 160);
	lisa_put32(p, 1);
	lisa_put32(p + 16, h->x);
	lisa_put32(p + 20, h->y);
	lisa_put32(p + 24, h->depth);
	lisa_put32(p + 28, h->flags);
	lisa_barrier();
}

int
lisa_hw_init(h, surface, validate, opaque)
struct lisa_hw *h;
unsigned char *surface;
void *(*validate)(void *, unsigned long, unsigned long, int);
void *opaque;
{
	struct timeval wall;
	unsigned long now;
	unsigned char *p;

	if (!h || !surface || !validate) {
		errno = EINVAL;
		return -1;
	}
	if (gettimeofday(&wall, 0) < 0 || lisa_hw_clock(&now, 0) < 0)
		return -1;
	memset(h, 0, sizeof *h);
	h->surface = surface;
	h->validate = validate;
	h->opaque = opaque;
	h->depth = 1;
	h->repeat_initial = 400;
	h->repeat_next = 150;
	h->motion_due = now + 16;
	h->wall_offset = ((unsigned long)wall.tv_sec * 1000 + wall.tv_usec / 1000 - now) & 0xffffffffUL;
	p = surface + LISA_POLICY;
	memset(p, 0, 20);
	lisa_put32(p, 0x4c495350);
	lisa_put32(p + 4, 1);
	lisa_put32(p + 12, 8);
	lisa_put32(p + 16, 16);
	p = surface + LISA_KEYBOARD;
	memset(p, 0, 36);
	lisa_put32(p, 0x4c4b4244);
	lisa_put32(p + 4, 1);
	lisa_put32(p + 8, 1);
	p = surface + LISA_INPUT;
	memset(p, 0, 1040);
	lisa_put32(p, 0x4c494e50);
	lisa_put32(p + 4, 1);
	p = surface + LISA_AUDIO;
	memset(p, 0, 36);
	lisa_put32(p, 0x4c415544);
	lisa_put32(p + 4, 2);
	lisa_put32(p + 8, 4);
	cursor(h);
	return 0;
}

static void
motion(h, now)
struct lisa_hw *h;
unsigned long now;
{
	unsigned char *p;
	long dx, dy, total, n;

	p = h->surface + LISA_POLICY;
	dx = h->dx;
	dy = h->dy;
	if (dx || dy) {
		if (lisa_get32(p + 8)) {
			total = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
			if ((unsigned long)total <= lisa_get32(p + 12)) {
				n = 5 * dy + 2;
				dy = (n + (n >= 0 ? 3 : 0)) >> 3;
			} else {
				n = 3 * dx;
				dx = (n + (n >= 0 ? 1 : 0)) >> 1;
			}
		}
		h->x += dx;
		h->y += dy;
		if (h->x < 0) h->x = 0;
		if (h->x > 719) h->x = 719;
		if (h->y < 0) h->y = 0;
		if (h->y > 363) h->y = 363;
		h->flags &= ~1UL;
		if (!h->depth)
			h->flags |= 2;
		h->dx = h->dy = 0;
		cursor(h);
	}
	h->motion_due = (now + lisa_get32(p + 16)) & 0xffffffffUL;
}

static int
enqueue(h, key, ascii, time)
struct lisa_hw *h;
unsigned int key, ascii;
unsigned long time;
{
	struct lisa_low_event *e;

	if (h->count == 64) {
		errno = ENOSPC;
		return -1;
	}
	e = &h->events[(h->head + h->count) % 64];
	e->key = key;
	e->ascii = ascii;
	e->state = h->state;
	e->x = h->x;
	e->y = h->y;
	e->time = time;
	h->count++;
	return 0;
}

static int
key_event(h, native, down, time)
struct lisa_hw *h;
unsigned int native, down;
unsigned long time;
{
	unsigned int key, mask, ascii, table;
	unsigned long modifier = 0;

	if (down)
		h->repeat_valid = 0;
	key = lisa_adb[native];
	if (key == 255)
		return 0;
	mask = 1 << (key % 8);
	if (down)
		h->keys[key / 8] |= mask;
	else
		h->keys[key / 8] &= ~mask;
	switch (key) {
	case 0x7d: modifier = 1; break;
	case 0x7e: modifier = 2; break;
	case 0x7c: modifier = 0x404; break;
	case 0x4e: modifier = 4; break;
	case 0x7f: modifier = 8; break;
	}
	if (modifier) {
		h->state &= ~0x40fUL;
		if (h->keys[0x7d / 8] & (1 << (0x7d % 8))) h->state |= 1;
		if (h->keys[0x7e / 8] & (1 << (0x7e % 8))) h->state |= 2;
		if (h->keys[0x7c / 8] & (1 << (0x7c % 8))) h->state |= 0x404;
		if (h->keys[0x4e / 8] & (1 << (0x4e % 8))) h->state |= 4;
		if (h->keys[0x7f / 8] & (1 << (0x7f % 8))) h->state |= 8;
		return 0;
	}
	if (!down)
		return 0;
	table = h->state & 1 ? 2 : h->state & 2 ? 1 : 0;
	if (h->state & 4)
		table += 3;
	ascii = lisa_ascii[table][key];
	if (ascii >= 0x10 && ascii <= 0x14) {
		errno = EINVAL;
		return -1;
	}
	return ascii ? enqueue(h, key, ascii, time) : 0;
}

int
lisa_hw_poll(h)
struct lisa_hw *h;
{
	unsigned char *p, e[16];
	unsigned long head, tail, now, time;
	unsigned int code, type;
	long value;

	if (!h || lisa_hw_clock(&now, 0) < 0)
		return -1;
	if (h->busy_pending && due(now, h->busy_due)) {
		h->busy_pending = 0;
		h->flags |= 4;
		cursor(h);
	}
	p = h->surface + LISA_KEYBOARD;
	if (lisa_get32(p) != 0x4c4b4244 || lisa_get32(p + 4) != 1 ||
		lisa_get32(p + 8) != 1) {
		errno = EINVAL;
		return -1;
	}
	p = h->surface + LISA_INPUT;
	if (lisa_get32(p) != 0x4c494e50 || lisa_get32(p + 4) != 1) {
		errno = EINVAL;
		return -1;
	}
	for (;;) {
		head = lisa_get32(p + 8);
		tail = lisa_get32(p + 12);
		if (((tail - head) & 0xffffffffUL) > 64) {
			errno = EIO;
			return -1;
		}
		if (head == tail)
			break;
		lisa_barrier();
		memcpy(e, p + 16 + (head % 64) * 16, 16);
		type = e[0];
		code = ((unsigned int)e[2] << 8) | e[3];
		value = signed32(lisa_get32(e + 4));
		if (lisa_get32(e + 12) >= 1000000) {
			errno = EINVAL;
			return -1;
		}
		time = (lisa_get32(e + 8) * 1000 + lisa_get32(e + 12) / 1000 - h->wall_offset) & 0xffffffffUL;
		if (type == IE_REL && code <= 1 && value >= -32768 && value <= 32767) {
			if (code == 0) h->dx += value;
			else h->dy += value;
		} else if (type == IE_KEY && code <= 127 && (value == 0 || value == 1)) {
			motion(h, now);
			if (key_event(h, code, value, time) < 0)
				return -1;
		} else if (type == IE_BTN && code == 1 && (value == 0 || value == 1)) {
			motion(h, now);
			if (value) h->state |= 16;
			else h->state &= ~16UL;
			if (value) h->keys[0] |= 1 << 6;
			else h->keys[0] &= ~(1 << 6);
			if (enqueue(h, 6, value ? 0 : 1, time) < 0)
				return -1;
		} else if (type == IE_SYN)
			motion(h, now);
		else {
			errno = EIO;
			return -1;
		}
		lisa_barrier();
		lisa_put32(p + 8, (head + 1) & 0xffffffffUL);
	}
	if (due(now, h->motion_due))
		motion(h, now);
	return 0;
}

static int
image(h, r, busy)
struct lisa_hw *h;
unsigned long *r;
int busy;
{
	unsigned char *p;
	void *data, *mask;
	unsigned long rows;

	rows = r[3] & 65535;
	if (!rows || rows > 32) {
		errno = EINVAL;
		return -1;
	}
	data = h->validate(h->opaque, r[9], 2 * rows, 0);
	mask = h->validate(h->opaque, r[10], 2 * rows, 0);
	if (!data || !mask) {
		errno = EFAULT;
		return -1;
	}
	p = busy ? h->busy : h->normal;
	memset(p, 0, 160);
	lisa_put32(p, 1);
	lisa_put32(p + 4, rows);
	lisa_put32(p + 8, word(r[1]));
	lisa_put32(p + 12, word(r[2]));
	memcpy(p + 32, data, 2 * rows);
	memcpy(p + 96, mask, 2 * rows);
	h->depth = 1;
	h->flags &= ~2UL;
	cursor(h);
	return 0;
}

static int
event(h, r, consume, repeat, index, wait)
struct lisa_hw *h;
unsigned long *r;
unsigned int consume, repeat, index, wait;
{
	static unsigned char repeatable[16] = {
		0,0,0,0,255,255,0,0,254,204,255,255,255,255,255,240
	};
	struct lisa_low_event e;
	unsigned long now;
	unsigned int i, synthetic = 0;
	int n;

	if (!index || index > 64) {
		errno = EINVAL;
		return -1;
	}
	for (;;) {
		if (h->interrupted && h->interrupted(h->opaque)) {
			errno = EINTR;
			return -1;
		}
		if (lisa_hw_poll(h) < 0 || lisa_hw_clock(&now, 0) < 0)
			return -1;
		if (index <= h->count) {
			e = h->events[(h->head + index - 1) % 64];
			break;
		}
		if (!h->count && index == 1 && repeat && h->repeat_valid &&
			(h->keys[h->repeat_event.key / 8] & (1 << (h->repeat_event.key % 8))) &&
			due(now, h->repeat_due)) {
			e = h->repeat_event;
			e.state = h->state | 32;
			e.x = h->x;
			e.y = h->y;
			e.time = now;
			synthetic = 1;
			break;
		}
		if (!wait) {
			for (i = 1; i <= 7; i++) r[i] = 0;
			return 0;
		}
		n = poll(0, 0, 10);
		if (h->interrupted && h->interrupted(h->opaque)) {
			errno = EINTR;
			return -1;
		}
		if (n < 0)
			return -1;
	}
	if (consume) {
		if (synthetic)
			h->repeat_due = (now + h->repeat_next) & 0xffffffffUL;
		else {
			h->head = (h->head + 1) % 64;
			h->count--;
			h->repeat_valid = !!(repeatable[e.key / 8] & (0x80 >> (e.key % 8)));
			h->repeat_event = e;
			h->repeat_due = (now + h->repeat_initial) & 0xffffffffUL;
		}
	}
	r[1] = 1;
	r[2] = e.key;
	r[3] = e.ascii;
	r[4] = e.state;
	r[5] = e.x;
	r[6] = e.y;
	r[7] = e.time;
	return 0;
}

int
lisa_hw_dispatch(h, r, selector)
struct lisa_hw *h;
unsigned long *r;
unsigned int selector;
{
	unsigned char *p, *shape;
	unsigned long now, us, n, i, flags;
	long left, top, rows, request, delay;

	if (!h || !r || lisa_hw_poll(h) < 0 || lisa_hw_clock(&now, &us) < 0)
		return -1;
	p = h->surface + LISA_POLICY;
	switch (selector) {
	case 2:
		r[1] = h->x;
		r[2] = h->y;
		return 0;
	case 4:
		request = (short)(word(r[1]) + 2);
		request = request >= 0 ? (request + 2) / 4 : -((-request + 2) / 4);
		if (request < 0) request = 0;
		if (request > 7) request = 7;
		lisa_put32(p + 16, 4 * request);
		return 0;
	case 6: lisa_put32(p + 8, !!(r[1] & 255)); return 0;
	case 8:
		request = word(r[1]);
		if (request < 0) request = 0;
		if (request > 256) request = 256;
		lisa_put32(p + 12, request);
		return 0;
	case 14: return image(h, r, 0);
	case 134: return image(h, r, 1);
	case 16:
	case 18:
		if (h->depth >= 32769) break;
		h->depth++;
		if (selector == 16)
			h->flags &= ~2UL;
		else {
			shape = h->flags & 4 ? h->busy : h->normal;
			left = (h->x - signed32(lisa_get32(shape + 8))) & ~15L;
			top = h->y - signed32(lisa_get32(shape + 12));
			rows = lisa_get32(shape + 4);
			if (top < 0) { rows += top; top = 0; }
			if (top + rows > 364) rows = 364 - top;
			if (left < 0) left = 0;
			if (rows > 0 && word(r[4]) >= top &&
				word(r[2]) - (rows - 1) < top && word(r[3]) >= left &&
				word(r[1]) - 32 < left)
				h->flags &= ~2UL;
		}
		cursor(h);
		return 0;
	case 20:
		if (h->depth) h->depth--;
		if (!h->depth && !(h->flags & 1)) h->flags |= 2;
		cursor(h);
		return 0;
	case 24: r[9] = 0xfa4000; return 0;
	case 26: r[9] = 0xfb0000; return 0;
	case 42:
		r[1] = lisa_get32(h->surface + LISA_AUDIO + 8);
		return 0;
	case 44:
		lisa_put32(h->surface + LISA_AUDIO + 8, r[1] & 7);
		return 0;
	case 48:
	case 50:
		p = h->surface + LISA_AUDIO;
		if (selector == 50 && (r[1] < 100 || r[1] > 50000 || !r[2] || r[2] > 2000))
			break;
		n = (lisa_get32(p + 12) + 1) & 0xffffffffUL;
		if (!n) n = 1;
		lisa_put32(p + 16, selector == 50 ? r[1] : 0);
		lisa_put32(p + 20, selector == 50 ? r[2] : 0);
		lisa_put32(p + 24, selector == 50 ? 1 : 2);
		lisa_barrier();
		lisa_put32(p + 12, n);
		return 0;
	case 52:
		n = r[1] & 255;
		if (n > 127) break;
		r[2] = !!(h->keys[n / 8] & (1 << (n % 8)));
		return 0;
	case 54:
		for (i = 0; i < 4; i++) {
			p = h->keys + i * 4;
			r[i + 1] = p[0] | ((unsigned long)p[1] << 8) |
				((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
		}
		return 0;
	case 58:
	case 148: return event(h, r, 1, !!(r[1] & 255), 1, !!(r[2] & 255));
	case 60:
	case 146: return event(h, r, 0, !!(r[1] & 255), r[2], 0);
	case 62: r[1] = h->repeat_initial; r[2] = h->repeat_next; return 0;
	case 64:
		if (r[1] > 0x7fffffffUL || r[2] > 0x7fffffffUL) break;
		h->repeat_initial = r[1];
		h->repeat_next = r[2];
		return 0;
	case 68: r[1] = now; return 0;
	case 154: r[1] = us; return 0;
	case 110:
		h->flags = (h->flags | 1) & ~2UL;
		cursor(h);
		return 0;
	case 112:
	case 156:
		h->depth = 0;
		h->flags = 2 | (selector == 156 ? h->flags & 4 : 0);
		cursor(h);
		return 0;
	case 114: r[1] = 0; return 0;
	case 116: if (!(r[1] & 255)) return 0; break;
	case 136:
		delay = signed32(r[1]);
		h->busy_pending = delay > 0;
		if (delay > 0) {
			h->flags &= ~4UL;
			h->busy_due = (now + delay) & 0xffffffffUL;
		} else h->flags |= 4;
		cursor(h);
		return 0;
	case 138:
		r[1] = 720;
		r[2] = 364;
		return 0;
	case 142: r[1] = 0; return 0;
	case 144:
		flags = lisa_get32(h->surface + LISA_KEYBOARD + 8);
		if (!(r[1] & 255) && flags == 1) return 0;
		break;
	}
	errno = EINVAL;
	return -1;
}
