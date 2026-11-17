# i.MX RT1062 USB HID proxy (FreeRTOS, MCUXpresso SDK)

Passes a physical USB mouse (USB2, host) through to a PC (USB1, High-Speed
device, 8 kHz polling). Movement, scroll and button commands sent as text over
LPUART3 (CH343P, 6 Mbaud) get merged into the stream. The same UART also
updates the firmware.

```
 Physical mouse ──USB-A──► USB2 / EHCI1 (host) ─┐
   (AP2553W6 VBUS switch on GPIO)               │ parse report ─► hid_merge ─► USB1 / EHCI0 (device) ──USB-C──► PC
                                                │                   ▲              HS, EP1 IN, bInterval=1 (125 µs)
 Control PC ──CH343P──6 Mbaud──► LPUART3 ─eDMA ring─► uart_parser ──┘
```

## Building

The project builds with CMake and the GNU Arm toolchain against the
open-source MCUXpresso SDK 2.16 (fetched from NXP's GitHub, not vendored).

```sh
# once: arm-none-eabi-gcc 10 or newer, CMake 3.20+, Ninja (or make)
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi cmake ninja-build   # Debian/Ubuntu

cd firmware/rt1062_hid_proxy
tools/fetch_sdk.sh                                   # SDK -> ./sdk (about 1.7 GB, shallow clones)
cmake -B build -G Ninja                              # Release (-O2 -g) by default
cmake --build build
```

Outputs in `build/`:

| File | Use |
|------|-----|
| `hid_proxy.elf` | Debugging, and flashing with a probe (J-Link, LinkServer, pyOCD) |
| `hid_proxy.bin` | Raw image. Write it to the start of the QSPI NOR (0x60000000) |
| `hid_proxy.hex` | Same image with addresses, for tools that take Intel HEX |
| `hid_proxy.map` | Linker map: memory use per section and symbol |

The image boots by XIP from FlexSPI NOR. It starts with the flash
configuration block (FCB) and the boot image vector table (IVT), so set the
boot pins/fuses to boot from FlexSPI NOR and no other step is needed. With
the NXP MCUBootUtility or `blhost`, write `hid_proxy.bin` at 0x60000000.

Build options (`cmake -B build -D<option>=<value>`):

| Option | Default | Effect |
|--------|---------|--------|
| `CMAKE_BUILD_TYPE` | `Release` | `Debug` (-Og -g3, `assert()` active), `Release` (-O2 -g), `MinSizeRel` (-Os) |
| `HID_PROXY_ITCM` | `OFF` | Run the latency-critical USB path (`APP_HOT`) from ITCM instead of flash |
| `BOARD_FLASH_SIZE` | `0x800000` | QSPI NOR size in bytes, written into the boot header |
| `MCUX_SDK_DIR` | `./sdk` | Where `tools/fetch_sdk.sh` put the SDK |
| `ARM_TOOLCHAIN_DIR` | (PATH) | Directory holding `arm-none-eabi-gcc`, if not on PATH |

Project code is compiled with `-Wall -Wextra -Wconversion -Werror`; SDK
code with its own (looser) settings.

## Files

| File | Role |
|------|------|
| `CMakeLists.txt`, `cmake/arm-none-eabi.cmake` | Build and toolchain definition |
| `tools/fetch_sdk.sh` | Clones the four SDK repositories (`mcux-sdk`, `mcux-sdk-middleware-usb`, `FreeRTOS-Kernel`, `CMSIS_5`) at tag `MCUX_2.16.000` |
| `config/` | `FreeRTOSConfig.h`, `usb_device_config.h`, `usb_host_config.h` (SDK templates, edited for this firmware) |
| `board/` | Clock setup (600 MHz, from the EVK), MPU/cache setup, QSPI boot header (FCB) and a minimal `pin_mux` |
| `linker/MIMXRT1062xxxxx_flexspi_nor.ld` | SDK linker script: code in QSPI NOR (limited to the 1 MB image slot), data and DMA buffers in DTCM, plus a `.noinit` section |
| `source/main.c` | Starts the scheduler and an init task that brings up USB1, the UART and USB2 in that order |
| `source/board_init.c/.h` | MPU/clocks, 24 MHz UART clock root (gives exactly 6 Mbaud), pin mux, AP2553 EN (starts LOW) and FLG pins |
| `source/usb_device.c` + `usb_device_app.h` | USB1 HID mouse: all descriptors, SDK class tables, ch9 and HID callbacks, `USB_OTG1_IRQHandler` |
| `source/usb_host.c` + `usb_host_app.h` | USB2 host: VBUS sequencing, interface claim, enumeration state machine, real-time IN callback, `USB_OTG2_IRQHandler` |
| `source/hid_report_parser.c/.h` | Reads the mouse's report descriptor to find buttons 1–16 (bit fields or arrays, across report IDs), X, Y, wheel and horizontal scroll |
| `source/hid_merge.c/.h` | Merges physical and injected input and handles button locking. It is the only code that builds the USB1 report |
| `source/smooth_move.c/.h` | Optional eased rendering of `cmd.move`: a fixed-point smoothstep lookup table, a segment FIFO and an 8 kHz PIT tick. Off by default |
| `source/uart_parser.c/.h` | Command grammar and responses (pure C) plus the LPUART3 eDMA ring driver and the parser task |
| `source/app_status.c/.h` | What the firmware knows about both links (PC configured, link speed, mouse, parsed layout, faults). Drives the LED and `cmd.status()` |
| `source/status_led.c/.h` | `app_fatal()` (crash handler), the FreeRTOS failure hooks and the millisecond clock |
| `source/system_task.c/.h` | Lowest-priority housekeeping: LED, suspend/remote wake-up, saving settings, feeding the watchdog |
| `source/watch.c/.h`, `source/watchdog.c/.h` | Which tasks must check in (pure C), and WDOG1 plus the reset cause |
| `source/settings.c/.h` | Settings log in flash (pure C, `cmd.smooth`) |
| `source/fw_update.c/.h`, `source/crc32.c/.h` | Firmware update protocol over the UART (pure C) |
| `source/flash_ops.c/.h` | QSPI erase/program through the boot ROM driver, and the ITCM routine that installs a new image |
| `source/flash_if.h` | The flash operations the pure modules use (a RAM fake in the tests) |
| `source/app_config.h` | Priorities, sizes, VID/PID and behaviour switches. It includes no SDK headers |
| `test/` | Unit tests that run on a PC for everything above that is pure C (`make -C test`), and `sim_board.c`, the command channel on a pseudo-terminal (`make -C test sim`) |
| `tools/fw_update.py` | Sends a new `.bin` to the board over the command UART |

The headers are named `*_app.h` because the SDK already ships `usb_device.h`
and `usb_host.h`.

## Execution contexts

| Context | Priority | Work |
|---------|----------|------|
| `USB_OTG1_IRQHandler` | NVIC 2 | USB1 device stack. When a send completes it calls `hid_merge_on_tx_complete()`, which re-primes EP1 IN |
| `USB_OTG2_IRQHandler` | NVIC 3 | Signals the host stack task |
| `PIT_IRQHandler` | NVIC 4 | Smooth-move tick at 8 kHz. Runs only while `cmd.smooth(ms)` > 0 |
| `LPUART3_IRQHandler` | NVIC 5 | Idle-line interrupt wakes the UART task. Received bytes arrive by DMA |
| `DMA0_DMA16_IRQHandler` | NVIC 5 | Counts laps of the RX ring (overflow detection) |
| `usbh_stack` task | 5 (highest) | `USB_HostEhciTaskFunction()`. Runs the mouse IN callback: re-arm, parse, merge, `USB_DeviceHidSend()` |
| `usbh_app` task | 4 | VBUS LOW→HIGH, `USB_HostInit`, SET_INTERFACE / GET_DESCRIPTOR(Report) / SET_IDLE / SET_PROTOCOL, detach, VBUS fault |
| `uart_cmd` task | 3 | Command parser; firmware update receiver |
| `system` task | 1 | Every 10 ms: LED, suspend/resume and remote wake-up, settings auto-save, watchdog |

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
the report range (±32767 for X/Y, ±127 for the wheels; ±127 for X/Y in boot
protocol) are split across consecutive reports, so nothing is lost either. If EP1 is still busy, new
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
  The linker script places all data, including these buffers, in DTCM,
  which the Cortex-M7 never caches, so no cache maintenance is needed.

The PC actually receives reports at the rate of the physical mouse plus the
injections. 8 kHz is the polling ceiling, not a report rate the firmware
generates on its own.

## SDK configuration

The SDK settings live in `config/`. The ones that matter:

```c
/* usb_device_config.h */
#define USB_DEVICE_CONFIG_EHCI        (1U)
#define USB_DEVICE_CONFIG_HID         (1U)   /* template default is 0 */
#define USB_DEVICE_CONFIG_ENDPOINTS   (4U)
#define USB_DEVICE_CONFIG_USE_TASK    (0U)   /* callbacks in ISR */
#define USB_DEVICE_CONFIG_SELF_POWER  (0U)   /* bus powered */
#define USB_DEVICE_CONFIG_LOW_POWER_MODE (1U) /* suspend notification */
#define USB_DEVICE_CONFIG_REMOTE_WAKEUP  (1U) /* SET_FEATURE(remote wake-up) */

/* usb_host_config.h */
#define USB_HOST_CONFIG_EHCI          (1U)
#define USB_HOST_CONFIG_HID           (1U)
#define USB_HOST_CONFIG_HUB           (1U)   /* every other class off */
#define USB_HOST_CONFIG_MAX_TRANSFERS (16U)  /* >= 2 free for IN re-arm */

/* FreeRTOSConfig.h */
#define configTICK_RATE_HZ            1000
#define configMAX_PRIORITIES          7
#define configTOTAL_HEAP_SIZE         (32U * 1024U)   /* heap_4 */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 2
```

Preprocessor symbols (set in `CMakeLists.txt`): `CPU_MIMXRT1062DVJ6B`,
`SDK_OS_FREE_RTOS`, `USE_RTOS=1`, `XIP_EXTERNAL_FLASH=1`,
`XIP_BOOT_HEADER_ENABLE=1`, `XIP_BOOT_HEADER_DCD_ENABLE=0` (no SDRAM),
`SDK_DEBUGCONSOLE=2` (no debug console; LPUART3 is the command channel).

## Things to adapt to your board

* **Pins** (`board_init.c`): LPUART3 is on GPIO_AD_B1_06/07, AP2553 EN on
  GPIO_AD_B1_10 (GPIO1_IO26), FLG on GPIO_AD_B1_11 and the status LED on
  GPIO_AD_B0_09 (GPIO1_IO09, active low; that is the user LED on the
  i.MX RT1060 EVK). These are examples. Set `BOARD_LED_ACTIVE_HIGH` to 1
  if your LED lights when the pin is high.
  Fit an external pull-down on EN, because the pad is high-Z during reset.
* **QSPI flash** (`board/flexspi_nor_config.c`): the boot header is the
  EVK's, for a 64 Mbit (8 MB) ISSI IS25WP064A-style quad SPI NOR read with
  command 0xEB at up to 133 MHz. Most 3.3 V quad SPI NOR parts accept it.
  For a different size, set `BOARD_FLASH_SIZE` (and the MPU region in
  `board/board.c`); for a part with another read command or dummy cycles,
  change the LUT in that file.
* **VID/PID** (`app_config.h`): placeholders. Use IDs you are entitled to.
* **Flash size**: at least 2 MB + 8 KB for the image slot, the update
  staging area and the settings (see [Flash layout](#flash-layout)).
* **Possible improvements**: DMA for UART TX; forwarding a mouse's
  keyboard/consumer reports through a second interface.

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
| `cmd.button(n,v)` | Injected button `n` = 1–16 | echo |
| `cmd.button(n)` | Query button `n` | `0` or `1` |
| `cmd.buttons(mask)` | Replace the whole injected button word (0–65535, bit 0 = button 1) | echo |
| `cmd.buttons()` | Query | logical button word |
| `cmd.release()` | Clear every injected button | echo |
| `cmd.move(dx,dy)` / `cmd.wheel(d)` / `cmd.pan(d)` | Add motion, vertical scroll, horizontal scroll | echo |
| `cmd.smooth(ms)` | Render each `cmd.move` over `ms` ms with ease-in/ease-out (0 = off, the default; max 1000). Kept across power cycles | echo |
| `cmd.smooth()` | Query | current duration in ms |
| `cmd.stop()` | Cancel the smoothed move in flight and any queued ones; the cursor stays where it is | echo |
| `cmd.reset()` | `stop()`, release every injected button and drop motion not yet sent. `cmd.smooth` keeps its value | echo |
| `cmd.sleep(ms)` | Prompt goes out first, then the parser task sleeps (0–600000 ms) | echo |
| `cmd.version()` / `cmd.info()` | | `1.1.0` / `HID-Proxy-RT1062` |
| `cmd.status()` | Report links, mouse, parsed report layout, UART counters, settings, reset cause and faults | seven lines, see [Bring-up](#bring-up) |
| `cmd.update(size,crc32)` | Receive a new firmware image (use `tools/fw_update.py`) | see [Firmware update](#firmware-update-over-the-uart) |

All button commands follow the button-locking rule. `release()` and
`buttons(mask)` only replace the *injected* state. A button held on the
physical mouse stays pressed, and a report goes out only if the logical word
changes.

`cmd.sleep` blocks only the parser task. Physical forwarding (host stack task
and USB ISRs) keeps running, and the DMA ring keeps receiving. Commands sent
during the sleep run in order once it ends. The ring holds 4096 bytes (roughly
200 commands); anything past that during a single sleep is lost, and counted
(see [UART overflow detection](#uart-overflow-detection)).

```
cmd.side1(1)\r\n   ->  cmd.side1(1)\r\n>>> 
cmd.buttons()\r\n  ->  cmd.buttons()\r\n8\r\n>>> 
cmd.version()\r\n  ->  cmd.version()\r\n1.1.0\r\n>>> 
cmd.left(2)\r\n    ->  (nothing)
```

## Smoothed moves

With `cmd.smooth(ms)` > 0, each `cmd.move(dx,dy)` is queued (16 segments;
when the queue is full, a new move is merged into the last queued one) and
played out over `ms` milliseconds. Position follows smoothstep
`e(t) = 3t² − 2t³`, so velocity starts at zero, peaks mid-move and returns to
zero at the target. The PIT tick recomputes the position from the curve on
every tick rather than adding up rounded steps. The deltas of a segment
therefore always sum to exactly `(dx, dy)`, and the output is deterministic.
Turning smoothing off mid-move sends the remaining distance at once. The
tuning points (duration, tick rate, curve table) are listed at the top of
`smooth_move.c`.

## Robustness

### Watchdog

WDOG1 resets the board if it is not fed for 2 s (`APP_WDOG_TIMEOUT_MS`).
The lowest-priority `system` task feeds it every 10 ms, but only while the
`uart_cmd` and `usbh_app` tasks have both checked in within the last second
(`watch.c`). So the board recovers on its own when:

* a task stops looping (stuck in a USB call, deadlock),
* anything of higher priority spins and starves the system task (a hung ISR
  or USB task), or
* the firmware crashes: `app_fatal()` flickers the LED and stops feeding.

`cmd.sleep()` parks the parser task so a long sleep is not mistaken for a
hang. The watchdog pauses while a debugger halts the core. Set
`APP_WATCHDOG_ENABLE` to 0 in `app_config.h` to switch it off.

After any restart, `cmd.status()` says why on its `reset:` line:
`power-on`, `watchdog`, `software` (after a firmware update), `debugger`,
`reset-button`, and so on. A watchdog reset also names the task that stopped
responding, and a crash adds the `app_fatal()` code (see `status_led.h`).
That note lives in a RAM section the startup code leaves alone
(`.noinit`), so it survives the reset but not a power cycle.

### UART overflow detection

The DMA ring counts its laps (eDMA major-loop interrupt), so the parser task
knows exactly how far the DMA is ahead. If it is more than the ring size
(4096 bytes) ahead, unread commands were overwritten: the task counts an
overflow and the number of lost bytes, skips to the newest data and drops
the half-received line. `cmd.status()` shows the counters on its `uart:`
line. In practice this needs more than 4 KB to arrive while the parser
cannot read, i.e. during `cmd.sleep()`.

### Suspend and remote wake-up

When the PC sleeps it suspends the bus. The board then stops sending,
discards motion (so the cursor does not jump on wake-up) and turns the LED
off. If the PC enabled wake-up for the device (Windows: Device Manager →
the mouse → Power Management → "Allow this device to wake the computer";
Linux: `power/wakeup` in sysfs), a button **press**, on the physical mouse
or through `cmd.left(1)` and friends, wakes it. Movement alone does not.
The board keeps drawing its normal current while suspended; USB allows only
2.5 mA, which PCs do not enforce but a strictly compliant hub could.

## Mouse compatibility

* **Horizontal scroll** (tilt wheel): the parser maps Consumer *AC Pan*, the
  report to the PC carries it, and `cmd.pan(d)` injects it.
* **16 buttons**: buttons 1–16 are forwarded, whether the mouse reports them
  as bits or as an array ("which buttons are down" lists), and also when
  they are spread over several reports with different report IDs. Extra
  keys that the mouse sends as *keyboard* or *consumer* reports (macro keys
  that type text or change the volume) are not mouse input and are not
  forwarded; the PC only sees a mouse.
* **Hubs and wireless dongles**: the mouse may sit behind a USB hub. For
  devices with several HID interfaces (Unifying/Lightspeed-style receivers,
  gaming mice with a separate keyboard interface) every HID interface is
  tried in turn (boot mouse first, then report-only, then the rest) until
  one has a mouse collection. One mouse is used at a time; a non-mouse
  device behind the same hub is released again.
* **BIOS/UEFI**: the PC-facing interface is a boot mouse. Firmware that
  selects boot protocol gets 3-byte boot reports (buttons, X, Y) with larger
  motion split over several reports; operating systems keep using the full
  report. `cmd.status()` shows `boot-protocol` while it is selected.
* **8 kHz mice**: two IN transfers are always queued on the mouse's
  endpoint, so the next poll is already scheduled while a report is being
  processed.

## Saved settings

`cmd.smooth(ms)` is kept across power cycles. A changed value is written
about a second after it last changed, so scripts that set it often do not
wear the flash. The settings live in two flash sectors used as a log
(16-byte records with a sequence number and CRC; one erase per 256 saves),
so a power cut during a save keeps the previous value. `cmd.status()` shows
the stored value on its `settings:` line.

## Firmware update over the UART

No debug probe is needed once the board runs this firmware:

```sh
pip install pyserial
python3 tools/fw_update.py /dev/ttyACM0 build/hid_proxy.bin     # COM7 on Windows
```

```
board firmware 1.1.0
sending 76848 bytes, crc32 0x0ac37286
100%  76848/76848
verified; installing and restarting (do not unplug)...
done: firmware 1.1.0
```

The image goes into a staging area first and is checked (CRC-32 of the
data as received, CRC-32 read back from the flash, and the i.MX RT boot
header) before anything else happens. A failed, interrupted or corrupted
transfer leaves the running firmware untouched; run the tool again. Only
the final step, copying the staging area over the running image (about a
second for a 64 KB image), must not lose power. If it does, the board needs
the ROM serial downloader (boot mode pins, over USB1, e.g. with NXP's
MCUXpresso Secure Provisioning or MCUBootUtility) or a debug probe.

The mouse keeps working during the transfer, apart from short stalls: each
4 KB erase (about 45 ms) and page write runs with interrupts off, because the
firmware executes from the same flash.

The protocol is plain text plus binary chunks of 1024 bytes, each
acknowledged; it is described in `source/fw_update.h`. `test/sim_board`
runs the board's side of it on a pseudo-terminal, so PC tools can be tested
without hardware (`make -C test sim`, then point the tool at the printed
`/dev/pts/N`).

### Flash layout

| Offset | Size | Contents |
|--------|------|----------|
| 0x000000 | 1 MB | Running image (boot header, vectors, code). The linker refuses anything larger |
| 0x100000 | 1 MB | Update staging area |
| 0x200000 | 2 × 4 KB | Saved settings |

The flash must be at least 2 MB + 8 KB.

## Bring-up

### Status LED

| Pattern | Meaning |
|---------|---------|
| On from power-up until the scheduler starts | The image booted and reached `board_init()` |
| One short blip every 2 s | Running; the PC has not configured the device (check the USB-C cable) |
| Two short blips every 2 s | Running, mouse working, PC not configured |
| Slow blink, 1 Hz | PC connected, no mouse (or still enumerating) |
| Solid on | PC connected and mouse working |
| Off (after it was on or blinking) | The PC is asleep (bus suspended) |
| Fast blink, 5 Hz | A fault: VBUS over-current (held 5 s), host stack failed to start, unsupported device or a failed mouse request. `cmd.status()` says which |
| Rapid flicker, 20 Hz | Crashed: HardFault, stack overflow, heap exhausted, a failed `assert()`/`configASSERT`, or a bring-up step failed. The watchdog restarts the board after 2 s and `cmd.status()` then shows the code on its `reset:` line (codes in `status_led.h`) |

If the LED never lights, the image did not boot: check the boot mode
and that the `.bin` was written at 0x60000000.

### `cmd.status()`

```
cmd.status()
pc: configured high-speed reports=18234 remote-wake
mouse: ready full-speed vid=046d pid=c08b interface=1 via-hub packet=8 reports=17950
layout: report-id=2 buttons=16(0xffff)@0 x=s12@16 y=s12@28 wheel=s8@40 pan=s8@48
uart: rx=5120 overflows=0 lost=0
settings: smooth=40 saves=3
reset: watchdog (task usbh_app stopped responding)
faults: none
>>> 
```

| Line | Contents |
|------|----------|
| `pc:` | `configured` and the negotiated speed (`high-speed` is needed for 8 kHz), or `not-configured`. `reports` counts reports delivered to the PC. Then `boot-protocol` (BIOS/UEFI selected it), `suspended` (PC asleep), `remote-wake` (the PC allows wake-up) and `wakeups=<n>` when they apply |
| `mouse:` | `none`, `enumerating`, `ready`, `unsupported` (no HID interface; shown until the next device is plugged in) or `error (<reason>)`. Then the mouse's speed, VID/PID, the interface used, `via-hub`, its packet size and how many reports it has sent |
| `layout:` | What the report descriptor parser found, as `<s/u><bits>@<bit offset>` after the report ID byte. `buttons=<count>(<mask>)@<offset of the lowest>`, `array=<first>-<last>:<slots>x<bits>@<offset>` for array buttons. `boot` means the boot-protocol fallback is in use. `#<id>` marks a field in a different report than X |
| `uart:` | Bytes received, RX ring overflows and the bytes they lost, and LPUART FIFO overruns if any |
| `settings:` | `cmd.smooth` value stored in flash, saves since boot, and write errors if any |
| `reset:` | Why the board last started; see [Watchdog](#watchdog) |
| `faults:` | `none`, or any of `host-init-failed`, `vbus-overcurrent=<count>` (`(active)` for 5 s after the last one), `unsupported-device`, `mouse-error` |

A quick first power-on check: connect only the PC side and expect one blip
every 2 s turning into a 1 Hz blink once the PC enumerates the device, then
`pc: configured high-speed`. Plug in the mouse and expect solid on,
`mouse: ready` and a `reports=` count that rises as you move it.

## Verification status

* `cmake --build build`: compiles and links with zero warnings (Release,
  Debug, MinSizeRel and `HID_PROXY_ITCM=ON`) against MCUXpresso SDK 2.16
  using arm-none-eabi-gcc 13.2. The image uses about 67 KB of flash and
  70 KB of the 128 KB DTCM (32 KB of that is the FreeRTOS heap). The boot
  header, IVT, vector table and every interrupt handler were checked in the
  output image, and the update's install routine was checked to sit in ITCM
  and call nothing in flash.
* `make -C test`: all unit tests pass, also under ASan/UBSan. They cover the
  parser (arrays, split reports, AC Pan), merging (16 buttons, boot
  protocol, suspend/wake), the commands, the settings log and the update
  protocol against a RAM flash with NOR semantics, and the watchdog's
  task watch.
* `tools/fw_update.py` was run against `test/sim_board` with the real
  `hid_proxy.bin`: transfer, verification and install complete.
* **Not yet done:** running on hardware. Everything that touches registers
  (watchdog, reset cause, ROM flash calls, suspend/resume, hub, the RX lap
  counter) has only been compiled, not exercised.
