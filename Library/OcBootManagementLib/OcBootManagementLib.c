/** @file
  Copyright (C) 2019, vit9696. All rights reserved.

  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include "BootManagementInternal.h"

#include <Guid/AppleVariable.h>
#include <Guid/OcVariables.h>

#include <IndustryStandard/AppleCsrConfig.h>

#include <Protocol/AppleBootPolicy.h>
#include <Protocol/AppleKeyMapAggregator.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextOut.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/OcCryptoLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/DevicePathLib.h>
#include <Library/OcGuardLib.h>
#include <Library/OcTimerLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcAppleKeyMapLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcDevicePathLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcMiscLib.h>
#include <Library/OcRtcLib.h>
#include <Library/OcStringLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>

EFI_STATUS
EFIAPI
OcShowSimplePasswordRequest (
  IN VOID                *Context,
  IN OC_PRIVILEGE_LEVEL  Level
  )
{
  OC_PRIVILEGE_CONTEXT *Privilege;

  BOOLEAN              Result;

  UINT8                Password[32];
  UINT32               PwIndex;

  UINT8                Index;
  EFI_STATUS           Status;
  EFI_INPUT_KEY        Key;

  if (Context == NULL) {
    return EFI_SUCCESS;
  }

  Privilege = (OC_PRIVILEGE_CONTEXT *)Context;

  if (Privilege->CurrentLevel >= Level) {
    return EFI_SUCCESS;
  }

  gST->ConOut->ClearScreen (gST->ConOut);

  for (Index = 0; Index < 3; ++Index) {
    PwIndex = 0;
    //
    // Skip previously pressed characters.
    //
    do {
      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    } while (!EFI_ERROR (Status));

    gST->ConOut->OutputString (gST->ConOut, L"Password: ");

    while (TRUE) {
      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      if (Status == EFI_NOT_READY) {
        continue;
      } else if (EFI_ERROR (Status)) {
        gST->ConOut->ClearScreen (gST->ConOut);
        SecureZeroMem (Password, PwIndex);
        SecureZeroMem (&Key.UnicodeChar, sizeof (Key.UnicodeChar));

        DEBUG ((DEBUG_ERROR, "Input device error\r\n"));
        return EFI_ABORTED;
      }

      if (Key.ScanCode == SCAN_ESC) {
        gST->ConOut->ClearScreen (gST->ConOut);
        SecureZeroMem (Password, PwIndex);
        //
        // ESC aborts the input.
        //
        return EFI_ABORTED;
      } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
        gST->ConOut->ClearScreen (gST->ConOut);
        //
        // RETURN finalizes the input.
        //
        break;
      } else if (Key.UnicodeChar == CHAR_BACKSPACE) {
        //
        // Delete the last entered character, if such exists.
        //
        if (PwIndex != 0) {
          --PwIndex;
          Password[PwIndex] = 0;
          //
          // Overwrite current character with a space.
          //
          gST->ConOut->SetCursorPosition (
                         gST->ConOut,
                         gST->ConOut->Mode->CursorColumn - 1,
                         gST->ConOut->Mode->CursorRow
                         );
          gST->ConOut->OutputString (gST->ConOut, L" ");
          gST->ConOut->SetCursorPosition (
                         gST->ConOut,
                         gST->ConOut->Mode->CursorColumn - 1,
                         gST->ConOut->Mode->CursorRow
                         );
        }

        continue;
      } else if (Key.UnicodeChar == CHAR_NULL
       || (UINT8)Key.UnicodeChar != Key.UnicodeChar) {
        //
        // Only ASCII characters are supported.
        //
        continue;
      }

      if (PwIndex == ARRAY_SIZE (Password)) {
        continue;
      }

      gST->ConOut->OutputString (gST->ConOut, L"*");

      Password[PwIndex] = (UINT8)Key.UnicodeChar;
      ++PwIndex;
    }

    Result = OcVerifyPasswordSha512 (
               Password,
               PwIndex,
               Privilege->Salt,
               Privilege->SaltSize,
               Privilege->Hash
               );

    SecureZeroMem (Password, PwIndex);

    if (Result) {
      gST->ConOut->ClearScreen (gST->ConOut);
      Privilege->CurrentLevel = Level;
      return EFI_SUCCESS;
    }
  }

  gST->ConOut->ClearScreen (gST->ConOut);
  DEBUG ((DEBUG_WARN, "Password retry limit exceeded.\r\n"));

  gBS->Stall (5000000);
  gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);
  return EFI_ACCESS_DENIED;
}

EFI_STATUS
OcRunSimpleBootPicker (
  IN OC_PICKER_CONTEXT  *Context,
  IN INTN               HotkeyNumber
  )
{
  EFI_STATUS                         Status;
  APPLE_BOOT_POLICY_PROTOCOL         *AppleBootPolicy;
  APPLE_KEY_MAP_AGGREGATOR_PROTOCOL  *KeyMap;
  OC_BOOT_ENTRY                      *Chosen;
  OC_BOOT_ENTRY                      *Entries;
  UINTN                              EntryCount;
  INTN                               DefaultEntry;
  INTN                               CurrentDefault;

  DefaultEntry = HotkeyNumber;
  
  AppleBootPolicy = OcAppleBootPolicyInstallProtocol (FALSE);
  if (AppleBootPolicy == NULL) {
    DEBUG ((DEBUG_ERROR, "OCB: AppleBootPolicy locate failure\n"));
    return EFI_NOT_FOUND;
  }
  
  KeyMap = OcAppleKeyMapInstallProtocols (FALSE);
  if (KeyMap == NULL) {
    DEBUG ((DEBUG_ERROR, "OCB: AppleKeyMap locate failure\n"));
    return EFI_NOT_FOUND;
  }

  if (Context->PickerCommand != OcPickerDefault) {
    Status = Context->RequestPrivilege (
                        Context->PrivilegeContext,
                        OcPrivilegeAuthorized
                        );
    if (EFI_ERROR (Status)) {
      if (Status != EFI_ABORTED) {
        ASSERT (FALSE);
        return Status;
      }

      Context->PickerCommand = OcPickerDefault;
    }
  } else {
      DefaultEntry = DefaultEntry >= 0 ?  DefaultEntry : OcLoadPickerHotKeys (Context);
  }

  while (TRUE) {
    DEBUG ((DEBUG_INFO, "OCB: Performing OcScanForBootEntries...\n"));

    Status = OcScanForBootEntries (
      AppleBootPolicy,
      Context,
      &Entries,
      &EntryCount,
      NULL,
      TRUE
      );

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "OCB: OcScanForBootEntries failure - %r\n", Status));
      return Status;
    }

    if (EntryCount == 0) {
      DEBUG ((DEBUG_WARN, "OCB: OcScanForBootEntries has no entries\n"));
      return EFI_NOT_FOUND;
    }

    DEBUG ((
      DEBUG_INFO,
      "OCB: Performing OcShowSimpleBootMenu... %d - %d entries found\n",
      Context->PollAppleHotKeys,
      EntryCount
      ));
    
    if (DefaultEntry < 0) {
      DefaultEntry = OcLoadPickerHotKeys (Context);
      HotkeyNumber = DefaultEntry;
    }
    
    CurrentDefault = OcGetDefaultBootEntry (Context, Entries, EntryCount);
    DefaultEntry = (DefaultEntry >= 0 && DefaultEntry < EntryCount) ?  DefaultEntry : CurrentDefault;

    if (Context->PickerCommand == OcPickerShowPicker && HotkeyNumber < 0) {
      Status = OcShowSimpleBootMenu (
        Context,
        Entries,
        EntryCount,
        DefaultEntry,
        &Chosen
        );
    } else if (Context->PickerCommand == OcPickerResetNvram) {
      return InternalSystemActionResetNvram ();
    } else {
      Chosen = &Entries[DefaultEntry];
      if ((CurrentDefault != DefaultEntry && !Context->AllowSetDefault && !Chosen->Hidden)
          || Context->PickerCommand == OcPickerBootWindows
          || Context->PickerCommand == OcPickerBootApple
          ) {
        Status = OcSetDefaultBootEntry (Context, Chosen);
        DEBUG ((DEBUG_INFO, "OCB: New default was set - %r\n", Status));
      }
      Status = EFI_SUCCESS;
    }

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "OCB: OcShowSimpleBootMenu failed - %r\n", Status));
      OcFreeBootEntries (Entries, EntryCount);
      return Status;
    }

    Context->TimeoutSeconds = 0;

    if (!EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_INFO,
        "OCB: Select to boot from %s (T:%d|F:%d)\n",
        Chosen->Name,
        Chosen->Type,
        Chosen->IsFolder
        ));
    }

    if (!EFI_ERROR (Status)) {
      Status = OcLoadBootEntry (
        AppleBootPolicy,
        Context,
        Chosen,
        gImageHandle
        );
      
      //
      // Do not wait on successful return code.
      //
      if (EFI_ERROR (Status)) {
        gBS->Stall (SECONDS_TO_MICROSECONDS (5));
        //
        // Show picker on first failure.
        //
        Context->PickerCommand = OcPickerShowPicker;
      }
      //
      // Ensure that we flush all pressed keys after the application.
      // This resolves the problem of application-pressed keys being used to control the menu.
      //
      OcKeyMapFlush (KeyMap, 0, TRUE);
    }
    
    if (Entries != NULL) {
      OcFreeBootEntries (Entries, EntryCount);
    }
  }
}
