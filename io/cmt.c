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

/* ==== TapeOS tape-interop patch: clean PCM WAV tapes ====
 *
 * The .cmt packed-bitstream format above is an invention of
 * this project (the stock core had no CMT input at all), so
 * tapes could neither leave nor enter from the outside
 * world, where the circulating form is WAV audio.  When the
 * XMIL_TAPE path ends in ".wav" (any case), the deck now
 * loads a clean PCM WAV instead: RIFF/WAVE, PCM (format 1),
 * 8- or 16-bit samples, mono or stereo (channel 0 is used),
 * any sample rate.  The waveform is digitised with ONE
 * fixed threshold (the midpoint of the sample range: 0 for
 * 16-bit signed, 128 for 8-bit unsigned) and then decoded
 * into the same internal bitstream with the recorder's own
 * rule: every bit cell starts with a rising edge and the
 * bit is the level at the speed's sample point (185 us
 * classic, 46 us at 4800 bps) after that edge.  Playback
 * then synthesises the pulses from the bits with the
 * timing table above, exactly as for .cmt, so WAV playback
 * timing is re-normalised to the deck's canonical timings.
 *
 * Recording to a ".wav" path (see cmt_rec_savefile) writes
 * the canonical form: 48000 Hz, 16-bit signed mono, square
 * wave at +/-12000, phases from the same timing table
 * (phase sample counts use cumulative remainder carrying,
 * shared with the host tool wavclean - see tapeos/tools/
 * wavclean.py, which renders bit-identical files).
 *
 * Deliberately NOT in the core: DC offset removal, level
 * normalisation, polarity correction, hysteresis.  Dirty
 * WAVs are the host-side wavclean tool's job; the core
 * stays deterministic and dumb, per the design agreement
 * (tape-interop.md).  The .cmt path below is unchanged and
 * remains the regression reference.
 */

static BOOL cmt_path_is_wav(const char *path) {

	size_t len;

	if (path == NULL) {
		return(FALSE);
	}
	len = strlen(path);
	if (len < 4) {
		return(FALSE);
	}
	path += len - 4;
	return((path[0] == '.') &&
			((path[1] == 'w') || (path[1] == 'W')) &&
			((path[2] == 'a') || (path[2] == 'A')) &&
			((path[3] == 'v') || (path[3] == 'V')));
}

static UINT cmt_rd16le(const UINT8 *p) {

	return((UINT)(p[0] | (p[1] << 8)));
}

static UINT32 cmt_rd32le(const UINT8 *p) {

	return((UINT32)p[0] | ((UINT32)p[1] << 8) |
			((UINT32)p[2] << 16) | ((UINT32)p[3] << 24));
}

/* digitised level (0/1) of frame `idx`, channel 0 */
static int cmt_wav_level(const UINT8 *data, UINT idx,
		UINT channels, UINT bits) {

	if (bits == 16) {
		const UINT8 *p = data + ((size_t)idx * channels * 2);
		int v;

		v = (int)(p[0] | (p[1] << 8));
		if (v >= 32768) {
			v -= 65536;
		}
		return(v > 0);
	}
	else {
		return(data[(size_t)idx * channels] > 127);
	}
}

static void cmt_wav_decode(const UINT8 *buf, UINT size) {

	const UINT8 *fmtp;
	const UINT8 *datap;
	UINT32 datalen;
	UINT pos;
	UINT format;
	UINT channels;
	UINT bits;
	UINT32 rate;
	UINT nframes;
	UINT off;
	UINT i;
	UINT count;
	UINT idx;
	int prev;
	int cur;

	if ((size < 44) || (memcmp(buf, "RIFF", 4) != 0) ||
			(memcmp(buf + 8, "WAVE", 4) != 0)) {
		return;
	}
	fmtp = NULL;
	datap = NULL;
	datalen = 0;
	pos = 12;
	while (pos + 8 <= size) {
		UINT32 clen;

		clen = cmt_rd32le(buf + pos + 4);
		if (clen > size - (pos + 8)) {
			clen = size - (pos + 8);
		}
		if ((memcmp(buf + pos, "fmt ", 4) == 0) &&
				(clen >= 16)) {
			fmtp = buf + pos + 8;
		}
		else if (memcmp(buf + pos, "data", 4) == 0) {
			datap = buf + pos + 8;
			datalen = clen;
		}
		pos += 8 + clen + (clen & 1);
	}
	if ((fmtp == NULL) || (datap == NULL)) {
		return;
	}
	format = cmt_rd16le(fmtp);
	channels = cmt_rd16le(fmtp + 2);
	rate = cmt_rd32le(fmtp + 4);
	bits = cmt_rd16le(fmtp + 14);
	if ((format != 1) || (channels < 1) || (channels > 2) ||
			((bits != 8) && (bits != 16)) || (rate < 4000)) {
		return;
	}
	nframes = datalen / (channels * (bits / 8));
	if (nframes == 0) {
		return;
	}
	/* sample-point offset in samples (the recorder's rule) */
	off = (UINT)(((UINT32)cmt_sample_us * rate) / 1000000);
	/* pass 1: count the rising edges (= the bit cells) */
	count = 0;
	prev = 0;
	for (i = 0; i < nframes; i++) {
		cur = cmt_wav_level(datap, i, channels, bits);
		if ((cur != 0) && (prev == 0)) {
			count++;
		}
		prev = cur;
	}
	if (count == 0) {
		return;
	}
	cmt_bits = (UINT8 *)malloc((size_t)count);
	if (cmt_bits == NULL) {
		return;
	}
	cmt_nbits = count;
	cmt_bits_cap = count;
	/* pass 2: the bit is the level at the sample point */
	idx = 0;
	prev = 0;
	for (i = 0; i < nframes; i++) {
		cur = cmt_wav_level(datap, i, channels, bits);
		if ((cur != 0) && (prev == 0)) {
			UINT j;

			j = i + off;
			if (j >= nframes) {
				j = nframes - 1;
			}
			cmt_bits[idx++] =
				(UINT8)cmt_wav_level(datap, j,
						channels, bits);
		}
		prev = cur;
	}
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
	if (cmt_path_is_wav(path)) {
		UINT8 *buf;

		buf = (UINT8 *)malloc((size_t)size);
		if (buf == NULL) {
			fclose(fp);
			return;
		}
		if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
			free(buf);
			fclose(fp);
			return;
		}
		fclose(fp);
		cmt_wav_decode(buf, (UINT)size);
		free(buf);
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

/* Canonical WAV rendering of the recorded bitstream
 * (see the tape-interop patch note at cmt_wav_decode):
 * 48000 Hz, 16-bit signed mono, high = +12000,
 * low = -12000.  Phase sample counts use cumulative
 * remainder carrying so the render is bit-identical to
 * the host wavclean tool's output for the same bits. */
static void cmt_wav_savefile(const char *path) {

	FILE *fp;
	UINT16 *samp;
	UINT total;
	UINT rem;
	UINT i;
	UINT n;
	UINT dur;
	UINT8 hdr[44];
	UINT32 dsz;

	/* pass 1: total sample count */
	total = 0;
	rem = 0;
	for (i = 0; i < cmt_nbits; i++) {
		dur = cmt_bits[i] ? cmt_hi1_us : cmt_hi0_us;
		n = (dur * 48000 + rem) / 1000000;
		rem = (dur * 48000 + rem) % 1000000;
		total += n;
		dur = cmt_bits[i] ? cmt_lo1_us : cmt_lo0_us;
		n = (dur * 48000 + rem) / 1000000;
		rem = (dur * 48000 + rem) % 1000000;
		total += n;
	}
	samp = (UINT16 *)malloc((size_t)(total ? total : 1) *
			sizeof(UINT16));
	if (samp == NULL) {
		return;
	}
	/* pass 2: fill */
	n = 0;
	rem = 0;
	for (i = 0; i < cmt_nbits; i++) {
		UINT k;
		UINT cnt;

		dur = cmt_bits[i] ? cmt_hi1_us : cmt_hi0_us;
		cnt = (dur * 48000 + rem) / 1000000;
		rem = (dur * 48000 + rem) % 1000000;
		for (k = 0; k < cnt; k++) {
			samp[n++] = (UINT16)12000;
		}
		dur = cmt_bits[i] ? cmt_lo1_us : cmt_lo0_us;
		cnt = (dur * 48000 + rem) / 1000000;
		rem = (dur * 48000 + rem) % 1000000;
		for (k = 0; k < cnt; k++) {
			samp[n++] = (UINT16)-12000;
		}
	}
	dsz = (UINT32)total * 2;
	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, "RIFF", 4);
	hdr[4] = (UINT8)((36 + dsz) & 0xff);
	hdr[5] = (UINT8)(((36 + dsz) >> 8) & 0xff);
	hdr[6] = (UINT8)(((36 + dsz) >> 16) & 0xff);
	hdr[7] = (UINT8)(((36 + dsz) >> 24) & 0xff);
	memcpy(hdr + 8, "WAVE", 4);
	memcpy(hdr + 12, "fmt ", 4);
	hdr[16] = 16;
	hdr[20] = 1;			/* PCM */
	hdr[22] = 1;			/* mono */
	hdr[24] = (UINT8)(48000 & 0xff);
	hdr[25] = (UINT8)((48000 >> 8) & 0xff);
	hdr[26] = (UINT8)((48000 >> 16) & 0xff);
	hdr[27] = (UINT8)((48000 >> 24) & 0xff);
	hdr[28] = (UINT8)(96000 & 0xff);
	hdr[29] = (UINT8)((96000 >> 8) & 0xff);
	hdr[30] = (UINT8)((96000 >> 16) & 0xff);
	hdr[31] = (UINT8)((96000 >> 24) & 0xff);
	hdr[32] = 2;			/* block align */
	hdr[34] = 16;			/* bits per sample */
	memcpy(hdr + 36, "data", 4);
	hdr[40] = (UINT8)(dsz & 0xff);
	hdr[41] = (UINT8)((dsz >> 8) & 0xff);
	hdr[42] = (UINT8)((dsz >> 16) & 0xff);
	hdr[43] = (UINT8)((dsz >> 24) & 0xff);
	fp = fopen(path, "wb");
	if (fp != NULL) {
		fwrite(hdr, 1, sizeof(hdr), fp);
		for (i = 0; i < total; i++) {
			UINT8 b[2];

			b[0] = (UINT8)(samp[i] & 0xff);
			b[1] = (UINT8)((samp[i] >> 8) & 0xff);
			fwrite(b, 1, 2, fp);
		}
		fclose(fp);
	}
	free(samp);
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
	if (cmt_path_is_wav(path)) {
		cmt_wav_savefile(path);
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

