/*
 * app_config.h - Central configuration for the i.MX RT1062 USB HID proxy.
 *
 * This header is deliberately SDK-free (no fsl_*.h includes) so the pure-logic
 * modules (hid_merge.c, hid_report_parser.c, uart_parser.c command layer) can be
 * unit-tested on a PC. Anything that needs SDK types (pin mux macros, IRQn
 * values) lives in board_init.c / the respective driver module.
 */
#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

/* ------------------------------------------------------------------------- */
/* Command UART (CH343P bridge)                                              */
/* ------------------------------------------------------------------------- */
#define APP_UART_BAUD               6000000U  /* 6 Mbaud, 8N1                 */
#define APP_UART_RX_RING_SIZE       4096U     /* power of two; ~6.8 ms @6Mbd  */
#define APP_UART_DMA_CHANNEL        0U        /* eDMA channel used for RX     */
#define APP_CMD_MAX_LINE            96U       /* longest accepted command     */

/* ------------------------------------------------------------------------- */
/* Injection behaviour                                                       */
/* ------------------------------------------------------------------------- */
/*
 * 1: an injected command is pushed to the PC right away if the USB1 IN
 *    endpoint is idle (needed when the physical mouse is stationary - a mouse
 *    that is not moving NAKs every poll, so no host-IN completion ever fires).
 * 0: injections are only merged into the next physical report (strict
 *    "piggy-back" mode from the spec; injections stall while the mouse is idle).
 */
#define APP_INJECT_SEND_IMMEDIATE   1

/* ------------------------------------------------------------------------- */
/* USB device (USB1 / USB_OTG1 / kUSB_ControllerEhci0)                       */
/* ------------------------------------------------------------------------- */
/* PLACEHOLDER IDs - you must use a VID/PID you are entitled to use.         */
#define APP_USB_VID                 0x1FC9U   /* NXP VID used by SDK examples */
#define APP_USB_PID                 0x00A0U   /* placeholder                  */
#define APP_USB_BCD_DEVICE          0x0100U

#define APP_HID_IN_EP               1U        /* EP1 IN                       */
#define APP_HID_IN_MPS              8U        /* report is 6 bytes            */
#define APP_HID_IN_INTERVAL_HS      1U        /* 2^(1-1) uframes = 125 us     */
#define APP_HID_IN_INTERVAL_FS      1U        /* 1 frame = 1 ms (fallback)    */

/* ------------------------------------------------------------------------- */
/* USB host (USB2 / USB_OTG2 / kUSB_ControllerEhci1)                         */
/* ------------------------------------------------------------------------- */
#define APP_HOST_REPORT_DESC_MAX    512U      /* bytes reserved for rpt desc  */
#define APP_HOST_REPORT_BUF_SIZE    64U       /* >= mouse interrupt IN MPS    */
#define APP_VBUS_OFF_TIME_MS        100U      /* hold VBUS low (discharge)    */
#define APP_VBUS_SETTLE_MS          20U       /* AP2553 soft-start + margin   */

/* ------------------------------------------------------------------------- */
/* FreeRTOS task priorities (configMAX_PRIORITIES assumed >= 6)              */
/* ------------------------------------------------------------------------- */
#define APP_PRIO_USB_HOST_STACK     5U  /* USB_HostEhciTaskFunction + IN cb   */
#define APP_PRIO_INIT               5U  /* one-shot bring-up task             */
#define APP_PRIO_USB_HOST_APP       4U  /* enumeration state machine          */
#define APP_PRIO_UART_PARSER        3U  /* command parser (medium)            */

#define APP_STACK_USB_HOST_STACK    1024U  /* words */
#define APP_STACK_USB_HOST_APP      1024U
#define APP_STACK_UART_PARSER       768U
#define APP_STACK_INIT              768U

/* ------------------------------------------------------------------------- */
/* NVIC priorities (0 = highest). configPRIO_BITS = 4 on RT1062.             */
/*                                                                           */
/* The USB stacks call OSA/FreeRTOS primitives from their ISRs, so these     */
/* MUST be numerically >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY       */
/* (2 in the SDK's default RT1060 FreeRTOSConfig.h). USB1 gets the highest   */
/* priority FreeRTOS allows so the IN endpoint is re-primed within a few us. */
/* ------------------------------------------------------------------------- */
#define APP_NVIC_PRIO_USB_DEVICE    2U
#define APP_NVIC_PRIO_USB_HOST      3U
#define APP_NVIC_PRIO_UART          5U

/*
 * Optional: place latency-critical code in ITCM to avoid FlexSPI XIP cache
 * misses. With the SDK this is normally done with AT_QUICKACCESS_SECTION_CODE()
 * (fsl_common_arm.h) or a linker-script rule. Left empty by default so the
 * unit tests build on a PC.
 */
#ifndef APP_HOT
#define APP_HOT
#endif

#endif /* APP_CONFIG_H_ */
