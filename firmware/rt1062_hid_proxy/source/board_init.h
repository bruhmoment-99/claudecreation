/*
 * board_init.h - Clock, pin and GPIO bring-up for the HID proxy board.
 */
#ifndef BOARD_INIT_H_
#define BOARD_INIT_H_

#include <stdbool.h>
#include <stdint.h>

/* MPU (cache policy), 600 MHz clocks, UART clock root, pins, VBUS GPIO = LOW. */
void board_init(void);

/* Frequency feeding the LPUART baud generator (24 MHz after board_init). */
uint32_t board_uart_clock_hz(void);

/* AP2553W6 EN pin (active high) for the USB-A host port VBUS. */
void board_vbus_enable(bool on);

/* AP2553W6 FLG pin (open drain, active low): true = over-current/thermal fault. */
bool board_vbus_fault(void);

#endif /* BOARD_INIT_H_ */
