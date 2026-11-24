
/* ---- i8251 USART: X1 RS-232C expansion board (ep.75) ----
	RS-232C patch, TapeOS project.

	The novel's ep.75 board carries an Intel 8251 USART; the
	novel gives neither its I/O address nor its baud rate, so
	both are reproduction-side choices, recorded in
	interface-spec.md and rs232c.md:

	  0x0c00  data register    (read = received byte,
	                            write = byte to transmit)
	  0x0c01  control (write) / status (read)
	          (the 8251's C/D line is port bit 0; the rest of
	          the 0x0c page mirrors these two registers)

	Page 0x0c is free in this core for every ROM type: the
	device cluster starts at 0x0d (EMM), 0x0b is bank memory
	on turbo-class machines and 0x07 is the OPM when the
	sound switch is on.

	Host bridge: when the environment variable XMIL_SERIAL
	names a character device (in practice the slave side of
	a PTY whose master a host terminal program holds), the
	board is wired to it. The device is switched to raw mode
	so bytes pass untranslated. With XMIL_SERIAL unset the
	board is present but unconnected: transmissions drain
	into the void on schedule, RxRDY never sets and the DSR
	status bit reads 0 - the existing headless regression
	suites are unaffected.

	Baud model: the board's baud-rate generator is taken as
	a fixed 153,600 Hz TxC/RxC clock, so the mode word's
	divider gives the nominal baud (x16 -> 9600 baud, the
	period default). The divider is accepted and honoured
	in the emulated timing of TxRDY / TxEMPTY (a frame of
	start + data + parity + stop bits at the nominal baud),
	but the host side moves bytes as fast as the PTY does:
	the nominal rate shapes status timing only.

	Register model, following the Intel datasheet:
	  - after reset (or an internal-reset command) the
	    first control write is the mode instruction; in
	    sync mode 1-2 sync characters follow; then control
	    writes are command instructions.
	  - status: TxRDY = transmit holding buffer empty
	    (the status bit is not gated by TxEN or CTS, per
	    the datasheet; only the TxRDY pin is), RxRDY =
	    receive FIFO non-empty while the receiver is
	    enabled, TxEMPTY = holding buffer and shift
	    register both empty, PE/OE/FE = latched error
	    flags (a PTY reports no parity/framing errors, so
	    only OE can occur, when the receive FIFO overruns),
	    SYNDET = 0 (async use), DSR = host device attached.
	  - the transmitter is a holding buffer + shift
	    register pair: a write fills the buffer, the byte
	    moves to the shift register when it frees, and
	    TxEMPTY rises one frame time after the last bit.
	    Bytes leave for the host when their frame time has
	    elapsed; if the host is not draining (write would
	    block) the byte stays in the shift register and
	    TxEMPTY stays low, as on a real wire.
	  - command bits honoured: TxEN (writes are ignored
	    while 0), RxE (receiver enable; while 0 the host
	    input is simply not read), ER (clear error flags),
	    IR (internal reset). DTR/RTS/SBRK drive pins this
	    board does not surface and are accepted silently.
	    Enter-hunt is accepted (sync mode is modelled only
	    as far as its setup sequence; the board's working
	    mode is async). */

#include	"compiler.h"
#include	"pccore.h"
#include	"iocore.h"
#include	"z80core.h"
#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#if defined(OSDWIN32) || defined(_WIN32)
#else
#include	<fcntl.h>
#include	<unistd.h>
#include	<errno.h>
#include	<termios.h>
#endif

#define	USART_BAUDCLK	153600		/* fixed TxC/RxC of the board */
#define	USART_RXFIFO	4096
#define	USART_RETRY_CLK	2000		/* host-not-draining retry */

typedef struct {
	UINT8	mode;		/* mode instruction byte */
	UINT8	cmd;		/* last command instruction byte */
	UINT8	phase;		/* 0=mode, 1=sync chars, 2=command */
	UINT8	syncleft;	/* sync characters still expected */
	UINT8	err;		/* latched PE(08) OE(10) FE(20) */
	UINT8	rxlast;		/* last byte read (empty-read value) */
	UINT	txhold;		/* holding buffer (0x100 = empty) */
	UINT	txshift;	/* shift register (0x100 = empty) */
	UINT32	txshiftdone;	/* clock when the frame has left */
	UINT8	rxfifo[USART_RXFIFO];
	UINT	rxhead;
	UINT	rxtail;
} USART;

static	USART	usart;
static	int	usart_fd = -1;	/* host device, -1 = unconnected */

/* clocks per transmitted frame at the nominal baud */
static UINT32 usart_frameclk(void) {

	UINT32	clk;
	UINT32	baud;
	UINT	bits;
	UINT	factor;

	clk = xmilcfg.X1_CPU_CLOCK;
	if (clk == 0) {
		clk = 4000000;
	}
	switch (usart.mode & 3) {
		case 1:	factor = 1; break;
		case 2:	factor = 16; break;
		case 3:	factor = 64; break;
		default: factor = 16; break;	/* sync: not timed */
	}
	baud = USART_BAUDCLK / factor;
	if (baud == 0) {
		baud = 1;
	}
	bits = 1					/* start */
	     + 5 + ((usart.mode >> 2) & 3)		/* data 5..8 */
	     + ((usart.mode & 0x10) ? 1 : 0);		/* parity */
	/* stop bits: mode 00=invalid (count 1), 01=1, 10=1.5,
	   11=2; 1.5 is rounded up to 2 in whole-bit accounting,
	   which only shifts TxEMPTY by half a bit time. */
	bits += (((usart.mode >> 6) & 3) >= 2) ? 2 : 1;
	return((clk * bits) / baud);	/* clk*bits <= ~5.2e7 */
}

static int usart_rxcount(void) {

	return((int)((usart.rxhead + USART_RXFIFO - usart.rxtail)
			% USART_RXFIFO));
}

/* Move bytes between the chip model and the host device.
   Called on every register access: the guest polls the
   status register in its wait loops, which is exactly
   when a real board's wires would be moving. */
static void usart_pump(void) {

	UINT32	now;
#if !defined(OSDWIN32) && !defined(_WIN32)
	UINT8	buf[512];
	ssize_t	n;
	ssize_t	i;
#endif

	now = (UINT32)CPU_CLOCKCOUNT;

	/* transmitter: retire frames whose time has come */
	while (1) {
		if (usart.txshift & 0x100) {
			break;				/* shift empty */
		}
		if ((SINT32)(now - usart.txshiftdone) < 0) {
			break;				/* still sending */
		}
#if !defined(OSDWIN32) && !defined(_WIN32)
		if (usart_fd >= 0) {
			UINT8	b = (UINT8)usart.txshift;
			n = write(usart_fd, &b, 1);
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				usart.txshiftdone = now + USART_RETRY_CLK;
				break;
			}
			/* other errors: the byte is gone, as it would
			   be on a wire whose far end vanished */
		}
#endif
		usart.txshift = 0x100;
		if (!(usart.txhold & 0x100)) {
			usart.txshift = usart.txhold;
			usart.txhold = 0x100;
			usart.txshiftdone = now + usart_frameclk();
		}
	}

	/* receiver: pull whatever the host has sent */
#if !defined(OSDWIN32) && !defined(_WIN32)
	if ((usart_fd >= 0) && (usart.cmd & 0x04)) {	/* RxE */
		while (1) {
			n = read(usart_fd, buf, sizeof(buf));
			if (n <= 0) {
				break;
			}
			for (i = 0; i < n; i++) {
				UINT	next = (usart.rxhead + 1) % USART_RXFIFO;
				if (next == usart.rxtail) {
					usart.err |= 0x10;	/* OE */
					continue;
				}
				usart.rxfifo[usart.rxhead] = buf[i];
				usart.rxhead = next;
			}
			if (n < (ssize_t)sizeof(buf)) {
				break;
			}
		}
	}
#endif
}

static void usart_open(void) {

#if !defined(OSDWIN32) && !defined(_WIN32)
	const char	*path;
	struct termios	tio;

	if (usart_fd >= 0) {
		close(usart_fd);
		usart_fd = -1;
	}
	path = getenv("XMIL_SERIAL");
	if ((path == NULL) || (path[0] == '\0')) {
		return;
	}
	usart_fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (usart_fd < 0) {
		return;
	}
	if (isatty(usart_fd) && (tcgetattr(usart_fd, &tio) == 0)) {
		cfmakeraw(&tio);
		tcsetattr(usart_fd, TCSANOW, &tio);
	}
#endif
}

void usart_reset(void) {

	usart.mode = 0;
	usart.cmd = 0;
	usart.phase = 0;
	usart.syncleft = 0;
	usart.err = 0;
	usart.rxlast = 0;
	usart.txhold = 0x100;
	usart.txshift = 0x100;
	usart.txshiftdone = 0;
	usart.rxhead = 0;
	usart.rxtail = 0;
	usart_open();
}

static REG8 usart_status(void) {

	REG8	ret = 0;

	if (usart.txhold & 0x100) {
		ret |= 0x01;				/* TxRDY */
	}
	if ((usart.cmd & 0x04) && (usart_rxcount() > 0)) {
		ret |= 0x02;				/* RxRDY */
	}
	if ((usart.txhold & 0x100) && (usart.txshift & 0x100)) {
		ret |= 0x04;				/* TxEMPTY */
	}
	ret |= usart.err & 0x38;			/* PE/OE/FE */
	if (usart_fd >= 0) {
		ret |= 0x80;				/* DSR */
	}
	return(ret);
}

static void usart_ctrl(REG8 dat) {

	if (usart.phase == 0) {				/* mode */
		usart.mode = dat;
		usart.err = 0;
		if ((dat & 3) == 0) {			/* sync mode */
			usart.syncleft = (dat & 0x40) ? 1 : 2;
			usart.phase = 1;
		}
		else {
			usart.phase = 2;
		}
		return;
	}
	if (usart.phase == 1) {				/* sync chars */
		if (--usart.syncleft == 0) {
			usart.phase = 2;
		}
		return;
	}
	/* command instruction */
	if (dat & 0x40) {				/* IR */
		usart.mode = 0;
		usart.cmd = 0;
		usart.phase = 0;
		usart.err = 0;
		usart.txhold = 0x100;
		usart.txshift = 0x100;
		usart.rxhead = 0;
		usart.rxtail = 0;
		return;
	}
	usart.cmd = dat;
	if (dat & 0x10) {				/* ER */
		usart.err = 0;
	}
}

void IOOUTCALL usart_o(UINT port, REG8 dat) {

	usart_pump();
	if (port & 1) {
		usart_ctrl(dat);
		return;
	}
	/* data write */
	if (!(usart.cmd & 0x01)) {			/* TxEN */
		return;
	}
	if (usart.txhold & 0x100) {
		if (usart.txshift & 0x100) {
			usart.txshift = dat;
			usart.txshiftdone = (UINT32)CPU_CLOCKCOUNT
					  + usart_frameclk();
		}
		else {
			usart.txhold = dat;
		}
	}
	/* a write while the holding buffer is full is lost,
	   as on the real chip */
}

REG8 IOINPCALL usart_i(UINT port) {

	usart_pump();
	if (port & 1) {
		return(usart_status());
	}
	/* data read */
	if (usart_rxcount() > 0) {
		usart.rxlast = usart.rxfifo[usart.rxtail];
		usart.rxtail = (usart.rxtail + 1) % USART_RXFIFO;
	}
	return(usart.rxlast);
}
