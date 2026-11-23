/* driver.c - a UEFI *boot service driver* that publishes LAB_GREETER_PROTOCOL.
 *
 * App vs. driver - the difference that matters:
 *   - An APPLICATION is unloaded when its entry point returns.
 *   - A DRIVER whose entry point returns EFI_SUCCESS STAYS RESIDENT in memory
 *     (as EfiBootServicesCode/Data), so the protocol it installed keeps working.
 * The PE header's subsystem field decides which one you are:
 *   10 = EFI application, 11 = boot service driver, 12 = runtime driver.
 *
 * This driver also hooks ExitBootServices(), to show the moment where every
 * boot service driver - including this one - is about to lose its world. */
#include "lab_protocol.h"
#include "lib.h"

static UINTN gCalls;
static EFI_HANDLE gHandle;     /* NULL in -> firmware creates a new handle for us */
static EFI_EVENT gExitEvent;

static EFI_STATUS EFIAPI Greet(LAB_GREETER_PROTOCOL *This, CHAR16 *Name) {
    (void)This;
    if (!Name) return EFI_INVALID_PARAMETER;
    gCalls++;
    print(u"  [driver] Hello, "); print(Name); print(u"! (call #"); print_dec(gCalls); print(u")\r\n");
    return EFI_SUCCESS;
}

static UINTN EFIAPI GetCallCount(LAB_GREETER_PROTOCOL *This) { (void)This; return gCalls; }

/* The interface instance. Lives in our image, which stays loaded. */
static LAB_GREETER_PROTOCOL gGreeter = {
    .Revision     = LAB_GREETER_REVISION,
    .Greet        = Greet,
    .GetCallCount = GetCallCount,
};

/* Runs at TPL_NOTIFY inside ExitBootServices(). Rules here are strict: no
 * allocating memory, no console - so we talk to the UART directly. */
static void EFIAPI OnExitBootServices(EFI_EVENT Event, void *Ctx) {
    (void)Event; (void)Ctx;
    serial_puts("  [driver] ExitBootServices signalled: my code/data is about to become "
                "free memory for the OS. Greet() was called ");
    serial_dec(gCalls);
    serial_puts(" time(s). Goodbye.\n");
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE Image, EFI_SYSTEM_TABLE *ST) {
    gST = ST;
    EFI_GUID guid = LAB_GREETER_PROTOCOL_GUID;

    EFI_STATUS s = ST->BootServices->InstallProtocolInterface(
        &gHandle, &guid, EFI_NATIVE_INTERFACE, &gGreeter);
    if (EFI_ERROR(s)) { print(u"  [driver] InstallProtocolInterface failed\r\n"); return s; }

    s = ST->BootServices->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY,
                                      OnExitBootServices, NULL, &gExitEvent);
    if (EFI_ERROR(s)) print(u"  [driver] warning: could not hook ExitBootServices\r\n");

    print(u"  [driver] loaded at image handle "); print_hex((uint64_t)Image);
    print(u", installed LAB_GREETER_PROTOCOL on handle "); print_hex((uint64_t)gHandle);
    print(u"\r\n");
    return EFI_SUCCESS;   /* success => stay resident */
}
