/*
 * hid_merge.h - Physical + injected mouse state merger with button locking.
 *
 * This is the single point where the outgoing USB1 report is built. It is
 * called from three contexts:
 *   - USB host stack task  (physical report arrived)        hid_merge_physical()
 *   - UART parser task     (command injected)               hid_merge_inject_*()
 *   - USB1 device ISR      (previous IN transfer completed) hid_merge_on_tx_complete()
 *
 * Shared state is guarded by very short PRIMASK critical sections (a few dozen
 * cycles); the actual USB_DeviceHidSend() call happens outside the lock and
 * exclusivity is guaranteed by a "tx busy" flag claimed inside the lock.
 */
#ifndef HID_MERGE_H_
#define HID_MERGE_H_

#include <stdbool.h>
#include <stdint.h>

/* Wire format sent to the PC - must match the report descriptor in usb_device.c */
typedef struct __attribute__((packed))
{
    uint8_t buttons; /* bit0 L, bit1 R, bit2 M, bit3..7 buttons 4..8 */
    int16_t x;       /* little endian, -32767..32767 (Cortex-M7 is LE) */
    int16_t y;
    int8_t wheel;    /* -127..127 */
} hid_mouse_report_t;

#define HID_BTN_LEFT   0x01U
#define HID_BTN_RIGHT  0x02U
#define HID_BTN_MIDDLE 0x04U

/* Implemented by usb_device.c (stubbed in unit tests). Returns true if the
 * transfer was queued; on true, hid_merge_on_tx_complete() must follow. */
bool usb_device_send_report(const hid_mouse_report_t *report);

void hid_merge_init(void);

/* USB1 configured / de-configured (bus reset, detach, SET_CONFIGURATION 0). */
void hid_merge_set_device_ready(bool ready);

/* Physical mouse report decoded by the host side.
 * buttonsValid=false means this report did not carry the button field
 * (e.g. different report ID) - keep the previous physical button state. */
void hid_merge_physical(uint8_t buttons, bool buttonsValid, int32_t dx, int32_t dy, int32_t wheel);

/* Physical mouse unplugged: release all physical buttons. */
void hid_merge_physical_disconnected(void);

/* Injection API (UART task). */
void hid_merge_inject_move(int32_t dx, int32_t dy);
void hid_merge_inject_wheel(int32_t delta);
void hid_merge_inject_button(uint8_t mask, bool pressed);

/* Logical state = physical OR injected. */
uint8_t hid_merge_get_logical_buttons(void);

/* Called from the USB1 HID class callback (ISR) when an IN transfer finished. */
void hid_merge_on_tx_complete(void);

#endif /* HID_MERGE_H_ */
