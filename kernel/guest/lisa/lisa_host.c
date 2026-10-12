#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include "lisa_host.h"
#include "lisa_audio.h"
#include "lisa_wire.h"

void
lisa_host_init(h)
struct lisa_host *h;
{
	memset(h, 0, sizeof *h);
	nh_display_init(&h->display);
	h->audio_fd = -1;
}

int
lisa_host_open(h, device, front)
struct lisa_host *h;
const char *device;
int front;
{
	struct fbinfo *f;
	struct fbcmap cmap;
	unsigned short red[2], green[2], blue[2];
	int saved;

	if (!h) {
		errno = EINVAL;
		return -1;
	}
	if (nh_display_open(&h->display, device, "Lisa", front) < 0)
		return -1;
	if (ioctl(h->display.keyboard, EVIOCGINFO, &h->keyboard_info) < 0)
		goto failed;
	if (h->keyboard_info.ei_kset != EVK_ADB) {
		errno = EINVAL;
		goto failed;
	}
	f = &h->display.info;
	if (nh_surface_init(&h->surface, h->display.mapping, f->fi_size, f) < 0)
		goto failed;
	h->white = 0;
	h->black = 1;
	if (f->fi_visual == FBV_TRUE) {
		h->white = f->fi_rmask | f->fi_gmask | f->fi_bmask;
		h->black = 0;
	} else if (f->fi_visual == FBV_PSEUDO) {
		if (f->fi_cmapsize < 2 || !(f->fi_flags & FBF_CMAP)) {
			errno = EINVAL;
			goto failed;
		}
		red[0] = green[0] = blue[0] = 65535;
		red[1] = green[1] = blue[1] = 0;
		cmap.cm_start = 0;
		cmap.cm_count = 2;
		cmap.cm_red = red;
		cmap.cm_green = green;
		cmap.cm_blue = blue;
		if (ioctl(h->display.framebuffer, FBIOPUTCMAP, &cmap) < 0)
			goto failed;
	} else if (f->fi_visual != FBV_MONO) {
		errno = EINVAL;
		goto failed;
	}
	return 0;
failed:
	saved = errno;
	nh_display_close(&h->display);
	memset(&h->surface, 0, sizeof h->surface);
	errno = saved;
	return -1;
}

static void
pixel(s, x, y, value)
struct nh_surface *s;
unsigned long x, y, value;
{
	unsigned char *p;
	unsigned long bit, shift, mask, bytes, n;

	p = s->pixels + y * s->stride;
	if (s->depth < 8) {
		bit = x * s->depth;
		shift = 8 - s->depth - bit % 8;
		mask = ((1UL << s->depth) - 1) << shift;
		p[bit / 8] = (unsigned char)((p[bit / 8] & ~mask) |
			(value << shift));
	} else {
		bytes = s->depth / 8;
		p += x * bytes;
		for (n = 0; n < bytes; n++)
			p[n] = (unsigned char)(value >> (8 * (bytes - n - 1)));
	}
}

int
lisa_host_present(h, bits, width, height, rowbytes)
struct lisa_host *h;
const unsigned char *bits;
unsigned long width, height, rowbytes;
{
	unsigned long x, y, sx, sy, value, size, dw, dh;
	unsigned char *copy;
	struct nh_surface *s;

	if (!h || !bits || !width || !height ||
		width / 8 + (width % 8 != 0) > rowbytes ||
		height > (~0UL) / rowbytes || !h->surface.pixels ||
		!h->surface.width || !h->surface.height ||
		width > (~0UL) / h->surface.height ||
		height > (~0UL) / h->surface.width) {
		errno = EINVAL;
		return -1;
	}
	s = &h->surface;
	size = height * rowbytes;
	if (size > h->snapshot_size) {
		copy = (unsigned char *)realloc(h->snapshot, size);
		if (!copy)
			return -1;
		h->snapshot = copy;
		h->snapshot_size = size;
	}
	memcpy(h->snapshot, bits, size);
	dw = s->width;
	dh = dw * height / width;
	if (dh > s->height) {
		dh = s->height;
		dw = dh * width / height;
	}
	if (!dw || !dh) {
		errno = EINVAL;
		return -1;
	}
	if (h->view_width != dw || h->view_height != dh) {
		if (nh_surface_fill(s, 0, 0, s->width, s->height, h->black) < 0)
			return -1;
	}
	h->view_x = (s->width - dw) / 2;
	h->view_y = (s->height - dh) / 2;
	h->view_width = dw;
	h->view_height = dh;
	for (y = 0; y < dh; y++) {
		sy = y * height / dh;
		for (x = 0; x < dw; x++) {
			sx = x * width / dw;
			value = h->snapshot[sy * rowbytes + sx / 8] &
				(0x80 >> (sx % 8)) ? h->black : h->white;
			pixel(s, h->view_x + x, h->view_y + y, value);
		}
	}
	return 0;
}

int
lisa_host_input(h, event)
struct lisa_host *h;
struct inev *event;
{
	int n, first, second;

	if (!h || !event) {
		errno = EINVAL;
		return -1;
	}
	first = h->input_turn ? h->display.mouse : h->display.keyboard;
	second = h->input_turn ? h->display.keyboard : h->display.mouse;
	h->input_turn = !h->input_turn;
	n = nh_input(first, event);
	return n ? n : nh_input(second, event);
}

int
lisa_host_publish(h, surface)
struct lisa_host *h;
unsigned char *surface;
{
	struct inev event;
	unsigned char *p, *slot, keys[16];
	unsigned long head, tail;
	int n, count = 0;

	if (!h || !surface) {
		errno = EINVAL;
		return -1;
	}
	p = surface + LISA_KEYBOARD;
	if (lisa_get32(p) != 0x4c4b4244 || lisa_get32(p + 4) != 1 ||
		h->keyboard_info.ei_kset != EVK_ADB) {
		errno = EINVAL;
		return -1;
	}
	lisa_put32(p + 8, h->keyboard_info.ei_kset);
	lisa_put32(p + 12, h->keyboard_info.ei_id);
	lisa_put32(p + 16, h->keyboard_info.ei_flags);
	if (ioctl(h->display.keyboard, EVIOCGKEYS, keys) < 0)
		return -1;
	memcpy(p + 20, keys, sizeof keys);
	p = surface + LISA_INPUT;
	if (lisa_get32(p) != 0x4c494e50 || lisa_get32(p + 4) != 1) {
		errno = EINVAL;
		return -1;
	}
	for (;;) {
		head = lisa_get32(p + 8);
		tail = lisa_get32(p + 12);
		if ((unsigned long)((tail - head) & 0xffffffffUL) > 64) {
			errno = ENOSPC;
			return -1;
		}
		if (((tail - head) & 0xffffffffUL) == 64)
			return count;
		n = lisa_host_input(h, &event);
		if (n <= 0)
			return n < 0 ? -1 : count;
		if ((event.ie_type == IE_KEY && (event.ie_code > 127 ||
			(event.ie_value != 0 && event.ie_value != 1))) ||
			(event.ie_type == IE_REL && (event.ie_code > 1 ||
			event.ie_value < -32768 || event.ie_value > 32767)) ||
			(event.ie_type == IE_BTN && (event.ie_code != 1 ||
			(event.ie_value != 0 && event.ie_value != 1))) ||
			(event.ie_type != IE_KEY && event.ie_type != IE_REL &&
			event.ie_type != IE_BTN && event.ie_type != IE_SYN)) {
			errno = EIO;
			return -1;
		}
		slot = p + 16 + (tail % 64) * 16;
		slot[0] = event.ie_type;
		slot[1] = event.ie_unit;
		slot[2] = event.ie_code >> 8;
		slot[3] = event.ie_code;
		lisa_put32(slot + 4, event.ie_value);
		lisa_put32(slot + 8, event.ie_sec);
		lisa_put32(slot + 12, event.ie_usec);
		lisa_barrier();
		lisa_put32(p + 12, (tail + 1) & 0xffffffffUL);
		count++;
	}
}

int
lisa_host_render(h, surface)
struct lisa_host *h;
const unsigned char *surface;
{
	unsigned char cursor[160], check[160], image[32760], verify[32760];
	unsigned long flags, rows, row, col, bit, mask, data;
	long left, top, x, y;
	int tries;

	if (!h || !surface) {
		errno = EINVAL;
		return -1;
	}
	for (tries = 0; tries < 4; tries++) {
		/* Retry while the shared image or cursor changes. */
		memcpy(cursor, surface + LISA_CURSOR, sizeof cursor);
		memcpy(image, surface + LISA_BITMAP, sizeof image);
		lisa_barrier();
		memcpy(check, surface + LISA_CURSOR, sizeof check);
		memcpy(verify, surface + LISA_BITMAP, sizeof verify);
		if (!memcmp(cursor, check, sizeof cursor) &&
			!memcmp(image, verify, sizeof image))
			break;
	}
	if (tries == 4) {
		errno = EAGAIN;
		return -1;
	}
	rows = lisa_get32(cursor + 4);
	flags = lisa_get32(cursor + 28);
	if (lisa_get32(cursor) != 1 || rows > 32 || flags > 7 ||
		lisa_get32(cursor + 24) > 32769 ||
		lisa_get32(cursor + 16) > 719 || lisa_get32(cursor + 20) > 363) {
		errno = EINVAL;
		return -1;
	}
	if (flags & 2) {
		left = (long)(int)lisa_get32(cursor + 16) - (long)(int)lisa_get32(cursor + 8);
		top = (long)(int)lisa_get32(cursor + 20) - (long)(int)lisa_get32(cursor + 12);
		for (row = 0; row < rows; row++) {
			y = top + row;
			if (y < 0 || y >= 364)
				continue;
			for (col = 0; col < 16; col++) {
				x = left + col;
				if (x < 0 || x >= 720)
					continue;
				mask = (cursor[96 + row * 2 + col / 8] >> (7 - col % 8)) & 1;
				data = (cursor[32 + row * 2 + col / 8] >> (7 - col % 8)) & 1;
				bit = (image[y * 90 + x / 8] >> (7 - x % 8)) & 1;
				bit = (bit & !mask) ^ data;
				image[y * 90 + x / 8] = (image[y * 90 + x / 8] &
					~(0x80 >> (x % 8))) | (bit << (7 - x % 8));
			}
		}
	}
	return lisa_host_present(h, image, 720, 364, 90);
}

int
lisa_host_close(h)
struct lisa_host *h;
{
	int saved = 0;

	if (!h) {
		errno = EINVAL;
		return -1;
	}
	if (lisa_audio_stop(h) < 0)
		saved = errno;
	if (nh_display_close(&h->display) < 0 && !saved)
		saved = errno;
	memset(&h->surface, 0, sizeof h->surface);
	free(h->snapshot);
	h->snapshot = 0;
	h->snapshot_size = 0;
	h->view_width = h->view_height = 0;
	if (saved) {
		errno = saved;
		return -1;
	}
	return 0;
}
