/*
 * board.h - Board definitions for the HID proxy (replaces the SDK's
 * boards/evkmimxrt1060/board.h; only what this firmware uses is kept).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _BOARD_H_
#define _BOARD_H_

#include "clock_config.h" /* BOARD_XTAL0_CLK_HZ, BOARD_BootClockRUN() */
#include "fsl_common.h"
#include "fsl_gpio.h"
#include "fsl_clock.h"

#define BOARD_NAME "HID-Proxy-RT1062"

/* QSPI NOR size. Used by the XIP boot header (fsl_flexspi_nor_boot.h) and
 * by the MPU setup in board.c. Match it to the fitted flash. */
#ifndef BOARD_FLASH_SIZE
#define BOARD_FLASH_SIZE (0x800000U)
#endif

/* USB PHY calibration (same values as the NXP EVKs). */
#define BOARD_USB_PHY_D_CAL     (0x0CU)
#define BOARD_USB_PHY_TXCAL45DP (0x06U)
#define BOARD_USB_PHY_TXCAL45DM (0x06U)

#if defined(__cplusplus)
extern "C" {
#endif

/* Cache attributes for every memory region; enables I/D caches. */
void BOARD_ConfigMPU(void);

#if defined(__cplusplus)
}
#endif

#endif /* _BOARD_H_ */
