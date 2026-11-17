/*
 * pin_mux.c - Boot pin setup. Intentionally minimal: the pads used by the
 * firmware (LPUART3, AP2553 EN/FLG, status LED) are muxed in
 * source/board_init.c so that all board-specific pins are in one place.
 */
#include "pin_mux.h"
#include "fsl_common.h"
#include "fsl_iomuxc.h"

void BOARD_InitBootPins(void)
{
    CLOCK_EnableClock(kCLOCK_Iomuxc);
}
