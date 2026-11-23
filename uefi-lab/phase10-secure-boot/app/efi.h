/* efi.h - Phase 6: phase4-smm/app/efi.h with typed Get/SetVariable.
 * Phase 4 slice of the UEFI spec: phase3-driver/efi.h plus the two
 * SMM protocols a ring-0 caller uses (SMM Access 2, SMM Communication).
 * Originally: Phase 3 slice (a superset of phase2-hello/efi.h).
 * Boot Services is now complete up to CreateEventEx, because we call
 * LoadImage, InstallProtocolInterface, LocateProtocol and ExitBootServices,
 * which sit further down the table. Field order is the ABI: never reorder. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint64_t  UINTN;
typedef uint64_t  EFI_STATUS;
typedef void     *EFI_HANDLE;
typedef void     *EFI_EVENT;
typedef uint16_t  CHAR16;
typedef uint8_t   BOOLEAN;
typedef uint8_t   UINT8;     /* EDK II spellings, used by LabSmmComm.h */
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;

#define EFIAPI __attribute__((ms_abi))
#define EFI_SUCCESS            0
#define EFI_ERROR(s)           (((int64_t)(s)) < 0)
#define EFI_INVALID_PARAMETER  0x8000000000000002ULL
#define EFI_BUFFER_TOO_SMALL   0x8000000000000005ULL
#define EFI_NOT_FOUND          0x800000000000000EULL

typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision, HeaderSize, CRC32, Reserved;
} EFI_TABLE_HEADER;

/* ---------------- Console ---------------- */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
    void *TestString, *QueryMode, *SetMode, *SetAttribute;
    EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);
    void *SetCursorPosition, *EnableCursor, *Mode;
};
typedef struct { uint16_t ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;
typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (EFIAPI *ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, EFI_INPUT_KEY *);
    EFI_EVENT WaitForKey;
};

/* ---------------- Memory ---------------- */
typedef enum {
    EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode,
    EfiBootServicesData, EfiRuntimeServicesCode, EfiRuntimeServicesData,
    EfiConventionalMemory, EfiUnusableMemory, EfiACPIReclaimMemory,
    EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
    EfiPalCode, EfiPersistentMemory, EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    uint32_t Type;
    uint64_t PhysicalStart, VirtualStart, NumberOfPages, Attribute;
} EFI_MEMORY_DESCRIPTOR;
#define EFI_MEMORY_RUNTIME 0x8000000000000000ULL

/* ---------------- Events / TPL ---------------- */
#define TPL_APPLICATION 4
#define TPL_CALLBACK    8
#define TPL_NOTIFY      16
#define EVT_SIGNAL_EXIT_BOOT_SERVICES 0x00000201
typedef void (EFIAPI *EFI_EVENT_NOTIFY)(EFI_EVENT Event, void *Context);

/* ---------------- Protocol plumbing ---------------- */
typedef enum { AllHandles, ByRegisterNotify, ByProtocol } EFI_LOCATE_SEARCH_TYPE;
#define EFI_NATIVE_INTERFACE 0

/* ---------------- Boot Services (complete through UEFI 2.0) ---------------- */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    /* Task priority */
    UINTN (EFIAPI *RaiseTPL)(UINTN NewTpl);
    void  (EFIAPI *RestoreTPL)(UINTN OldTpl);
    /* Memory */
    EFI_STATUS (EFIAPI *AllocatePages)(uint32_t Type, uint32_t MemType, UINTN Pages, uint64_t *Addr);
    EFI_STATUS (EFIAPI *FreePages)(uint64_t Addr, UINTN Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MapSize, EFI_MEMORY_DESCRIPTOR *Map,
                                      UINTN *MapKey, UINTN *DescSize, uint32_t *DescVer);
    EFI_STATUS (EFIAPI *AllocatePool)(uint32_t PoolType, UINTN Size, void **Buf);
    EFI_STATUS (EFIAPI *FreePool)(void *Buf);
    /* Events & timers */
    EFI_STATUS (EFIAPI *CreateEvent)(uint32_t Type, UINTN NotifyTpl, EFI_EVENT_NOTIFY Fn,
                                     void *Ctx, EFI_EVENT *Event);
    void *SetTimer;
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN N, EFI_EVENT *Events, UINTN *Index);
    void *SignalEvent;
    EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT);
    void *CheckEvent;
    /* Protocol handlers */
    EFI_STATUS (EFIAPI *InstallProtocolInterface)(EFI_HANDLE *Handle, EFI_GUID *Protocol,
                                                  uint32_t IfaceType, void *Iface);
    void *ReinstallProtocolInterface;
    EFI_STATUS (EFIAPI *UninstallProtocolInterface)(EFI_HANDLE, EFI_GUID *, void *);
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **Iface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    /* Image services */
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN BootPolicy, EFI_HANDLE Parent, void *DevicePath,
                                   void *SrcBuf, UINTN SrcSize, EFI_HANDLE *Image);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE Image, UINTN *ExitDataSize, CHAR16 **ExitData);
    EFI_STATUS (EFIAPI *Exit)(EFI_HANDLE, EFI_STATUS, UINTN, CHAR16 *);
    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE);
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE Image, UINTN MapKey);
    /* Misc */
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, uint64_t Code, UINTN Size, CHAR16 *Data);
    /* Driver model */
    void *ConnectController, *DisconnectController;
    /* Open/close protocol */
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    /* Library */
    EFI_STATUS (EFIAPI *ProtocolsPerHandle)(EFI_HANDLE, EFI_GUID ***Buf, UINTN *Count);
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE, EFI_GUID *Protocol,
                                            void *SearchKey, UINTN *NoHandles, EFI_HANDLE **Buf);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Iface);
    void *InstallMultipleProtocolInterfaces, *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32, *CopyMem, *SetMem, *CreateEventEx;
} EFI_BOOT_SERVICES;

/* ---------------- Runtime Services ---------------- */
typedef struct {
    uint16_t Year; uint8_t Month, Day, Hour, Minute, Second, Pad1;
    uint32_t Nanosecond; int16_t TimeZone; uint8_t Daylight, Pad2;
} EFI_TIME;
typedef enum { EfiResetCold, EfiResetWarm, EfiResetShutdown } EFI_RESET_TYPE;
typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_STATUS (EFIAPI *GetTime)(EFI_TIME *Time, void *Capabilities);
    void *SetTime, *GetWakeupTime, *SetWakeupTime;
    void *SetVirtualAddressMap, *ConvertPointer;
    EFI_STATUS (EFIAPI *GetVariable)(CHAR16 *Name, EFI_GUID *Vendor, uint32_t *Attributes, UINTN *DataSize, void *Data);
    void *GetNextVariableName;
    EFI_STATUS (EFIAPI *SetVariable)(CHAR16 *Name, EFI_GUID *Vendor, uint32_t Attributes, UINTN DataSize, void *Data);
    void *GetNextHighMonotonicCount;
    void (EFIAPI *ResetSystem)(EFI_RESET_TYPE, EFI_STATUS, UINTN, void *);
} EFI_RUNTIME_SERVICES;

typedef struct { EFI_GUID VendorGuid; void *VendorTable; } EFI_CONFIGURATION_TABLE;

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

/* ================= Firmware protocols we consume ================= */

/* Loaded Image: "who am I, and which device was I loaded from?" */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    {0x5B1B31A1,0x9562,0x11d2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}}
typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    uint32_t LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    uint64_t ImageSize;
    uint32_t ImageCodeType, ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* Simple File System + File: FAT access, provided by firmware. */
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    {0x964E5B22,0x6459,0x11d2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}}
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **New, CHAR16 *Name,
                              uint64_t Mode, uint64_t Attr);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *);
    void *Delete;
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *, UINTN *Size, void *Buf);
    void *Write;
    EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *, uint64_t *Pos);
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *, uint64_t Pos);
    void *GetInfo, *SetInfo, *Flush;
};
#define EFI_FILE_MODE_READ 0x1ULL
typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **Root);
};

/* Graphics Output: the framebuffer an OS loader hands to its kernel. */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}}
typedef struct {
    uint32_t Version, HorizontalResolution, VerticalResolution, PixelFormat;
    uint32_t PixelMask[4];
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
typedef struct {
    uint32_t MaxMode, Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    uint64_t FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;
typedef struct {
    void *QueryMode, *SetMode, *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* Block I/O: raw disks. We only count handles that carry it. */
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    {0x964e5b21,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}

/* ================= SMM, seen from ring 0 ================= */

/* SMM Access 2: describes SMRAM and whether it is open/closed/locked. */
#define EFI_SMM_ACCESS2_PROTOCOL_GUID \
    {0xc2702b74,0x800c,0x4131,{0x87,0x46,0x8f,0xb5,0xb8,0x9c,0xe4,0xac}}
#define EFI_SMRAM_OPEN    0x1ULL
#define EFI_SMRAM_CLOSED  0x2ULL
#define EFI_SMRAM_LOCKED  0x4ULL
typedef struct {
    uint64_t PhysicalStart, CpuStart, PhysicalSize, RegionState;
} EFI_SMRAM_DESCRIPTOR;
typedef struct EFI_SMM_ACCESS2_PROTOCOL EFI_SMM_ACCESS2_PROTOCOL;
struct EFI_SMM_ACCESS2_PROTOCOL {
    void *Open, *Close, *Lock;
    EFI_STATUS (EFIAPI *GetCapabilities)(EFI_SMM_ACCESS2_PROTOCOL *, UINTN *MapSize, EFI_SMRAM_DESCRIPTOR *Map);
    BOOLEAN LockState, OpenState;
};

/* SMM Communication: the official doorbell from ring 0 into SMM. Firmware
 * fills a buffer, raises a software SMI and the SMM core dispatches the
 * payload to whichever handler registered HeaderGuid. */
#define EFI_SMM_COMMUNICATION_PROTOCOL_GUID \
    {0xc68ed8e2,0x9dc6,0x4cbd,{0x9d,0x94,0xdb,0x65,0xac,0xc5,0xc3,0x32}}
typedef struct EFI_SMM_COMMUNICATION_PROTOCOL EFI_SMM_COMMUNICATION_PROTOCOL;
struct EFI_SMM_COMMUNICATION_PROTOCOL {
    EFI_STATUS (EFIAPI *Communicate)(EFI_SMM_COMMUNICATION_PROTOCOL *, void *CommBuffer, UINTN *CommSize);
};
typedef struct {
    EFI_GUID HeaderGuid;
    UINTN    MessageLength;
    /* UINT8 Data[MessageLength] follows */
} EFI_SMM_COMMUNICATE_HEADER;

#define AllocateAnyPages 0

#define EFI_VARIABLE_NON_VOLATILE        0x1
#define EFI_VARIABLE_BOOTSERVICE_ACCESS  0x2
#define EFI_VARIABLE_RUNTIME_ACCESS      0x4
#define EFI_WRITE_PROTECTED              0x8000000000000008ULL
#define EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS 0x20
#define EFI_VARIABLE_APPEND_WRITE        0x40
#define EFI_ACCESS_DENIED                0x800000000000000FULL
#define EFI_SECURITY_VIOLATION           0x800000000000001AULL

/* Device paths: how firmware names "this file on that disk". */
#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    {0x09576e91,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}}
typedef struct { uint8_t Type, SubType; uint8_t Length[2]; } EFI_DEVICE_PATH_PROTOCOL;
