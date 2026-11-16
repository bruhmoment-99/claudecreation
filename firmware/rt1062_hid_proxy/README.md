# i.MX RT1062 USB HID proxy (FreeRTOS, MCUXpresso SDK)

Passes a physical USB mouse (USB2, host) through to a PC (USB1, High-Speed
device, 8 kHz polling). Movement, wheel and button commands sent as text over
LPUART3 (CH343P, 6 Mbaud) get merged into the stream.

```
 Physical mouse ──USB-A──► USB2 / EHCI1 (host) ─┐
   (AP2553W6 VBUS switch on GPIO)               │ parse report ─► hid_merge ─► USB1 / EHCI0 (device) ──USB-C──► PC
                                                │                   ▲              HS, EP1 IN, bInterval=1 (125 µs)
 Control PC ──CH343P──6 Mbaud──► LPUART3 ─eDMA ring─► uart_parser ──┘
```

## Files

| File | Role |
|------|------|
| `source/main.c` | Starts the scheduler and an init task that brings up USB1, the UART and USB2 in that order |
| `source/board_init.c/.h` | MPU/clocks, 24 MHz UART clock root (gives exactly 6 Mbaud), pin mux, AP2553 EN (starts LOW) and FLG pins |
| `source/usb_device.c` + `usb_device_app.h` | USB1 HID mouse: all descriptors, SDK class tables, ch9 and HID callbacks, `USB_OTG1_IRQHandler` |
| `source/usb_host.c` + `usb_host_app.h` | USB2 host: VBUS sequencing, interface claim, enumeration state machine, real-time IN callback, `USB_OTG2_IRQHandler` |
| `source/hid_report_parser.c/.h` | Reads the mouse's report descriptor to find the buttons, X, Y and wheel fields (report IDs, 12-bit packed axes and so on) |
| `source/hid_merge.c/.h` | Merges physical and injected input and handles button locking. It is the only code that builds the USB1 report |
| `source/uart_parser.c/.h` | Command grammar and responses (pure C) plus the LPUART3 eDMA ring driver and the parser task |
| `source/app_config.h` | Priorities, sizes, VID/PID and behaviour switches. It includes no SDK headers |
| `test/` | Unit tests that run on a PC for the merger, the descriptor parser and the command parser (`make -C test`) |

The headers are named `*_app.h` because the SDK already ships `usb_device.h`
and `usb_host.h`.

## Execution contexts

| Context | Priority | Work |
|---------|----------|------|
| `USB_OTG1_IRQHandler` | NVIC 2 | USB1 device stack. When a send completes it calls `hid_merge_on_tx_complete()`, which re-primes EP1 IN |
| `USB_OTG2_IRQHandler` | NVIC 3 | Signals the host stack task |
| `LPUART3_IRQHandler` | NVIC 5 | Idle-line interrupt wakes the UART task. Received bytes arrive by DMA |
| `usbh_stack` task | 5 (highest) | `USB_HostEhciTaskFunction()`. Runs the mouse IN callback: re-arm, parse, merge, `USB_DeviceHidSend()` |
| `usbh_app` task | 4 | VBUS LOW→HIGH, `USB_HostInit`, SET_INTERFACE / GET_DESCRIPTOR(Report) / SET_IDLE / SET_PROTOCOL, detach, VBUS fault |
| `uart_cmd` task | 3 | Command parser |

The USB IRQ priorities must be numerically **≥
`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`**, because the SDK USB stacks
call OSA/FreeRTOS functions from those ISRs.

## Button locking

`hid_merge` keeps `physButtons`, `injButtons` and `lastSent`. A report goes to
the PC only when there is motion to deliver or when
`(physButtons | injButtons) != lastSent`. So:

* `cmd.left(1)` while the physical left button is already down sends nothing.
* Releasing the physical button while `cmd.left(1)` is active sends nothing,
  and the PC keeps seeing the button held.
* `cmd.left(0)` sends the release, but only if the physical button is also up.

Motion from the mouse and from commands is added to accumulators. Each report
takes what it carries out of them, so nothing is repeated. Values larger than
the report range (±32767 for X/Y, ±127 for the wheel) are split across
consecutive reports, so nothing is lost either. If EP1 is still busy, new
deltas add to the accumulator and go out with the next completion.

**Deviation from the spec (configurable):** a mouse that isn't moving NAKs
every poll, so no host-IN completion ever happens. If injections were applied
only on physical reports, they would stall until the mouse moved. With
`APP_INJECT_SEND_IMMEDIATE = 1` (the default), a command is sent right away
whenever EP1 is idle. Set it to `0` for strict "apply only on the next
physical report" behaviour.

## 8 kHz polling

For a High-Speed interrupt endpoint, `bInterval = 1` means a poll every
2^(1-1) = 1 microframe, which is 125 µs. The EHCI device controller answers IN
tokens **in hardware** from a primed dTD, and NAKs when nothing is primed (to
HID, a NAK means "no change"). The CPU only has to re-prime after each
completion. That path is ISR → class callback → `hid_merge_on_tx_complete()`
→ `USB_DeviceHidSend()`: a few microseconds at 600 MHz. Keep it fast:

* Leave USB_OTG1 at the highest priority FreeRTOS allows. The `hid_merge`
  locks mask interrupts for well under 1 µs.
* Optionally put the ISR path in ITCM to avoid FlexSPI cache misses
  (`APP_HOT` in `app_config.h`, e.g. via `AT_QUICKACCESS_SECTION_CODE`).
* All USB/DMA buffers use `USB_DMA_*_DATA_ALIGN` / `AT_NONCACHEABLE_SECTION*`.
  These need `BOARD_ConfigMPU()` to have set up the non-cacheable region.

The PC actually receives reports at the rate of the physical mouse plus the
injections. 8 kHz is the polling ceiling, not a report rate the firmware
generates on its own.

## SDK configuration

Copy the SDK templates `middleware/usb/config/device/ehci/usb_device_config.h`
and `.../host/ehci/usb_host_config.h` into your project and set:

```c
/* usb_device_config.h */
#define USB_DEVICE_CONFIG_EHCI        (1U)
#define USB_DEVICE_CONFIG_HID         (1U)   /* template default is 0 */
#define USB_DEVICE_CONFIG_ENDPOINTS   (4U)
#define USB_DEVICE_CONFIG_USE_TASK    (0U)   /* callbacks in ISR */

/* usb_host_config.h */
#define USB_HOST_CONFIG_EHCI          (1U)
#define USB_HOST_CONFIG_HID           (1U)
#define USB_HOST_CONFIG_MAX_TRANSFERS (16U)  /* >= 2 free for IN re-arm */
```

Add these SDK components: `usb_device_ehci`, `usb_device_hid`,
`usb_device_ch9`, `usb_host_ehci`, `usb_host_hid`, `usb_phy`,
`osa_free_rtos`, `lists`, `lpuart`, `edma`, `dmamux`, `igpio`, plus
FreeRTOS (heap_4, ≥ 24 KB). Preprocessor: `CPU_MIMXRT1062DVJ6B`,
`SDK_OS_FREE_RTOS`, `USE_RTOS=1`, `FSL_OSA_TASK_ENABLE=1`,
`XIP_EXTERNAL_FLASH=1`, `DATA_SECTION_IS_CACHEABLE=1`.

## Things to adapt to your board

* **Pins** (`board_init.c`): LPUART3 is on GPIO_AD_B1_06/07, AP2553 EN on
  GPIO_AD_B1_10 (GPIO1_IO26) and FLG on GPIO_AD_B1_11. These are examples.
  Fit an external pull-down on EN, because the pad is high-Z during reset.
* **VID/PID** (`app_config.h`): placeholders. Use IDs you are entitled to.
* **Boot protocol**: the USB1 interface is declared non-boot (subclass 0),
  because the 16-bit report isn't boot-compatible. BIOS/UEFI support would
  need subclass 1 / protocol 2 and a 3-byte report while boot protocol is
  selected.
* **Host side**: one mouse, no hub. The interface with protocol "mouse" is
  preferred. If there isn't one, the first HID interface with protocol 0 is
  used, and its report descriptor must contain a Mouse/Pointer collection.
  If the descriptor can't be parsed, a boot-capable interface falls back to
  the boot protocol layout.
* **Possible improvements**: horizontal scroll (AC Pan) in the parser and the
  report; keeping two host IN transfers queued for 8 kHz HS mice; DMA for
  UART TX.

## Command protocol

One command per line, terminated by `\r\n`. A command with a valid name and
valid arguments is echoed back, followed by `\r\n>>> `. Queries and
`version`/`info` put their value on its own line before the prompt.
**Anything invalid gets no reply at all and changes nothing**: an unknown
name, bad or out-of-range arguments, or a line longer than 96 characters.

| Command | Effect | Reply |
|---------|--------|-------|
| `cmd.left(v)` `cmd.right(v)` `cmd.middle(v)` | Injected button 1/2/3, `v` = 1 press / 0 release | echo |
| `cmd.side1(v)` | Button 4 (Back), report bit 3 | echo |
| `cmd.side2(v)` | Button 5 (Forward), report bit 4 | echo |
| `cmd.left()` … `cmd.side2()` | Query | `0` or `1` (physical OR injected) |
| `cmd.buttons(mask)` | Replace the whole injected button byte (0–255) | echo |
| `cmd.buttons()` | Query | logical button byte |
| `cmd.release()` | Clear every injected button | echo |
| `cmd.move(dx,dy)` / `cmd.wheel(d)` | Add motion | echo |
| `cmd.sleep(ms)` | Prompt goes out first, then the parser task sleeps (0–600000 ms) | echo |
| `cmd.version()` / `cmd.info()` | | `1.0.0` / `HID-Proxy-RT1062` |

All button commands follow the button-locking rule. `release()` and
`buttons(mask)` only replace the *injected* state. A button held on the
physical mouse stays pressed, and a report goes out only if the logical byte
changes.

`cmd.sleep` blocks only the parser task. Physical forwarding (host stack task
and USB ISRs) keeps running, and the DMA ring keeps receiving. Commands sent
during the sleep run in order once it ends. The ring holds 4096 bytes (roughly
200 commands), and anything past that during a single sleep is lost.

```
cmd.side1(1)\r\n   ->  cmd.side1(1)\r\n>>> 
cmd.buttons()\r\n  ->  cmd.buttons()\r\n8\r\n>>> 
cmd.version()\r\n  ->  cmd.version()\r\n1.0.0\r\n>>> 
cmd.left(2)\r\n    ->  (nothing)
```

## Verification status

* `make -C test`: all unit tests pass. They also pass under
  ASan/UBSan.
* Every file in `source/` passes `clang --target=arm-none-eabi
  -mcpu=cortex-m7 -fsyntax-only -Wall -Wextra -Wconversion` against the
  current MCUXpresso SDK (mcux-sdk, mcux-sdk-middleware-usb, FreeRTOS-Kernel,
  CMSIS 5) with zero warnings in project code.
* **Not yet done:** linking a full image and testing on hardware.
