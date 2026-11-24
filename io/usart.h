
/* i8251 USART for the X1 RS-232C expansion board (ep.75) -
   RS-232C patch, TapeOS project. See usart.c for the model. */

#ifdef __cplusplus
extern "C" {
#endif

void IOOUTCALL usart_o(UINT port, REG8 dat);
REG8 IOINPCALL usart_i(UINT port);

void usart_reset(void);

#ifdef __cplusplus
}
#endif
