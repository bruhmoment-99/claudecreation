/*
 * hid_merge.h - Physical + injected mouse state merger with button locking.
 *
 * This is the single point where the outgoing USB1 report is built. It is
 * called from these contexts:
 *   - USB host stack task  (physical report arrived)        hid_merge_physical()
 *   - UART parser task     (command injected)               hid_merge_inject_*()
 *   - PIT ISR              (smoothed move step)             hid_merge_inject_move()
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

/* Wire format sent to the PC in report protocol - must match the report
 * descriptor in usb_device.c. In boot protocol usb_device.c sends only the
 * first three bytes' worth: buttons 1-8, X and Y as int8 (hid_merge limits
 * X/Y to +-127 and drops wheel/pan while boot protocol is selected). */
typedef struct __attribute__((packed))
{
    uint16_t buttons; /* bit n = button n+1 (1 L, 2 R, 3 M, 4 Back, 5 Fwd, ...) */
    int16_t x;        /* little endian, -32767..32767 (Cortex-M7 is LE) */
    int16_t y;
    int8_t wheel;     /* -127..127, vertical scroll                    */
    int8_t pan;       /* -127..127, horizontal scroll (AC Pan)         */
} hid_mouse_report_t;

#define HID_BTN_LEFT   0x0001U
#define HID_BTN_RIGHT  0x0002U
#define HID_BTN_MIDDLE 0x0004U
#define HID_BTN_SIDE1  0x0008U /* button 4 - "Back"    */
#define HID_BTN_SIDE2  0x0010U /* button 5 - "Forward" */
#define HID_BUTTON_COUNT 16U

/* Implemented by usb_device.c (stubbed in unit tests). Returns true if the
 * transfer was queued; on true, hid_merge_on_tx_complete() must follow. */
bool usb_device_send_report(const hid_mouse_report_t *report);

/* Implemented by usb_device.c (stubbed in unit tests): a button was pressed
 * while the PC has the bus suspended. Must not block. */
void usb_device_request_wakeup(void);

void hid_merge_init(void);

/* USB1 configured / de-configured (bus reset, detach, SET_CONFIGURATION 0). */
void hid_merge_set_device_ready(bool ready);

/* PC selected boot protocol (true) or report protocol (false). */
void hid_merge_set_boot_protocol(bool boot);

/* PC suspended / resumed the bus. While suspended nothing is sent, motion is
 * discarded, and a new button press calls usb_device_request_wakeup(). */
void hid_merge_set_suspended(bool suspended);

/* Physical mouse report decoded by the host side. validMask says which
 * buttons this report carries; the others keep their previous state
 * (mice that split buttons over several report IDs). */
void hid_merge_physical(uint16_t buttons, uint16_t validMask, int32_t dx, int32_t dy, int32_t wheel, int32_t pan);

/* Physical mouse unplugged: release all physical buttons. */
void hid_merge_physical_disconnected(void);

/* Injection API (UART task, smooth-move tick). */
void hid_merge_inject_move(int32_t dx, int32_t dy);
void hid_merge_inject_wheel(int32_t delta);
void hid_merge_inject_pan(int32_t delta);
void hid_merge_inject_button(uint16_t mask, bool pressed);
/* Replace the whole injected button word (cmd.buttons(mask), cmd.release()). */
void hid_merge_set_injected_buttons(uint16_t mask);

/* cmd.reset(): release every injected button and drop motion that has not
 * been delivered yet. Physically held buttons stay pressed. */
void hid_merge_reset_injected(void);

/* Logical state = physical OR injected. */
uint16_t hid_merge_get_logical_buttons(void);

/* Called from the USB1 HID class callback (ISR) when an IN transfer finished. */
void hid_merge_on_tx_complete(void);

#endif /* HID_MERGE_H_ */
