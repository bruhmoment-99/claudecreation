/*
 * usb_device_app.h - USB1 (USB_OTG1, EHCI0) High-Speed HID mouse towards the PC.
 */
#ifndef USB_DEVICE_APP_H_
#define USB_DEVICE_APP_H_

#include <stdbool.h>
#include "hid_merge.h"

/* Clocks + PHY, class init, NVIC, USB_DeviceRun(). Call from a task context. */
void usb_device_app_init(void);

/* Called every few ms from the system task: detects resume after suspend
 * and performs remote wake-up when a button was pressed while suspended. */
void usb_device_poll(uint32_t nowMs);

/* bool usb_device_send_report(const hid_mouse_report_t *r) is declared in
 * hid_merge.h and implemented in usb_device.c.
 * (Named *_app.h because the SDK already ships a usb_device.h.) */

#endif /* USB_DEVICE_APP_H_ */
