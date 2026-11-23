# UEFI Lab — writing your own firmware (UEFI → ring -2 SMM)

You learn this in an emulator first, so nothing can brick. The real-hardware
phase is optional and uses a separate board, never your daily driver.

| Phase | What | Status |
|---|---|---|
| 0 | Mental model: ring 3 → 0 → -1 (hypervisor) → -2 (SMM) → -3 (ME/PSP) | — |
| 1 | Toolchain: LLVM/Clang + lld-link, QEMU + OVMF | ✅ |
| 2 | First UEFI app → `BOOTX64.EFI`, boot in QEMU/OVMF | ✅ [`phase2-hello/`](phase2-hello) |
| 3 | UEFI driver + protocols, boot vs. runtime services, OS handoff | next |
| 4 | Ring -2: a `DXE_SMM_DRIVER` / SMI handler in EDK II (emulator) | |
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
