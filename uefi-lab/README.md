# UEFI Lab — writing your own firmware (UEFI → ring -2 SMM), on Windows 11

You develop on Windows 11. The firmware code runs in **QEMU**, an emulator,
on top of **OVMF**, open-source UEFI firmware. Nothing here touches your PC's
real firmware, Secure Boot or BitLocker, and nothing can brick it.

> These scripts only run if you run them. Nothing in this repo runs on its own.

| Phase | What | Folder |
|---|---|---|
| 0 | Mental model: ring 3 → 0 → -1 (hypervisor) → -2 (SMM) → -3 (ME/PSP) | — |
| 1 | Toolchain: LLVM/Clang + lld-link, QEMU + OVMF | `setup.cmd` |
| 2 | First UEFI app → `BOOTX64.EFI` | [`phase2-hello/`](phase2-hello) |
| 3 | UEFI driver + protocols, boot vs. runtime services, OS handoff | [`phase3-driver/`](phase3-driver) |
| 4 | Ring -2: an SMI handler in SMRAM (emulator) | next |

## Quick start (Windows 11)

Open the `uefi-lab` folder in File Explorer or a terminal, then:

| Step | Run | What it does |
|---|---|---|
| 1 | `setup.cmd` | Installs LLVM and QEMU with winget. Each one shows a UAC prompt. QEMU includes the OVMF firmware. |
| 2 | `selftest.cmd` | Builds phases 2 and 3 and boots each one in QEMU with no window. Prints `PASS`/`FAIL` (takes about a minute). |
| 3 | `phase2-hello\run.cmd` | Boots phase 2 in a QEMU window. |
|   | `phase2-hello\run.cmd -Headless` | Same, but the firmware console shows in your terminal. Quit with **Ctrl+A** then **X**. |

The `.cmd` files are thin wrappers around the `.ps1` scripts. They exist
because Windows 11 blocks `.ps1` files by default ("running scripts is
disabled on this system"). They bypass that for the one script only, and
don't change any system setting.

Each phase also has `build.cmd` (compile only) and `run.cmd` (builds first if
needed).

### What setup installs
| Tool | winget id | Default location |
|---|---|---|
| LLVM (clang + lld-link) | `LLVM.LLVM` | `C:\Program Files\LLVM\bin` |
| QEMU (+ OVMF in `share\edk2-x86_64-code.fd`) | `SoftwareFreedomConservancy.QEMU` | `C:\Program Files\qemu` |

The scripts find these even if the installers didn't add them to `PATH`. If
your OVMF files are somewhere else, set `LAB_OVMF_CODE` and `LAB_OVMF_VARS`.

### Layout
```
uefi-lab\
  setup.cmd / selftest.cmd     one-time setup, build + boot everything
  tools\common.ps1             shared helpers: find tools, build .efi, start QEMU
  phase2-hello\                main.c, efi.h, build.cmd, run.cmd
  phase3-driver\               loader.c, driver.c, lab_protocol.h, build.cmd, run.cmd
```
Build output goes to each phase's `esp\` folder. QEMU shows that folder to the
VM as a FAT disk, and OVMF boots `esp\EFI\BOOT\BOOTX64.EFI` from it.

## Phase 2 — first UEFI application

```bat
phase2-hello\build.cmd
phase2-hello\run.cmd
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
(QEMU for Windows ships its own OVMF build, so the vendor string and numbers will differ a little.)

### What to read in the code
- **`efi.h`**: the UEFI structs typed out by hand, with no EDK II or gnu-efi.
  The System Table, Boot Services, Runtime Services and the console protocols
  are all there.
- **`main.c`**:
  - The entry point is `efi_main(ImageHandle, SystemTable)`. It uses the MS x64
    ABI (`EFIAPI`), because UEFI follows the Windows PE calling convention.
    That's also why the compiler target is `x86_64-unknown-windows`.
  - Everything goes through `SystemTable`: `ConOut` for text, `ConIn` for
    keys, `BootServices` for memory and timers, `RuntimeServices` for reset.
  - `GetMemoryMap` uses the usual "ask for the size, allocate, ask again"
    pattern. Step through descriptors by `DescriptorSize`, not `sizeof`.
  - The big `Reserved` total on q35 comes from firmware-reserved address
    space such as the PCIe ECAM window, not from RAM.

### Build flags explained (see `Invoke-EfiBuild` in `tools\common.ps1`)
`-target x86_64-unknown-windows`: UEFI images are PE32+ files, the same
format as Windows `.exe`s. `-ffreestanding -nostdlib`: there is no C runtime.
`-mno-red-zone`: firmware interrupts can overwrite the red zone.
`-fshort-wchar`: `CHAR16` strings (`u"..."`). `-subsystem:efi_application`:
sets the PE subsystem field to 10 so firmware accepts the image.

## Phase 3 — driver, protocols, handoff

```bat
phase3-driver\build.cmd
phase3-driver\run.cmd -Headless
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

## Phase 4 preview

The QEMU for Windows installer already includes an SMM-enabled OVMF
(`share\edk2-x86_64-secure-code.fd`, run with `-machine q35,smm=on`). That
means the first ring -2 experiments can run on Windows without building
EDK II yourself.

## Also works on Linux / WSL
Every phase has `build.sh` and `run.sh` that do the same thing (needs
`clang lld qemu-system-x86 ovmf`). Every phase was tested that way in a cloud
Linux container. The Windows scripts were written for Windows PowerShell 5.1
(built into Windows 11) and drive the same compiler flags and QEMU command
line, but they haven't been run on a Windows machine yet.
