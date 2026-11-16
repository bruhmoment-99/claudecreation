/*
 * usb_host_app.h - USB2 (USB_OTG2, EHCI1) host for the physical mouse.
 * (Named *_app.h because the SDK already ships a usb_host.h.)
 */
#ifndef USB_HOST_APP_H_
#define USB_HOST_APP_H_

/*
 * Enumeration / control-transfer state machine task (APP_PRIO_USB_HOST_APP).
 * On start it performs the VBUS power sequence, initialises the host stack and
 * spawns the high-priority stack task that runs USB_HostEhciTaskFunction() -
 * the context in which the interrupt-IN completion callback (the real-time
 * merge path) executes.
 */
void usb_host_app_task(void *arg);

#endif /* USB_HOST_APP_H_ */
