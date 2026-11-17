/*
 * pin_mux.h - Boot pin setup hook expected by board_init.c.
 *
 * If you generate pin_mux.c/.h with the MCUXpresso Config Tools, replace
 * these two files with the generated ones. The pads this firmware uses are
 * configured in source/board_init.c, so the generated BOARD_InitBootPins()
 * only needs to cover anything else on your board.
 */
#ifndef _PIN_MUX_H_
#define _PIN_MUX_H_

#if defined(__cplusplus)
extern "C" {
#endif

void BOARD_InitBootPins(void);

#if defined(__cplusplus)
}
#endif

#endif /* _PIN_MUX_H_ */
