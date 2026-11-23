/* untrusted.c - a tiny test program that is NOT in db. Same shape as
 * trusted.c, different bytes, so a different hash. */
#include "efi.h"
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    (void)image;
    st->ConOut->OutputString(st->ConOut, (CHAR16 *)u"      [untrusted.efi] If you can read this, Secure Boot is OFF.\r\n");
    return EFI_SUCCESS;
}
