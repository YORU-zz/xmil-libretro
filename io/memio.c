#include	"compiler.h"
#include	"dosio.h"
#include	"z80core.h"
#include	"pccore.h"
#include	"iocore.h"
#include	"defipl.res"


	UINT8	biosmem[0x8000];
#if defined(SUPPORT_BANKMEM)
	UINT8	bankmem[16][0x8000];
#endif

/* ---- EMM (expansion memory) - M5 patch, TapeOS project ----
	The stock core leaves ports 0x0d00-0x0dff on the dummy
	handlers, so the EMM device the compatible IPL can boot
	from (IPLDRV=0x10) did not exist. This implements it with
	the exact interface the IPL uses (EMM_LOAD1ST /
	EMM_LOADFILE in X1_compatible_rom.z80):
	  0x0d00  address bits 0..7   (write; read = current)
	  0x0d01  address bits 8..15  (write; read = current)
	  0x0d02  address bits 16..23 (write; read = current)
	  0x0d03  data: read/write the byte at the address,
	          then auto-increment the 24-bit address
	          (the IPL's read loop relies on the increment;
	          writes behave symmetrically).
	Backing store is 1MB; the address register is 24 bits
	and wraps inside the store. Content is zeroed on reset
	(volatile, like real RAM). */

	UINT8	emmmem[0x100000];
static	UINT32	emmaddr;

void IOOUTCALL emm_o(UINT port, REG8 dat) {

	switch (port & 3) {
		case 0:
			emmaddr = (emmaddr & 0xffff00) | dat;
			break;
		case 1:
			emmaddr = (emmaddr & 0xff00ff) | ((UINT32)dat << 8);
			break;
		case 2:
			emmaddr = (emmaddr & 0x00ffff) | ((UINT32)dat << 16);
			break;
		default:
			emmmem[emmaddr & 0xfffff] = dat;
			emmaddr = (emmaddr + 1) & 0xffffff;
			break;
	}
}

REG8 IOINPCALL emm_i(UINT port) {

	REG8	ret;

	switch (port & 3) {
		case 0:
			ret = (REG8)(emmaddr & 0xff);
			break;
		case 1:
			ret = (REG8)((emmaddr >> 8) & 0xff);
			break;
		case 2:
			ret = (REG8)((emmaddr >> 16) & 0xff);
			break;
		default:
			ret = emmmem[emmaddr & 0xfffff];
			emmaddr = (emmaddr + 1) & 0xffffff;
			break;
	}
	return(ret);
}


void memio_update(void) {

#if defined(SUPPORT_BANKMEM)
	if (iocore.s.bankmem & 0x10) {
#endif
		if (!(iocore.s.ppib & 0x10)) {
			z80core.e.memread = biosmem;
		}
		else {
			z80core.e.memread = mainmem;
		}
		z80core.e.memwrite = mainmem;
#if defined(SUPPORT_BANKMEM)
	}
	else {
		z80core.e.memread = bankmem[iocore.s.bankmem & 15];
		z80core.e.memwrite = bankmem[iocore.s.bankmem & 15];
	}
#endif
}

#if defined(SUPPORT_BANKMEM)
void IOOUTCALL memio_bank_o(UINT port, REG8 dat) {

	iocore.s.bankmem = dat;
	memio_update();
	(void)port;
}

REG8 IOINPCALL memio_bank_i(UINT port) {

	(void)port;
	return(iocore.s.bankmem);
}
#endif

void IOOUTCALL memio_rom(UINT port, REG8 dat) {

	iocore.s.ppib &= ~0x10;
#if defined(SUPPORT_BANKMEM)
	if (iocore.s.bankmem & 0x10) {
#endif
		z80core.e.memread = biosmem;
#if defined(SUPPORT_BANKMEM)
	}
#endif
	(void)port;
	(void)dat;
}

void IOOUTCALL memio_ram(UINT port, REG8 dat) {

	iocore.s.ppib |= 0x10;
#if defined(SUPPORT_BANKMEM)
	if (iocore.s.bankmem & 0x10) {
#endif
		z80core.e.memread = mainmem;
#if defined(SUPPORT_BANKMEM)
	}
#endif
	(void)port;
	(void)dat;
}


/* ---- */

static const OEMCHAR iplromx1[] = OEMTEXT("IPLROM.X1");
static const OEMCHAR iplromx1t[] = OEMTEXT("IPLROM.X1T");
#ifdef __LIBRETRO__
extern char retro_system_conf[512];
extern char slash ;
#endif
void memio_reset(void) {

const OEMCHAR	*iplfile;
	UINT		iplsize;
	FILEH		fh;

	ZeroMemory(biosmem, 0x8000);
	CopyMemory(biosmem, defaultiplrom, sizeof(defaultiplrom));
	if (pccore.ROM_TYPE >= 1) {
		if (pccore.ROM_TYPE >= 2) {
			iplfile = iplromx1t;
			iplsize = 0x8000;
		}
		else {
			iplfile = iplromx1;
			iplsize = 0x8000;
		}
#ifdef __LIBRETRO__
	char rtmp[512];
	sprintf(rtmp,"%s%c%s\0",retro_system_conf,slash,iplfile);
		fh = file_open_rb(rtmp);
#else
		fh = file_open_rb_c(iplfile);
#endif
		if (fh != FILEH_INVALID) {
			file_read(fh, biosmem, iplsize);
			file_close(fh);
		}
	}

#if defined(SUPPORT_BANKMEM)
	iocore.s.bankmem = 0x78;
#endif
	ZeroMemory(emmmem, sizeof(emmmem));		/* M5: EMM is volatile */
	emmaddr = 0;
	memio_update();
}

