/* lab_protocol.h - OUR protocol. Shared contract between driver and loader.
 *
 * A UEFI protocol is just: a GUID + a struct of function pointers. The driver
 * installs it on a handle; anyone can find it by GUID. That's the whole
 * "firmware API" model - every firmware service you used in Phase 2 (ConOut,
 * file system, GOP) is exactly this, installed by some other driver.
 *
 * GUID generated fresh for this lab - never reuse someone else's. */
#pragma once
#include "efi.h"

#define LAB_GREETER_PROTOCOL_GUID \
    {0x6c1f0e8a,0x3b52,0x4d7e,{0x9a,0x41,0x2f,0x8d,0x5e,0x17,0xc0,0x93}}

#define LAB_GREETER_REVISION 0x00010000

typedef struct LAB_GREETER_PROTOCOL LAB_GREETER_PROTOCOL;
struct LAB_GREETER_PROTOCOL {
    uint32_t Revision;
    /* Print a greeting for Name via the firmware console. */
    EFI_STATUS (EFIAPI *Greet)(LAB_GREETER_PROTOCOL *This, CHAR16 *Name);
    /* How many times Greet has been called (state lives inside the driver). */
    UINTN      (EFIAPI *GetCallCount)(LAB_GREETER_PROTOCOL *This);
};
