/* efi.h - the minimum slice of the UEFI spec this lab needs.
 * No EDK II, no gnu-efi: just the structs, typed out, so you can see them.
 * Layouts follow UEFI Spec 2.10, section 4 (System Table) and 12 (Console). */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint64_t  UINTN;
typedef uint64_t  EFI_STATUS;
typedef void     *EFI_HANDLE;
typedef void     *EFI_EVENT;
typedef uint16_t  CHAR16;
typedef uint8_t   BOOLEAN;

#define EFIAPI __attribute__((ms_abi))   /* UEFI uses the Microsoft x64 ABI */
#define EFI_SUCCESS 0
#define EFI_ERROR(s) (((int64_t)(s)) < 0)
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL

typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision, HeaderSize, CRC32, Reserved;
} EFI_TABLE_HEADER;

/* ---- Console output protocol (Simple Text Output) ---- */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
    void *TestString, *QueryMode, *SetMode, *SetAttribute;
    EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);
    void *SetCursorPosition, *EnableCursor, *Mode;
};

/* ---- Console input protocol ---- */
typedef struct { uint16_t ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;
typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (EFIAPI *ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, EFI_INPUT_KEY *);
    EFI_EVENT WaitForKey;
};

/* ---- Memory map ---- */
typedef struct {
    uint32_t Type;
    uint64_t PhysicalStart, VirtualStart, NumberOfPages, Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---- Boot Services: only usable until ExitBootServices() ---- */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL;
    void *AllocatePages, *FreePages;
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MapSize, EFI_MEMORY_DESCRIPTOR *Map,
                                      UINTN *MapKey, UINTN *DescSize, uint32_t *DescVer);
    EFI_STATUS (EFIAPI *AllocatePool)(uint32_t PoolType, UINTN Size, void **Buf);
    EFI_STATUS (EFIAPI *FreePool)(void *Buf);
    void *CreateEvent, *SetTimer;
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN N, EFI_EVENT *Events, UINTN *Index);
    void *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    void *HandleProtocol, *Reserved, *RegisterProtocolNotify, *LocateHandle;
    void *LocateDevicePath, *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage, *ExitBootServices;
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, uint64_t Code, UINTN Size, CHAR16 *Data);
    /* ...more follow in the spec; not needed yet */
} EFI_BOOT_SERVICES;

/* ---- Runtime Services: survive into the OS ---- */
typedef enum { EfiResetCold, EfiResetWarm, EfiResetShutdown } EFI_RESET_TYPE;
typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime, *SetTime, *GetWakeupTime, *SetWakeupTime;
    void *SetVirtualAddressMap, *ConvertPointer;
    void *GetVariable, *GetNextVariableName, *SetVariable;
    void *GetNextHighMonotonicCount;
    void (EFIAPI *ResetSystem)(EFI_RESET_TYPE, EFI_STATUS, UINTN, void *);
} EFI_RUNTIME_SERVICES;

typedef struct { EFI_GUID VendorGuid; void *VendorTable; } EFI_CONFIGURATION_TABLE;

/* ---- The System Table: the one pointer firmware hands you ---- */
typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    uint32_t                         FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE         *ConfigurationTable;
} EFI_SYSTEM_TABLE;
