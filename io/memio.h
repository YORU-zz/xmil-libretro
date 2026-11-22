
#ifdef __cplusplus
extern "C" {
#endif

extern	UINT8	biosmem[0x8000];
#if defined(SUPPORT_BANKMEM)
extern	UINT8	bankmem[16][0x8000];
#endif

void memio_update(void);

#if defined(SUPPORT_BANKMEM)
void IOOUTCALL memio_bank_o(UINT port, REG8 dat);
REG8 IOINPCALL memio_bank_i(UINT port);
#endif

void IOOUTCALL memio_rom(UINT port, REG8 dat);
void IOOUTCALL memio_ram(UINT port, REG8 dat);

/* EMM (expansion memory, ports 0x0d00-0x0d03) - M5 patch,
   TapeOS project. See memio.c for the protocol notes. */
extern	UINT8	emmmem[0x100000];
void IOOUTCALL emm_o(UINT port, REG8 dat);
REG8 IOINPCALL emm_i(UINT port);

void memio_reset(void);

#ifdef __cplusplus
}
#endif

