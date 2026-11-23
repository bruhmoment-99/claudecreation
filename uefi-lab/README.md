# UEFI Lab — writing your own firmware (UEFI → ring -2 SMM)

You learn this in an emulator first, so nothing can brick. The real-hardware
phase is optional and uses a separate board, never your daily driver.

| Phase | What | Status |
|---|---|---|
| 0 | Mental model: ring 3 → 0 → -1 (hypervisor) → -2 (SMM) → -3 (ME/PSP) | — |
| 1 | Toolchain: LLVM/Clang + lld-link, QEMU + OVMF | ✅ |
| 2 | First UEFI app → `BOOTX64.EFI`, boot in QEMU/OVMF | ✅ [`phase2-hello/`](phase2-hello) |
| 3 | UEFI driver + protocols, boot vs. runtime services, OS handoff | ✅ [`phase3-driver/`](phase3-driver) |
| 4 | Ring -2: a `DXE_SMM_DRIVER` / SMI handler in EDK II (emulator) | next |
| 5 | Real hardware: Boot-Guard-free coreboot board (e.g. ThinkPad X230) + CH341A clip | optional |

> ⚠️ Phase 5 is **not** for a B760M / 14th-gen Intel board. Intel Boot Guard is
> fused in at the factory there, so the CPU refuses unsigned firmware.

## Phase 1 — toolchain

**Windows** (accept the UAC prompt for each):
```powershell
winget install LLVM.LLVM
winget install SoftwareFreedomConservancy.QEMU
```
QEMU for Windows already includes OVMF (`share\edk2-x86_64-code.fd`).

**Linux / WSL:**
```bash
sudo apt install clang lld qemu-system-x86 ovmf
```

## Phase 2 — run it

Windows:
```powershell
.\phase2-hello\build.ps1
.\phase2-hello\run.ps1          # -Headless for serial output in the terminal
```
Linux:
```bash
./phase2-hello/build.sh && ./phase2-hello/run.sh --headless
```

Example output (OVMF, 256 MiB VM):
```
Hello from bare metal (well, QEMU)!

Firmware vendor : Ubuntu distribution of EDK II
UEFI revision   : 2.7
Config tables   : 11

Memory map: 127 descriptors
  BootServicesCode: 3 MiB
  BootServicesData: 38 MiB
  RuntimeServicesCode: 1 MiB
  RuntimeServicesData: 2 MiB
  Conventional: 208 MiB
  ...
```

### What to read in the code
- **`efi.h`**: the UEFI structs typed out by hand, with no EDK II or gnu-efi.
  The System Table, Boot Services, Runtime Services and the console protocols
  are all there.
- **`main.c`**:
  - The entry point is `efi_main(ImageHandle, SystemTable)`. It uses the MS x64
    ABI (`EFIAPI`) because UEFI follows the PE/Windows calling convention.
  - Everything goes through `SystemTable`: `ConOut` for text, `ConIn` for
    keys, `BootServices` for memory and timers, `RuntimeServices` for reset.
  - `GetMemoryMap` uses the usual "ask for the size, allocate, ask again" pattern.
    Step through descriptors by `DescriptorSize`, not `sizeof`.
  - **Boot vs. runtime services:** the `BootServices*` regions are released
    when an OS loader calls `ExitBootServices()`. The `RuntimeServices*`
    regions stay mapped for the OS. That's the Phase 3 handoff.
  - The big `Reserved` total on q35 comes from firmware-reserved address
    space such as the PCIe ECAM window, not from RAM.

### Build flags explained
`-target x86_64-unknown-windows`: UEFI images are PE32+, so we target the
Windows object format. `-ffreestanding -nostdlib`: there is no libc.
`-mno-red-zone`: firmware interrupts can overwrite the red zone.
`-fshort-wchar`: `CHAR16` strings (`u"..."`). `-subsystem:efi_application`:
sets the PE subsystem field to 10 so firmware accepts the image.

## Phase 3 — driver, protocols, handoff

```bash
./phase3-driver/build.sh && ./phase3-driver/run.sh --headless
```
```powershell
.\phase3-driver\build.ps1 ; .\phase3-driver\run.ps1 -Headless
```

Two images go onto the emulated ESP:

| File | PE subsystem | Role |
|---|---|---|
| `EFI/BOOT/BOOTX64.EFI` ← `loader.c` | 10 `efi_application` | plays the OS loader |
| `EFI/lab/greeter.efi` ← `driver.c` | 11 `efi_boot_service_driver` | installs `LAB_GREETER_PROTOCOL` and stays resident |

The loader runs in three steps:

1. **Protocol discovery.** It walks the firmware handle database
   (`LocateHandleBuffer`, `ProtocolsPerHandle`), counts disks (BlockIo) and
   filesystems, and reads the GOP framebuffer.
2. **Load your own driver.** It goes `LoadedImage → DeviceHandle →
   SimpleFileSystem → File` to read `greeter.efi` from the volume it booted
   from, then calls `LoadImage` and `StartImage`. After that,
   `LocateProtocol(LAB_GREETER_GUID)` finds the driver's interface and the
   loader calls `Greet()`.
3. **Handoff.** It calls `GetMemoryMap` and then `ExitBootServices(MapKey)`,
   retrying if the map changed in between. The driver's
   `EVT_SIGNAL_EXIT_BOOT_SERVICES` callback fires during the call. After that,
   ConOut is gone, so output goes straight to the COM1 UART with `outb`. The
   loader sorts the final memory map into reusable and runtime memory, then
   calls `RuntimeServices->GetTime()` and `ResetSystem()`. Those still work
   because their code lives in `EfiRuntimeServicesCode`.

Real output:
```
[1] Protocol discovery: the firmware handle database
    handles in database     : 180
    protocol instances      : 562
    GOP framebuffer         : 1280x800 @ 0x0000000080000000
    LAB_GREETER before load : not found (expected)
[2] Loading driver \EFI\lab\greeter.efi
  [driver] loaded at image handle 0x...E76BA98, installed LAB_GREETER_PROTOCOL on handle 0x...E76B718
  [driver] Hello, ring 3 world! (call #1)
[3] Handoff: GetMemoryMap + ExitBootServices
  [driver] ExitBootServices signalled: my code/data is about to become free memory for the OS. ...
[loader] ExitBootServices succeeded (attempt 1). We now own the machine.
    => usable by the OS (incl. old boot-services memory): 249 MiB
    => must stay mapped for Runtime Services: 5 regions, 7444 KiB
[loader] RuntimeServices->GetTime(): 2026-11-23 02:46:17 (CMOS RTC)
```

### Key ideas
- **A protocol is a GUID plus a struct of function pointers** (`lab_protocol.h`).
  Every firmware service, including ConOut, the filesystem and GOP, works the
  same way, installed by some driver in OVMF.
- **App vs. driver:** an application is unloaded when it returns. A driver
  that returns `EFI_SUCCESS` stays in memory, so its protocol keeps working.
- **Boot vs. runtime services:** after `ExitBootServices`, every boot service
  driver's memory, including `greeter.efi`'s, becomes free RAM for the OS.
  Only runtime code and data (`EFI_MEMORY_RUNTIME`) survives. A real kernel
  would then call `SetVirtualAddressMap`.
- **The MapKey rule:** any allocation between `GetMemoryMap` and
  `ExitBootServices` invalidates the key. That includes allocations made by a
  timer callback inside firmware. Always loop.
- **Security note:** `LoadImage` is where Secure Boot checks signatures. OVMF
  here runs without Secure Boot, so the unsigned driver loads.
- **Why this leads to Phase 4:** this driver dies at `ExitBootServices`. An SMM
  driver is loaded into SMRAM instead, survives the OS boot, and runs on every
  SMI. Nothing the OS does can unload it.

### Going further (optional exercises)
- Turn `greeter.efi` into a UEFI Driver Model driver: add an
  `EFI_DRIVER_BINDING_PROTOCOL` with `Supported` and `Start` functions and
  attach it to a device handle with `ConnectController`.
- Build it as `efi_runtime_driver` with its data in `EfiRuntimeServicesData`,
  then call it after `ExitBootServices`.
