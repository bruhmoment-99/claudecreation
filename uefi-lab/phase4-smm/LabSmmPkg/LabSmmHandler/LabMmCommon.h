/** @file
  LabMmCommon.h - the interface between the shared handler code
  (LabSmmHandler.c) and the two entry points, one per MM architecture:

    LabSmmEntryTraditional.c  DXE_SMM_DRIVER  (Traditional MM, Phases 4 + 6)
    LabMmEntryStandalone.c    MM_STANDALONE   (Standalone MM, Phase 7)

  Everything that differs between the two models is behind these hooks.
**/
#ifndef LAB_MM_COMMON_H_
#define LAB_MM_COMMON_H_

#include <PiMm.h>
#include <Guid/LabSmmComm.h>

/* Provided by the entry file for its model. */
BOOLEAN
LabIsBufferOutsideMmValid (
  IN EFI_PHYSICAL_ADDRESS  Buffer,
  IN UINT64                Length
  );

/* Provided by the entry file: may the GUID handler use this comm buffer?
   The two MM cores hand handlers DIFFERENT kinds of pointers here. */
BOOLEAN
LabIsCommBufferValid (
  IN VOID   *CommBuffer,
  IN UINTN  Length
  );

/* Provided by LabSmmHandler.c. Called once by either entry point. */
EFI_STATUS
LabMmCommonInit (
  IN EFI_MM_SYSTEM_TABLE  *Mmst,
  IN UINT64               MmModel,
  IN UINT64               MmramBase,
  IN UINT64               MmramSize
  );

#endif
