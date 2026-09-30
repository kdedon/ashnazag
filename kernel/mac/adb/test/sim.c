/*
 * sim.c -- host model of VIA1 port B / shift register and the Mac II
 * class ADB transceiver with devices on the bus, for testing adb.c.
 *
 * The transceiver acts on port-B state changes: CMD sends the byte in
 * SR, EVEN/ODD move the next data byte, IDLE lets sim_idle() auto-poll
 * the last talk command.  Every byte ends by setting IFR bit 2, with
 * PB3 reflecting the rules in adb.c's header.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../adb.h"
#include "sim.h"

struct sdev sim_dev[SIM_NDEV];
int sim_ndev;
int sim_dead;		/* transceiver never interrupts */
int sim_stall;		/* stop interrupting after this many bytes (0 = never) */
int sim_end2;		/* 2-byte replies signal the end on byte 1 */
int sim_nbytes;
unsigned long sim_ncmd;

static unsigned char orb = 0x3F, ddrb, acr, sr, ifr, ier;
static int pb3 = 1;		/* 1 = high (inactive) */
static int lastst = ST_IDLE;
static int lastcmd = -1;
static int mode;		/* 0 none, 1 talk, 2 listen */
static unsigned char reply[8];
static int nreply, ridx;
static unsigned char ldata[8];
static int nldata;
static int lcmd;

static void
irq(v, low)
int v, low;
{
	if (sim_dead)
		return;
	if (sim_stall && ++sim_nbytes > sim_stall)
		return;
	sr = v;
	pb3 = !low;
	ifr |= IFR_SR;
}

static int
hasdata(d)
struct sdev *d;
{
	if (d->kind == SIM_KBD)
		return d->nq > 0;
	if (d->kind == SIM_MOUSE)
		return d->mdx || d->mdy || d->mbtn != d->mlast;
	return 0;
}

/* some device other than those at addr wants service */
static int
srq_others(addr)
int addr;
{
	int i;

	for (i = 0; i < sim_ndev; i++)
		if (sim_dev[i].addr != addr && sim_dev[i].srqen && hasdata(&sim_dev[i]))
			return 1;
	return 0;
}

static int
clamp7(v)
int v;
{
	if (v > 63) v = 63;
	if (v < -64) v = -64;
	return v & 0x7F;
}

/* the reply of the first device at addr, register reg; collisions on R3 */
static int
talk(addr, reg, buf)
int addr, reg;
unsigned char *buf;
{
	int i, n = 0, first = -1;
	struct sdev *d;

	for (i = 0; i < sim_ndev; i++) {
		d = &sim_dev[i];
		if (d->addr != addr)
			continue;
		if (reg == 3) {
			d->collided = first >= 0;
			if (first < 0)
				first = i;
			continue;
		}
		if (first < 0)
			first = i;
	}
	if (first < 0)
		return 0;
	d = &sim_dev[first];
	switch (reg) {
	case 3:
		buf[0] = (d->srqen ? 0x20 : 0) | (rand() & 0x0F);
		buf[1] = d->handler;
		return 2;
	case 2:
		if (d->kind != SIM_KBD)
			return 0;
		buf[0] = 0xFF;
		buf[1] = 0xF8 | (~d->leds & 7);
		return 2;
	case 0:
		if (d->kind == SIM_KBD && d->nq > 0) {
			buf[0] = d->q[0];
			buf[1] = d->nq > 1 ? d->q[1] : 0xFF;
			n = d->nq > 1 ? 2 : 1;
			memmove(d->q, d->q + n, d->nq - n);
			d->nq -= n;
			return 2;
		}
		if (d->kind == SIM_MOUSE && hasdata(d)) {
			buf[0] = (d->mbtn ? 0 : 0x80) | clamp7(d->mdy);
			buf[1] = 0x80 | clamp7(d->mdx);
			d->mdx = d->mdy = 0;
			d->mlast = d->mbtn;
			return 2;
		}
		return 0;
	}
	return 0;
}

static void
listen_apply()
{
	int i, a = ADB_ADDR(lcmd), reg = ADB_REG(lcmd);
	struct sdev *d;

	for (i = 0; i < sim_ndev; i++) {
		d = &sim_dev[i];
		if (d->addr != a)
			continue;
		if (reg == 3 && nldata >= 2) {
			switch (ldata[1]) {
			case 0xFE:
				if (d->collided)
					continue;
				d->addr = ldata[0] & 0x0F;
				break;
			case 0x00:
				d->addr = ldata[0] & 0x0F;
				break;
			case 0xFD: case 0xFF:
				continue;
			default:
				if (ldata[1] == d->handler0 || ldata[1] == d->handler2)
					d->handler = ldata[1];
				else
					continue;
			}
			d->srqen = (ldata[0] & 0x20) != 0;
		}
		if (reg == 2 && nldata >= 2 && d->kind == SIM_KBD)
			d->leds = ~ldata[1] & 7;
	}
}

static void
command(c)
int c;
{
	int i;

	sim_ncmd++;
	lastcmd = c;
	mode = 0;
	if (c == ADB_RESET) {
		for (i = 0; i < sim_ndev; i++) {
			sim_dev[i].addr = sim_dev[i].orig;
			sim_dev[i].handler = sim_dev[i].handler0;
			sim_dev[i].srqen = 1;
			sim_dev[i].collided = 0;
		}
	} else if (ADB_ISTALK(c)) {
		mode = 1;
		nreply = talk(ADB_ADDR(c), ADB_REG(c), reply);
		ridx = 0;
	} else if (ADB_ISLISTEN(c)) {
		mode = 2;
		nldata = 0;
		lcmd = c;
	}
	irq(c, srq_others(ADB_ADDR(c)));
}

static void
datastep()
{
	if (mode == 1) {
		if (nreply == 0 || ridx >= nreply) {
			irq(0xFF, 1);
			ridx++;
			return;
		}
		irq(reply[ridx], sim_end2 && nreply == 2 && ridx == 1);
		ridx++;
	} else if (mode == 2) {
		if (nldata < 8)
			ldata[nldata++] = sr;
		irq(sr, 0);
	}
}

static void
statechange(st)
int st;
{
	if (mode == 2 && st != ST_EVEN && st != ST_ODD) {
		listen_apply();
		mode = 0;
	}
	if (st == ST_CMD || st == ST_IDLE)
		pb3 = 1;		/* /INT released between transactions */
	switch (st) {
	case ST_CMD:
		if ((acr & ACR_SRMASK) != ACR_SROUT)
			fprintf(stderr, "sim: CMD with SR not shifting out\n");
		command(sr);
		break;
	case ST_EVEN:
	case ST_ODD:
		datastep();
		break;
	case ST_IDLE:
		mode = 0;
		break;
	}
}

unsigned char
sim_rd(r)
int r;
{
	switch (r) {
	case V_ORB:
		return (orb & ~PB_INT) | (pb3 ? PB_INT : 0);
	case V_DDRB: return ddrb;
	case V_ACR: return acr;
	case V_SR:
		ifr &= ~IFR_SR;
		return sr;
	case V_IFR:
		return ifr | ((ifr & ier & 0x7F) ? 0x80 : 0);
	case V_IER: return ier | 0x80;
	}
	return 0;
}

void
sim_wr(r, v)
int r, v;
{
	int st;

	switch (r) {
	case V_ORB:
		st = v & PB_ST;
		orb = v;
		if (st != lastst) {
			lastst = st;
			statechange(st);
		}
		break;
	case V_DDRB: ddrb = v; break;
	case V_ACR: acr = v; break;
	case V_SR:
		ifr &= ~IFR_SR;
		sr = v;
		break;
	case V_IFR: ifr &= ~(v & 0x7F); break;
	case V_IER:
		if (v & 0x80) ier |= v & 0x7F;
		else ier &= ~(v & 0x7F);
		break;
	}
}

/* time passes on an idle bus: the transceiver auto-polls */
void
sim_idle()
{
	int c = lastcmd;

	if (lastst != ST_IDLE || (ifr & IFR_SR) || c < 0 || !ADB_ISTALK(c) ||
	    ADB_REG(c) != 0)
		return;
	mode = 1;
	nreply = talk(ADB_ADDR(c), 0, reply);
	ridx = 0;
	if (nreply > 0 || srq_others(ADB_ADDR(c)))
		irq(c, 1);
	else
		mode = 0;
}

int sim_pending() { return (ifr & ier & IFR_SR) != 0; }
int sim_state() { return lastst; }
int sim_srqen(i) int i; { return sim_dev[i].srqen; }

void
sim_reset()
{
	orb = 0x3F; ddrb = acr = sr = ifr = ier = 0;
	pb3 = 1; lastst = ST_IDLE; lastcmd = -1; mode = 0;
	sim_ndev = 0; sim_dead = sim_stall = sim_end2 = sim_nbytes = 0;
	sim_ncmd = 0;
	memset(sim_dev, 0, sizeof sim_dev);
}

int
sim_add(kind, orig, handler, handler2)
int kind, orig, handler, handler2;
{
	struct sdev *d = &sim_dev[sim_ndev];

	d->kind = kind;
	d->orig = d->addr = orig;
	d->handler = d->handler0 = handler;
	d->handler2 = handler2;
	d->srqen = 1;
	return sim_ndev++;
}

void
sim_key(i, code)
int i, code;
{
	sim_dev[i].q[sim_dev[i].nq++] = code;
}
