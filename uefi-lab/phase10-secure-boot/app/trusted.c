/* trusted.c - a tiny test program whose hash gets enrolled in db. */
#include "efi.h"
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    (void)image;
    st->ConOut->OutputString(st->ConOut, (CHAR16 *)u"      [trusted.efi] Hello! The firmware checked my hash and let me run.\r\n");
    return EFI_SUCCESS;
}
