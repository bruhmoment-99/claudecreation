/*
 * app_status.h - What the firmware currently knows about both USB links,
 * for the status LED and cmd.status().
 *
 * The USB code reports events here (PC configured, mouse attached, layout
 * parsed, VBUS fault, ...). Two readers use it:
 *   - status_led.c turns it into a blink pattern (app_status_led_on()),
 *   - cmd.status() prints it (app_status_print()).
 *
 * This module is pure C with no SDK dependency so it runs in the PC unit
 * tests. Writers are single fields or counters updated from one context
 * each; readers take no lock, so a status line printed in the middle of an
 * enumeration can mix old and new values. That is fine for a diagnostic.
 */
#ifndef APP_STATUS_H_
#define APP_STATUS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hid_report_parser.h"

/* USB speeds, numerically equal to the SDK's USB_SPEED_FULL/LOW/HIGH. */
#define APP_USB_SPEED_FULL 0U
#define APP_USB_SPEED_LOW  1U
#define APP_USB_SPEED_HIGH 2U

typedef enum
{
    APP_MOUSE_NONE = 0,    /* nothing attached to the USB-A port          */
    APP_MOUSE_ENUMERATING, /* claimed, class requests in progress         */
    APP_MOUSE_READY,       /* reports are streaming                       */
    APP_MOUSE_UNSUPPORTED, /* attached device has no usable HID interface */
    APP_MOUSE_ERROR,       /* a request failed; see mouseError            */
} app_mouse_state_t;

typedef struct
{
    /* USB1, towards the PC */
    volatile bool pcConfigured;
    volatile uint8_t pcSpeed;       /* APP_USB_SPEED_*                    */
    volatile uint32_t pcReports;    /* reports delivered to the PC        */
    volatile bool pcBootProtocol;   /* BIOS/UEFI selected boot protocol   */
    volatile bool pcSuspended;      /* PC suspended the bus (sleep)       */
    volatile bool pcRemoteWake;     /* PC allowed remote wake-up          */
    volatile uint32_t pcWakeups;    /* remote wake-ups signalled          */

    /* USB2, the physical mouse */
    volatile app_mouse_state_t mouseState;
    const char *volatile mouseError; /* static string, APP_MOUSE_ERROR   */
    uint8_t mouseSpeed;
    uint16_t mouseVid;
    uint16_t mousePid;
    uint8_t mouseInterface;         /* bInterfaceNumber                   */
    bool mouseViaHub;               /* attached behind a hub              */
    bool mouseBootProtocol;         /* layout is the boot fallback        */
    uint16_t mousePacketSize;       /* interrupt IN wMaxPacketSize used   */
    hid_mouse_layout_t layout;      /* valid while READY                  */
    volatile uint32_t mouseReports; /* IN reports received from the mouse */

    /* Command UART */
    volatile uint32_t uartRxBytes;   /* bytes taken from the RX ring      */
    volatile uint32_t uartOverflows; /* times the ring was overrun        */
    volatile uint32_t uartLostBytes; /* bytes lost in those overruns      */
    volatile uint32_t uartHwOverruns;/* LPUART FIFO overruns (should be 0)*/

    /* Saved settings */
    volatile uint16_t settingsSmooth; /* value in flash                   */
    volatile uint32_t settingsSaves;
    volatile uint32_t settingsErrors;

    /* Why the board last started (reset_info.c) */
    const char *resetCause;           /* "power-on", "watchdog", ...      */
    uint32_t resetFatalCode;          /* app_fatal() code before it, or 0 */
    const char *resetStalledTask;     /* task the watchdog gave up on     */

    /* Faults */
    volatile bool hostInitFailed;   /* USB_HostInit() failed              */
    volatile uint32_t vbusFaults;   /* AP2553 over-current events         */
    volatile uint32_t vbusFaultMs;  /* time of the last one               */
} app_status_t;

/* How long a VBUS over-current keeps the LED in the fault pattern. */
#define APP_STATUS_VBUS_FAULT_HOLD_MS 5000U

void app_status_init(void);

/* The one instance (read-only for callers). */
const app_status_t *app_status_get(void);

/* ---- USB1 / PC side (usb_device.c) -------------------------------------- */
void app_status_pc_speed(uint8_t speed);
void app_status_pc_configured(bool configured);
void app_status_pc_report_sent(void);
void app_status_pc_boot_protocol(bool boot);
void app_status_pc_suspended(bool suspended);
void app_status_pc_remote_wake(bool enabled);
void app_status_pc_wakeup(void);

/* ---- USB2 / mouse side (usb_host.c) ------------------------------------- */
/* A device offered a HID interface we claimed; enumeration continues. */
void app_status_mouse_attached(uint16_t vid, uint16_t pid, uint8_t speed, uint8_t interfaceNumber, bool viaHub);
/* A device without a usable HID interface was plugged in. The state stays
 * UNSUPPORTED until the next device is attached (the SDK does not report
 * the detach of a device nobody claimed). */
void app_status_mouse_unsupported(uint16_t vid, uint16_t pid, uint8_t speed);
/* Streaming started with this layout. */
void app_status_mouse_ready(const hid_mouse_layout_t *layout, bool bootProtocol, uint16_t packetSize);
/* reason: a string literal, kept by pointer. */
void app_status_mouse_error(const char *reason);
void app_status_mouse_detached(void);
void app_status_mouse_report(void);

void app_status_host_init_failed(void);
void app_status_vbus_fault(uint32_t nowMs);

/* ---- Command UART (uart_parser.c) ---------------------------------------- */
void app_status_uart(uint32_t rxBytes, uint32_t overflows, uint32_t lostBytes, uint32_t hwOverruns);

/* ---- Settings and reset cause (system task, reset_info.c) ---------------- */
void app_status_settings(uint16_t smoothMs, uint32_t saves, uint32_t errors);
/* cause: static string; fatalCode 0 = none; stalledTask NULL = none. */
void app_status_reset(const char *cause, uint32_t fatalCode, const char *stalledTask);

/* ---- Readers ------------------------------------------------------------ */

/* True if anything in the status counts as a fault for the LED. */
bool app_status_has_fault(const app_status_t *s, uint32_t nowMs);

/*
 * LED pattern (period 2 s, evaluated every few tens of ms):
 *   fault (see app_status_has_fault)       fast blink, 5 Hz
 *   PC suspended the bus (PC asleep)       off
 *   PC configured, mouse ready             solid on
 *   PC configured, no mouse                slow blink, 1 Hz
 *   PC not configured, mouse ready         two short blips every 2 s
 *   PC not configured, no mouse            one short blip every 2 s
 * (Fatal errors are shown separately by app_fatal(): a fast 20 Hz flicker.)
 */
bool app_status_led_on(const app_status_t *s, uint32_t nowMs);

/* Write the multi-line status report, lines separated by "\r\n", with no
 * trailing line break. write() receives pieces of text. */
void app_status_print(const app_status_t *s, uint32_t nowMs, void (*write)(const char *data, size_t len));

#endif /* APP_STATUS_H_ */
