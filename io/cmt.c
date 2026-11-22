#include	"compiler.h"
#include	"pccore.h"
#include	"iocore.h"
#include	"z80core.h"
#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>


/* ==== TapeOS M3 patch: clock-synchronised CMT tape player ====
 *
 * The stock xmil core never drives PPI port B bit1 (the tape
 * input): cmt_read() was a stub returning 0 and its call site in
 * io/ppi.c was commented out, so the compatible IPL's CMT boot
 * could never work.  This player feeds a packed bitstream file
 * (path given by the XMIL_TAPE environment variable) into bit1,
 * timed by the emulated CPU clock.  Timing uses CPU_CLOCKCOUNT
 * deltas sampled at each port read; the IPL polls the port in a
 * tight loop while reading tape, so the deltas are exact.
 *
 * Bit encoding (matches the IPL's MT_RDBIT reader): every bit
 * cell starts with a rising edge; the IPL samples the level
 * about 185 us after the edge -- still high means 1, already
 * low means 0.
 *     bit 1: high 380 us, then low 280 us
 *     bit 0: high 100 us, then low 280 us
 * File format: raw packed bits, MSB first (see mkcmt.py in the
 * TapeOS project).  PLAY resumes where STOP paused; REW rewinds.
 */

static UINT8 *cmt_bits = NULL;		/* one byte per bit cell */
static UINT cmt_nbits = 0;
static UINT cmt_pos = 0;			/* current bit cell */
static UINT32 cmt_phase = 0;		/* clocks into current phase */
static BOOL cmt_high = FALSE;		/* current phase is high */
static BOOL cmt_playing = FALSE;
static UINT32 cmt_lastclk = 0;
static UINT cmt_bits_cap = 0;	/* capacity of cmt_bits */

/* ==== TapeOS tape-4800 patch: transfer speed selection ====
 *
 * The environment variable XMIL_TAPE_BPS selects the wire
 * speed for BOTH directions of this deck: unset (or any
 * value other than "4800") keeps the classic timings the
 * compatible IPL reads (bit 1: high 380 us + low 280 us;
 * bit 0: high 100 us + low 280 us; recorder sample point
 * 185 us after each rising edge).  "4800" selects the
 * width format: bit 1 = high 130 us + low 70 us (a
 * 200 us cell), bit 0 = high 35 us + low 130 us (a
 * 165 us cell) - the classes differ in both phases -
 * recorder sample at 46 us, for sessions
 * running the TAPE4800 build of TapeOS, whose IOCS tape
 * reader/writer use the matching constants in
 * tapeos/src/tape_x1.inc.  The IPL boot path is NOT
 * reachable at 4800: the IPL ROM is a third-party
 * artifact (CC0) whose reader is fixed at the classic
 * decision point, so IPL CMT boot sessions must keep
 * the default speed.  See tape-4800.md.
 */
static UINT cmt_hi1_us = 380;		/* bit 1 high phase */
static UINT cmt_hi0_us = 100;		/* bit 0 high phase */
static UINT cmt_lo1_us = 280;		/* bit 1 low phase */
static UINT cmt_lo0_us = 280;		/* bit 0 low phase */
static UINT cmt_sample_us = 185;	/* recorder sample point */

static void cmt_speed_init(void) {

	const char *bps;

	bps = getenv("XMIL_TAPE_BPS");
	if ((bps != NULL) && (strcmp(bps, "4800") == 0)) {
		cmt_hi1_us = 130;
		cmt_hi0_us = 35;
		cmt_lo1_us = 70;
		cmt_lo0_us = 130;
		cmt_sample_us = 46;
	}
	else {
		cmt_hi1_us = 380;
		cmt_hi0_us = 100;
		cmt_lo1_us = 280;
		cmt_lo0_us = 280;
		cmt_sample_us = 185;
	}
}

static UINT32 cmt_us2clk(UINT us) {

	UINT32 clk;

	clk = xmilcfg.X1_CPU_CLOCK;
	if (clk == 0) {
		clk = 4000000;
	}
	/* X1_CPU_CLOCK is a whole number of MHz in this core */
	return(us * (clk / 1000000));
}

static void cmt_tape_free(void) {

	if (cmt_bits != NULL) {
		free(cmt_bits);
		cmt_bits = NULL;
	}
	cmt_nbits = 0;
	cmt_pos = 0;
	cmt_playing = FALSE;
	cmt_bits_cap = 0;
}

static void cmt_tape_load(void) {

	const char *path;
	FILE *fp;
	long size;
	UINT8 *packed;
	UINT i;
	int b;

	cmt_tape_free();
	path = getenv("XMIL_TAPE");
	if ((path == NULL) || (path[0] == '\0')) {
		return;
	}
	fp = fopen(path, "rb");
	if (fp == NULL) {
		return;
	}
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size <= 0) {
		fclose(fp);
		return;
	}
	packed = (UINT8 *)malloc((size_t)size);
	cmt_bits = (UINT8 *)malloc((size_t)size * 8);
	if ((packed == NULL) || (cmt_bits == NULL)) {
		free(packed);
		cmt_tape_free();
		fclose(fp);
		return;
	}
	if (fread(packed, 1, (size_t)size, fp) != (size_t)size) {
		free(packed);
		cmt_tape_free();
		fclose(fp);
		return;
	}
	fclose(fp);
	cmt_nbits = (UINT)size * 8;
	cmt_bits_cap = cmt_nbits;
	for (i = 0; i < (UINT)size; i++) {
		for (b = 0; b < 8; b++) {
			cmt_bits[(i * 8) + b] =
					(UINT8)((packed[i] >> (7 - b)) & 1);
		}
	}
	free(packed);
}

REG8 cmt_read(void) {

	UINT32 now;
	UINT32 elapsed;
	UINT32 dur;

	if ((!cmt_playing) || (cmt_bits == NULL)) {
		return(0);
	}
	now = (UINT32)CPU_CLOCKCOUNT;
	elapsed = now - cmt_lastclk;
	cmt_lastclk = now;
	if (elapsed > cmt_us2clk(5000)) {
		/* clock counter jumped (reset/slice boundary):
		   drop the unmeasurable interval */
		elapsed = 0;
	}
	cmt_phase += elapsed;
	while (cmt_pos < cmt_nbits) {
		if (cmt_high) {
			dur = cmt_us2clk(cmt_bits[cmt_pos]
					? cmt_hi1_us : cmt_hi0_us);
		}
		else {
			dur = cmt_us2clk(cmt_bits[cmt_pos]
					? cmt_lo1_us : cmt_lo0_us);
		}
		if (cmt_phase < dur) {
			return(cmt_high ? 0x02 : 0x00);
		}
		cmt_phase -= dur;
		if (cmt_high) {
			cmt_high = FALSE;
		}
		else {
			cmt_high = TRUE;
			cmt_pos++;
		}
	}
	cmt_playing = FALSE;
	return(0);
}

/* ==== TapeOS M3 patch, part 2: CMT recorder ====
 *
 * cmt_write() is called from io/ppi.c setportc() with the tape
 * output bit (8255 port C bit0).  While the deck is in REC,
 * level transitions are timestamped by CPU clock.  On STOP the
 * recorded segment is decoded with the IPL's rule (the bit is
 * the level 185 us after each rising edge) and appended to the
 * loaded tape; the tape file is then rewritten (path from
 * XMIL_TAPE_REC, falling back to XMIL_TAPE).  A REC that does
 * not directly follow a REC..STOP segment starts a fresh tape,
 * so TWINF followed by TWDAT builds one continuous stream.
 */

static UINT8 cmt_wline = 0;		/* last level on port C bit0 */
static BOOL cmt_rec = FALSE;
static BOOL cmt_rec_chain = FALSE;
static BOOL cmt_rec_trunc = FALSE;
static UINT32 *cmt_rec_t = NULL;	/* segment clock of transition */
static UINT8 *cmt_rec_l = NULL;		/* level after transition */
static UINT cmt_rec_n = 0;
static UINT cmt_rec_cap = 0;
static UINT32 cmt_rec_lastclk = 0;
static UINT8 cmt_rec_level0 = 0;	/* line level at REC start */

static BOOL cmt_bits_reserve(UINT need) {

	UINT8 *nb;
	UINT cap;

	if (need <= cmt_bits_cap) {
		return(TRUE);
	}
	cap = cmt_bits_cap ? cmt_bits_cap : 65536;
	while (cap < need) {
		cap *= 2;
	}
	nb = (UINT8 *)malloc((size_t)cap);
	if (nb == NULL) {
		return(FALSE);
	}
	if (cmt_bits != NULL) {
		memcpy(nb, cmt_bits, cmt_nbits);
		free(cmt_bits);
	}
	cmt_bits = nb;
	cmt_bits_cap = cap;
	return(TRUE);
}

static void cmt_rec_savefile(void) {

	const char *path;
	FILE *fp;
	UINT i;
	UINT nbytes;
	UINT8 *packed;

	path = getenv("XMIL_TAPE_REC");
	if ((path == NULL) || (path[0] == '\0')) {
		path = getenv("XMIL_TAPE");
	}
	if ((path == NULL) || (path[0] == '\0')) {
		return;
	}
	nbytes = (cmt_nbits + 7) / 8;
	packed = (UINT8 *)malloc(nbytes ? nbytes : 1);
	if (packed == NULL) {
		return;
	}
	memset(packed, 0, nbytes);
	for (i = 0; i < cmt_nbits; i++) {
		if (cmt_bits[i]) {
			packed[i / 8] |= (UINT8)(0x80 >> (i % 8));
		}
	}
	fp = fopen(path, "wb");
	if (fp != NULL) {
		fwrite(packed, 1, nbytes, fp);
		fclose(fp);
	}
	free(packed);
}

static void cmt_rec_flush(void) {

	UINT i;
	UINT j;
	UINT32 sample;
	UINT8 level;
	UINT8 prev;

	if (cmt_rec_trunc) {
		cmt_nbits = 0;
		cmt_rec_trunc = FALSE;
	}
	for (i = 0; i < cmt_rec_n; i++) {
		prev = (i == 0) ? cmt_rec_level0 : cmt_rec_l[i - 1];
		if ((prev == 0) && (cmt_rec_l[i] != 0)) {
			/* rising edge: the bit is the level at the
			   speed's sample point (185 us classic,
			   46 us at 4800 bps) */
			sample = cmt_rec_t[i] + cmt_us2clk(cmt_sample_us);
			level = cmt_rec_l[i];
			for (j = i + 1; j < cmt_rec_n; j++) {
				if (cmt_rec_t[j] > sample) {
					break;
				}
				level = cmt_rec_l[j];
			}
			if (cmt_bits_reserve(cmt_nbits + 1)) {
				cmt_bits[cmt_nbits++] = level ? 1 : 0;
			}
		}
	}
	cmt_rec_n = 0;
	cmt_pos = 0;
	cmt_rec_savefile();
}

void cmt_write(REG8 dat) {

	UINT32 now;
	UINT32 delta;
	UINT8 cur;
	UINT ncap;
	UINT32 *nt;
	UINT8 *nl;

	cmt_wline = (UINT8)(dat & 1);
	if (!cmt_rec) {
		return;
	}
	cur = (cmt_rec_n > 0) ? cmt_rec_l[cmt_rec_n - 1]
						  : cmt_rec_level0;
	if (cmt_wline == cur) {
		return;
	}
	now = (UINT32)CPU_CLOCKCOUNT;
	delta = now - cmt_rec_lastclk;
	cmt_rec_lastclk = now;
	if (delta > cmt_us2clk(60000)) {
		delta = 0;	/* counter discontinuity: drop it */
	}
	if (cmt_rec_n >= cmt_rec_cap) {
		ncap = cmt_rec_cap ? cmt_rec_cap * 2 : 8192;
		nt = (UINT32 *)malloc((size_t)ncap * sizeof(UINT32));
		nl = (UINT8 *)malloc((size_t)ncap);
		if ((nt == NULL) || (nl == NULL)) {
			free(nt);
			free(nl);
			cmt_rec = FALSE;
			return;
		}
		if (cmt_rec_t != NULL) {
			memcpy(nt, cmt_rec_t,
					(size_t)cmt_rec_n * sizeof(UINT32));
			memcpy(nl, cmt_rec_l, cmt_rec_n);
			free(cmt_rec_t);
			free(cmt_rec_l);
		}
		cmt_rec_t = nt;
		cmt_rec_l = nl;
		cmt_rec_cap = ncap;
	}
	cmt_rec_t[cmt_rec_n] =
		((cmt_rec_n > 0) ? cmt_rec_t[cmt_rec_n - 1] : 0) + delta;
	cmt_rec_l[cmt_rec_n] = cmt_wline;
	cmt_rec_n++;
}

void cmt_ctrl(REG8 cmd) {

	switch(cmd) {
		case 0x00:				/* EJECT */
			cmt_playing = FALSE;
			if (cmt_rec) {
				cmt_rec = FALSE;
				cmt_rec_flush();
			}
			cmt_rec_chain = FALSE;
			cmt_pos = 0;
			iocore.s.ppib &= ~0x01;
			break;

		case 0x01:				/* STOP */
			cmt_playing = FALSE;
			if (cmt_rec) {
				cmt_rec = FALSE;
				cmt_rec_flush();
				cmt_rec_chain = TRUE;
			}
			else {
				cmt_rec_chain = FALSE;
			}
			iocore.s.ppib &= ~0x01;
			break;

		case 0x02:				/* PLAY */
			if (cmt_rec) {
				cmt_rec = FALSE;
				cmt_rec_flush();
			}
			cmt_rec_chain = FALSE;
			if (cmt_bits != NULL) {
				cmt_playing = TRUE;
				cmt_high = TRUE;
				cmt_phase = 0;
				cmt_lastclk = (UINT32)CPU_CLOCKCOUNT;
			}
			break;

		case 0x03:				/* FF */
		case 0x05:				/* APSS_FF */
			cmt_playing = FALSE;
			cmt_rec_chain = FALSE;
			break;

		case 0x04:				/* REW */
		case 0x06:				/* APSS_REW */
			cmt_playing = FALSE;
			cmt_rec_chain = FALSE;
			cmt_pos = 0;
			break;

		case 0x0a:				/* REC */
			cmt_playing = FALSE;
			cmt_rec = TRUE;
			cmt_rec_n = 0;
			cmt_rec_trunc = !cmt_rec_chain;
			cmt_rec_chain = FALSE;
			cmt_rec_level0 = cmt_wline;
			cmt_rec_lastclk = (UINT32)CPU_CLOCKCOUNT;
			break;

		default:
			cmt_playing = FALSE;
			break;
	}
	cmt.cmd = (UINT8)cmd;
}

REG8 cmt_tape_stat(void) {

	if (cmt_bits == NULL) {
		return(0);
	}
	/* bit0: tape rotating, bit1: tape set, bit2: write protect */
	return(cmt_playing ? 0x03 : 0x02);
}

REG8 cmt_ctrl_stat(void) {

	return(cmt.cmd);
}

#if 0
REG8 cmt_test(void) {

	if (cmt.stop) {
		cmt.stop = 0;
		return(0);
	}
	else {
		return(1);
	}
}
#endif


/* reset */

void cmt_reset(void) {

	cmt.cmd = 0;
	iocore.s.ppib &= ~0x01;
	cmt_rec = FALSE;
	cmt_rec_chain = FALSE;
	cmt_rec_n = 0;
	cmt_speed_init();
	cmt_tape_load();
}

