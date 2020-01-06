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

#include <IndustryStandard/AppleHibernate.h>
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
OcDescribeBootEntry (
  IN     APPLE_BOOT_POLICY_PROTOCOL *BootPolicy,
  IN OUT OC_BOOT_ENTRY              *BootEntry
  )
{
  EFI_STATUS                       Status;
  CHAR16                           *BootDirectoryName;
  CHAR16                           *RecoveryBootName;
  EFI_HANDLE                       Device;
  EFI_HANDLE                       ApfsVolumeHandle;
  UINT32                           BcdSize;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;

  //
  // Custom entries need no special description.
  //
  if (BootEntry->Type == OcBootCustom) {
    return EFI_SUCCESS;
  }

  Status = BootPolicy->DevicePathToDirPath (
    BootEntry->DevicePath,
    &BootDirectoryName,
    &Device,
    &ApfsVolumeHandle
    );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->HandleProtocol (
    Device,
    &gEfiSimpleFileSystemProtocolGuid,
    (VOID **) &FileSystem
    );

  if (EFI_ERROR (Status)) {
    FreePool (BootDirectoryName);
    return Status;
  }

  //
  // Try to use APFS-style label or legacy HFS one.
  //
  BootEntry->Name = InternalGetAppleDiskLabel (FileSystem, BootDirectoryName, L".contentDetails");
  if (BootEntry->Name == NULL) {
    BootEntry->Name = InternalGetAppleDiskLabel (FileSystem, BootDirectoryName, L".disk_label.contentDetails");
  }

  //
  // With FV2 encryption on HFS+ the actual boot happens from "Recovery HD/S/L/CoreServices".
  // For some reason "Recovery HD/S/L/CoreServices/.disk_label" may not get updated immediately,
  // and will contain "Recovery HD" despite actually pointing to "Macintosh HD".
  // This also spontaneously happens with renamed APFS volumes. The workaround is to manually
  // edit the file or sometimes choose the boot volume once more in preferences.
  //
  // TODO: Bugreport this to Apple, as this is clearly their bug, which should be reproducible
  // on original hardware.
  //
  // There exists .root_uuid, which contains real partition UUID in ASCII, however, Apple
  // BootPicker only uses it for entry deduplication, and we cannot figure out the name
  // on an encrypted volume anyway.
  //
  BootEntry->Hidden = FALSE;
  //
  // Windows boot entry may have a custom name, so ensure OcBootWindows is set correctly.
  //
  if (BootEntry->Type == OcBootUnknown) {
    DEBUG ((DEBUG_INFO, "Trying to detect Microsoft BCD\n"));
    Status = ReadFileSize (FileSystem, L"\\EFI\\Microsoft\\Boot\\BCD", &BcdSize);
    if (!EFI_ERROR (Status)) {
      BootEntry->Type = OcBootWindows;
      if (BootEntry->Name == NULL) {
        BootEntry->Name = AllocateCopyPool (sizeof (L"BOOTCAMP Windows"), L"BOOTCAMP Windows");
      }
    }
  }

  if (BootEntry->Name == NULL) {
    BootEntry->Name = GetVolumeLabel (FileSystem);
    if (BootEntry->Name != NULL
      && (!StrCmp (BootEntry->Name, L"Recovery HD")
       || !StrCmp (BootEntry->Name, L"Recovery"))) {
      if (BootEntry->Type == OcBootUnknown || BootEntry->Type == OcBootApple) {
        BootEntry->Type = OcBootAppleRecovery;
        BootEntry->Hidden = TRUE;
      }
      RecoveryBootName = InternalGetAppleRecoveryName (FileSystem, BootDirectoryName);
      if (RecoveryBootName != NULL) {
        FreePool (BootEntry->Name);
        BootEntry->Name = RecoveryBootName;
      }
    }
  }

  if (BootEntry->Name == NULL) {
    FreePool (BootDirectoryName);
    return EFI_NOT_FOUND;
  }

  BootEntry->PathName = BootDirectoryName;

  return EFI_SUCCESS;
}

VOID
OcResetBootEntry (
  IN OUT OC_BOOT_ENTRY              *BootEntry
  )
{
  if (BootEntry->DevicePath != NULL) {
    FreePool (BootEntry->DevicePath);
    BootEntry->DevicePath = NULL;
  }

  if (BootEntry->Name != NULL) {
    FreePool (BootEntry->Name);
    BootEntry->Name = NULL;
  }

  if (BootEntry->PathName != NULL) {
    FreePool (BootEntry->PathName);
    BootEntry->PathName = NULL;
  }

  if (BootEntry->LoadOptions != NULL) {
    FreePool (BootEntry->LoadOptions);
    BootEntry->LoadOptions     = NULL;
    BootEntry->LoadOptionsSize = 0;
  }
  
  BootEntry->Hidden = FALSE;
}

VOID
OcFreeBootEntries (
  IN OUT OC_BOOT_ENTRY              *BootEntries,
  IN     UINTN                      Count
  )
{
  UINTN  Index;

  for (Index = 0; Index < Count; ++Index) {
    OcResetBootEntry (&BootEntries[Index]);
  }

  FreePool (BootEntries);
}

/**
  Resets selected NVRAM variables and reboots the system.

**/
EFI_STATUS
InternalSystemActionResetNvram (
  VOID
  )
{
  OcDeleteVariables ();
  DirectRestCold ();
  return EFI_DEVICE_ERROR;
}

STATIC
UINTN
internalFillCustomBootEntries (
  IN     OC_PICKER_CONTEXT         *Context,
  IN OUT OC_BOOT_ENTRY             *Entries,
  IN     UINTN                     EntryIndex,
     OUT BOOLEAN                   BootWindowsFound
  )
{
  UINTN                            Index;
  CHAR16                           *PathName;
  CONST FILEPATH_DEVICE_PATH       *FilePath;
  
  for (Index = 0; Index < Context->AbsoluteEntryCount; ++Index) {
    Entries[EntryIndex].Name = AsciiStrCopyToUnicode (Context->CustomEntries[Index].Name, 0);
    PathName                 = AsciiStrCopyToUnicode (Context->CustomEntries[Index].Path, 0);
    if (Entries[EntryIndex].Name == NULL || PathName == NULL) {
      OcFreeBootEntries (Entries, EntryIndex + 1);
      return EFI_OUT_OF_RESOURCES;
    }
    // Properly re-assign the boot type for custom entries according the pathname instead of using OcBootCustom.

    if (StrStr(PathName, L"\\EFI\\Microsoft\\Boot") != NULL) {
      Entries[EntryIndex].Type = OcBootWindows;
    } else if (StrStr(PathName, L"\\System\\Library\\CoreServices\\boot.efi") != NULL) {
      Entries[EntryIndex].Type = OcBootApple;
    } else {
      Entries[EntryIndex].Type = OcBootCustom;
    }
    //
    // Check for possible Windows entry in custom entries if not yet found with auto scan when Windows boot
    // was called with hotkey W, Will skip the rest if find one here.
    //
    if (Context->PickerCommand == OcPickerBootWindows && Entries[EntryIndex].Type == OcBootWindows) {
        Index = Context->AbsoluteEntryCount;
        BootWindowsFound = TRUE;
    }
    
    Entries[EntryIndex].DevicePath = ConvertTextToDevicePath (PathName);
    FreePool (PathName);
    if (Entries[EntryIndex].DevicePath == NULL) {
      FreePool (Entries[EntryIndex].Name);
      continue;
    }

    FilePath = (FILEPATH_DEVICE_PATH *)(
                 FindDevicePathNodeWithType (
                   Entries[EntryIndex].DevicePath,
                   MEDIA_DEVICE_PATH,
                   MEDIA_FILEPATH_DP
                   )
                 );
    if (FilePath == NULL) {
      FreePool (Entries[EntryIndex].Name);
      FreePool (Entries[EntryIndex].DevicePath);
      continue;
    }

    Entries[EntryIndex].PathName = AllocateCopyPool (
                                     OcFileDevicePathNameSize (FilePath),
                                     FilePath->PathName
                                     );
    if (Entries[EntryIndex].PathName == NULL) {
      FreePool (Entries[EntryIndex].Name);
      FreePool (Entries[EntryIndex].DevicePath);
      continue;
    }
    
    Entries[EntryIndex].LoadOptionsSize = (UINT32) AsciiStrLen (Context->CustomEntries[Index].Arguments);
    if (Entries[EntryIndex].LoadOptionsSize > 0) {
      Entries[EntryIndex].LoadOptions = AllocateCopyPool (
        Entries[EntryIndex].LoadOptionsSize + 1,
        Context->CustomEntries[Index].Arguments
        );
      if (Entries[EntryIndex].LoadOptions == NULL) {
        Entries[EntryIndex].LoadOptionsSize = 0;
      }
    }
    
    Entries[EntryIndex].Hidden = Context->CustomEntries[Index].Hidden;
    
    ++EntryIndex;
  }
  
  return EntryIndex;
}

EFI_STATUS
OcScanForBootEntries (
  IN  APPLE_BOOT_POLICY_PROTOCOL  *BootPolicy,
  IN  OC_PICKER_CONTEXT           *Context,
  OUT OC_BOOT_ENTRY               **BootEntries,
  OUT UINTN                       *Count,
  OUT UINTN                       *AllocCount OPTIONAL,
  IN  BOOLEAN                     Describe
  )
{
  EFI_STATUS                       Status;
  BOOLEAN                          Result;

  UINTN                            NoHandles;
  EFI_HANDLE                       *Handles;
  UINTN                            Index;
  OC_BOOT_ENTRY                    *Entries;
  UINTN                            EntriesSize;
  UINTN                            EntryIndex;
  CHAR16                           *PathName;
  CHAR16                           *DevicePathText;

  UINTN                            DevPathScanInfoSize;
  INTERNAL_DEV_PATH_SCAN_INFO      *DevPathScanInfo;
  INTERNAL_DEV_PATH_SCAN_INFO      *DevPathScanInfos;
  EFI_DEVICE_PATH_PROTOCOL         *DevicePathWalker;
  BOOLEAN                          BootWindowsFound;

  Result = OcOverflowMulUN (Context->AllCustomEntryCount, sizeof (OC_BOOT_ENTRY), &EntriesSize);
  if (Result) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (Context->ShowNvramReset) {
    Result = OcOverflowAddUN (EntriesSize, sizeof (OC_BOOT_ENTRY), &EntriesSize);
    if (Result) {
      return EFI_OUT_OF_RESOURCES;
    }
  }

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &NoHandles,
                  &Handles
                  );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((DEBUG_INFO, "OCB: Found %u potentially bootable filesystems\n", (UINT32) NoHandles));

  if (NoHandles == 0) {
    FreePool (Handles);
    return EFI_NOT_FOUND;
  }

  Result = OcOverflowMulUN (
             NoHandles,
             sizeof (*DevPathScanInfos),
             &DevPathScanInfoSize
             );
  if (Result) {
    FreePool (Handles);
    return EFI_OUT_OF_RESOURCES;
  }

  DevPathScanInfos = AllocateZeroPool (DevPathScanInfoSize);
  if (DevPathScanInfos == NULL) {
    FreePool (Handles);
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index < NoHandles; ++Index) {
    DevPathScanInfo = &DevPathScanInfos[Index];

    Status = InternalPrepareScanInfo (
      BootPolicy,
      Context,
      Handles,
      Index,
      DevPathScanInfo
      );

    if (EFI_ERROR (Status)) {
      continue;
    }

    ASSERT (DevPathScanInfo->NumBootInstances > 0);

    Result = OcOverflowMulAddUN (
               DevPathScanInfo->NumBootInstances,
               2 * sizeof (OC_BOOT_ENTRY),
               EntriesSize,
               &EntriesSize
               );
    if (Result) {
      FreePool (Handles);
      FreePool (DevPathScanInfos);
      return EFI_OUT_OF_RESOURCES;
    }
  }
  //
  // Errors from within the loop are not fatal.
  //
  Status = EFI_SUCCESS;

  FreePool (Handles);

  if (EntriesSize == 0) {
    FreePool (DevPathScanInfos);
    return EFI_NOT_FOUND;
  }

  Entries = AllocateZeroPool (EntriesSize);
  if (Entries == NULL) {
    FreePool (DevPathScanInfos);
    return EFI_OUT_OF_RESOURCES;
  }
  
  BootWindowsFound = FALSE;
  EntryIndex = 0;
  
  EntryIndex = internalFillCustomBootEntries (
                 Context,
                 Entries,
                 EntryIndex,
                 BootWindowsFound
                 );
  
  if (Context->PickerCommand != OcPickerBootWindows || !BootWindowsFound) {
    for (Index = 0; Index < NoHandles; ++Index) {
      DevPathScanInfo = &DevPathScanInfos[Index];

      DevicePathWalker = DevPathScanInfo->BootDevicePath;
      if (DevicePathWalker == NULL) {
        continue;
      }

      EntryIndex = InternalFillValidBootEntries (
                     BootPolicy,
                     Context,
                     DevPathScanInfo,
                     DevicePathWalker,
                     Entries,
                     EntryIndex
                     );

      FreePool (DevPathScanInfo->BootDevicePath);
    }

    FreePool (DevPathScanInfos);
    
    if (Describe) {
      DEBUG ((DEBUG_INFO, "Scanning got %u entries\n", (UINT32) EntryIndex));

      for (Index = Context->AbsoluteEntryCount; Index < EntryIndex; ++Index) {
        Status = OcDescribeBootEntry (BootPolicy, &Entries[Index]);
        if (EFI_ERROR (Status)) {
          break;
        }
        
        DEBUG_CODE_BEGIN ();
        DEBUG ((
          DEBUG_INFO,
          "Entry %u is %s at %s (T:%d|F:%d)\n",
          (UINT32) Index,
          Entries[Index].Name,
          Entries[Index].PathName,
          Entries[Index].Type,
          Entries[Index].IsFolder
          ));

        DevicePathText = ConvertDevicePathToText (Entries[Index].DevicePath, FALSE, FALSE);
        if (DevicePathText != NULL) {
          DEBUG ((
            DEBUG_INFO,
            "Entry %u is %s at dp %s\n",
            (UINT32) Index,
            Entries[Index].Name,
            DevicePathText
            ));
          FreePool (DevicePathText);
        }
        DEBUG_CODE_END ();
      }

      if (EFI_ERROR (Status)) {
        OcFreeBootEntries (Entries, EntryIndex);
        return Status;
      }
    }
    
    for (Index = Context->AbsoluteEntryCount; Index < Context->AllCustomEntryCount; ++Index) {
      Entries[EntryIndex].Name = AsciiStrCopyToUnicode (Context->CustomEntries[Index].Name, 0);
      PathName                 = AsciiStrCopyToUnicode (Context->CustomEntries[Index].Path, 0);
      if (Entries[EntryIndex].Name == NULL || PathName == NULL) {
        OcFreeBootEntries (Entries, EntryIndex + 1);
        return EFI_OUT_OF_RESOURCES;
      }
      
      Entries[EntryIndex].Type = OcBootCustom;
      Entries[EntryIndex].Hidden = TRUE;

      UnicodeUefiSlashes (PathName);
      Entries[EntryIndex].PathName = PathName;

      Entries[EntryIndex].LoadOptionsSize = (UINT32) AsciiStrLen (Context->CustomEntries[Index].Arguments);
      if (Entries[EntryIndex].LoadOptionsSize > 0) {
        Entries[EntryIndex].LoadOptions = AllocateCopyPool (
          Entries[EntryIndex].LoadOptionsSize + 1,
          Context->CustomEntries[Index].Arguments
          );
        if (Entries[EntryIndex].LoadOptions == NULL) {
          Entries[EntryIndex].LoadOptionsSize = 0;
        }
      }
      ++EntryIndex;
    }

    if (Context->ShowNvramReset) {
      Entries[EntryIndex].Name = AllocateCopyPool (
                                   L_STR_SIZE (L"Reset NVRAM"),
                                   L"Reset NVRAM"
                                   );
      if (Entries[EntryIndex].Name == NULL) {
        OcFreeBootEntries (Entries, EntryIndex + 1);
        return EFI_OUT_OF_RESOURCES;
      }

      Entries[EntryIndex].Type         = OcBootSystem;
      Entries[EntryIndex].Hidden       = FALSE;
      Entries[EntryIndex].SystemAction = InternalSystemActionResetNvram;
      ++EntryIndex;
    }
  }

  *BootEntries = Entries;
  *Count       = EntryIndex;

  ASSERT (*Count <= EntriesSize / sizeof (OC_BOOT_ENTRY));

  if (AllocCount != NULL) {
    *AllocCount = EntriesSize / sizeof (OC_BOOT_ENTRY);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
OcActivateHibernateWake (
  IN UINT32                       HibernateMask
  )
{
  EFI_STATUS               Status;
  UINTN                    Size;
  UINT32                   Attributes;
  VOID                     *Value;
  AppleRTCHibernateVars    RtcVars;
  BOOLEAN                  HasHibernateInfo;
  BOOLEAN                  HasHibernateInfoInRTC;
  UINT8                    Index;
  UINT8                    *RtcRawVars;
  EFI_DEVICE_PATH_PROTOCOL *BootImagePath;
  EFI_DEVICE_PATH_PROTOCOL *RemainingPath;
  INTN                     NumPatchedNodes;

  if (HibernateMask == HIBERNATE_MODE_NONE) {
    return EFI_NOT_FOUND;
  }

  HasHibernateInfo = FALSE;
  HasHibernateInfoInRTC = FALSE;

  //
  // If legacy boot-switch-vars exists (NVRAM working), then use it.
  //
  Status = GetVariable2 (L"boot-switch-vars", &gAppleBootVariableGuid, &Value, &Size);
  if (!EFI_ERROR (Status)) {
    //
    // Leave it as is.
    //
    SecureZeroMem (Value, Size);
    FreePool (Value);
    DEBUG ((DEBUG_INFO, "OCB: Found legacy boot-switch-vars\n"));
    return EFI_SUCCESS;
  }

  Status = GetVariable3 (
             L"boot-image",
             &gAppleBootVariableGuid,
             (VOID **)&BootImagePath,
             &Size,
             &Attributes
             );
  if (!EFI_ERROR (Status)) {
    if (IsDevicePathValid (BootImagePath, Size)) {
      DebugPrintDevicePath (
        DEBUG_INFO,
        "OCB: boot-image pre-fix",
        BootImagePath
        );

      RemainingPath   = BootImagePath;
      NumPatchedNodes = OcFixAppleBootDevicePath (&RemainingPath);
      if (NumPatchedNodes > 0) {
        DebugPrintDevicePath (
          DEBUG_INFO,
          "OCB: boot-image post-fix",
          BootImagePath
          );

        Status = gRT->SetVariable (
                        L"boot-image",
                        &gAppleBootVariableGuid,
                        Attributes,
                        Size,
                        BootImagePath
                        );
      }
      if (NumPatchedNodes >= 0) {
        DebugPrintDevicePath (
          DEBUG_INFO,
          "OCB: boot-image post-fix remainder",
          RemainingPath
          );
      }
    } else {
      DEBUG ((DEBUG_INFO, "OCB: Invalid boot-image variable\n"));
    }

    SecureZeroMem (BootImagePath, Size);
    FreePool (BootImagePath);
  }

  DEBUG ((DEBUG_INFO, "OCB: boot-image is %u bytes - %r\n", (UINT32) Size, Status));

  //
  // Work with RTC memory if allowed.
  //
  if (HibernateMask & HIBERNATE_MODE_RTC) {
    RtcRawVars = (UINT8 *) &RtcVars;
    for (Index = 0; Index < sizeof (AppleRTCHibernateVars); Index++) {
      RtcRawVars[Index] = OcRtcRead (Index + 128);
    }

    HasHibernateInfoInRTC = RtcVars.signature[0] == 'A'
                         && RtcVars.signature[1] == 'A'
                         && RtcVars.signature[2] == 'P'
                         && RtcVars.signature[3] == 'L';
    HasHibernateInfo = HasHibernateInfoInRTC;

    DEBUG ((DEBUG_INFO, "OCB: RTC hibernation is %d\n", HasHibernateInfoInRTC));
  }

  if (HibernateMask & HIBERNATE_MODE_NVRAM) {
    //
    // If RTC variables is still written to NVRAM (and RTC is broken).
    // Prior to 10.13.6.
    //
    Status = GetVariable2 (L"IOHibernateRTCVariables", &gAppleBootVariableGuid, &Value, &Size);
    if (!HasHibernateInfo && !EFI_ERROR (Status) && Size == sizeof (RtcVars)) {
      CopyMem (RtcRawVars, Value, sizeof (RtcVars));
      HasHibernateInfo = RtcVars.signature[0] == 'A'
                      && RtcVars.signature[1] == 'A'
                      && RtcVars.signature[2] == 'P'
                      && RtcVars.signature[3] == 'L';
    }

    DEBUG ((
      DEBUG_INFO,
      "OCB: NVRAM hibernation is %d / %r / %u\n",
      HasHibernateInfo,
      Status,
      (UINT32) Size
      ));

    //
    // Erase RTC variables in NVRAM.
    //
    if (!EFI_ERROR (Status)) {
      Status = gRT->SetVariable (
        L"IOHibernateRTCVariables",
        &gAppleBootVariableGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        0,
        NULL
        );
      SecureZeroMem (Value, Size);
      FreePool (Value);
    }
  }

  //
  // Convert RTC data to boot-key and boot-signature
  //
  if (HasHibernateInfo) {
    gRT->SetVariable (
      L"boot-image-key",
      &gAppleBootVariableGuid,
      EFI_VARIABLE_BOOTSERVICE_ACCESS,
      sizeof (RtcVars.wiredCryptKey),
      RtcVars.wiredCryptKey
      );
    gRT->SetVariable (
      L"boot-signature",
      &gAppleBootVariableGuid,
      EFI_VARIABLE_BOOTSERVICE_ACCESS,
      sizeof (RtcVars.booterSignature),
      RtcVars.booterSignature
      );
  }

  //
  // Erase RTC memory similarly to AppleBds.
  //
  if (HasHibernateInfoInRTC) {
    SecureZeroMem (RtcRawVars, sizeof(AppleRTCHibernateVars));
    RtcVars.signature[0] = 'D';
    RtcVars.signature[1] = 'E';
    RtcVars.signature[2] = 'A';
    RtcVars.signature[3] = 'D';

    for (Index = 0; Index < sizeof(AppleRTCHibernateVars); Index++) {
      OcRtcWrite (Index + 128, RtcRawVars[Index]);
    }
  }

  //
  // We have everything we need now.
  //
  if (HasHibernateInfo) {
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

BOOLEAN
OcIsAppleHibernateWake (
  VOID
  )
{
  EFI_STATUS   Status;
  UINTN        ValueSize;

  //
  // This is reverse engineered from boot.efi.
  // To cancel hibernate wake it is enough to delete the variables.
  // Starting with 10.13.6 boot-switch-vars is no longer supported.
  //
  ValueSize = 0;
  Status = gRT->GetVariable (
    L"boot-signature",
    &gAppleBootVariableGuid,
    NULL,
    &ValueSize,
    NULL
    );

  if (Status == EFI_BUFFER_TOO_SMALL) {
    ValueSize = 0;
    Status = gRT->GetVariable (
      L"boot-image-key",
      &gAppleBootVariableGuid,
      NULL,
      &ValueSize,
      NULL
      );

    if (Status == EFI_BUFFER_TOO_SMALL) {
      return TRUE;
    }
  } else {
    ValueSize = 0;
    Status = gRT->GetVariable (
      L"boot-switch-vars",
      &gAppleBootVariableGuid,
      NULL,
      &ValueSize,
      NULL
      );

    if (Status == EFI_BUFFER_TOO_SMALL) {
      return TRUE;
    }
  }

  return FALSE;
}

EFI_STATUS
OcLoadBootEntry (
  IN  APPLE_BOOT_POLICY_PROTOCOL  *BootPolicy,
  IN  OC_PICKER_CONTEXT           *Context,
  IN  OC_BOOT_ENTRY               *BootEntry,
  IN  EFI_HANDLE                  ParentHandle
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 EntryHandle;
  INTERNAL_DMG_LOAD_CONTEXT  DmgLoadContext;

  if (BootEntry->Type == OcBootSystem) {
    ASSERT (BootEntry->SystemAction != NULL);
    return BootEntry->SystemAction ();
  }

  Status = InternalLoadBootEntry (
    BootPolicy,
    Context,
    BootEntry,
    ParentHandle,
    &EntryHandle,
    &DmgLoadContext
    );
  if (!EFI_ERROR (Status)) {
    Status = Context->StartImage (BootEntry, EntryHandle, NULL, NULL);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "OCB: StartImage failed - %r\n", Status));
      //
      // Unload dmg if any.
      //
      InternalUnloadDmg (&DmgLoadContext);
    }
  } else {
    DEBUG ((DEBUG_ERROR, "OCB: LoadImage failed - %r\n", Status));
  }

  return Status;
}

VOID
OcLoadPickerHotKeys (
  IN OUT OC_PICKER_CONTEXT  *Context
  )
{
  EFI_STATUS                         Status;
  APPLE_KEY_MAP_AGGREGATOR_PROTOCOL  *KeyMap;

  UINTN                              NumKeys;
  APPLE_MODIFIER_MAP                 Modifiers;
  APPLE_KEY_CODE                     Keys[8];

  BOOLEAN                            HasCommand;
  BOOLEAN                            HasEscape;
  BOOLEAN                            HasOption;
  BOOLEAN                            HasKeyP;
  BOOLEAN                            HasKeyR;
  BOOLEAN                            HasKeyW;
  BOOLEAN                            HasKeyX;

  Status = gBS->LocateProtocol (
    &gAppleKeyMapAggregatorProtocolGuid,
    NULL,
    (VOID **) &KeyMap
    );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "OCB: Missing AppleKeyMapAggregator - %r\n", Status));
    return;
  }

  NumKeys = ARRAY_SIZE (Keys);
  Status = KeyMap->GetKeyStrokes (
                     KeyMap,
                     &Modifiers,
                     &NumKeys,
                     Keys
                     );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "OCB: GetKeyStrokes - %r\n", Status));
    return;
  }

  //
  // I do not like this code a little, as it is prone to race conditions during key presses.
  // For the good false positives are not too critical here, and in reality users are not that fast.
  //
  // Reference key list:
  // https://support.apple.com/HT201255
  // https://support.apple.com/HT204904
  //
  // We are slightly more permissive than AppleBds, as we permit combining keys.
  //

  HasCommand = (Modifiers & (APPLE_MODIFIER_LEFT_COMMAND | APPLE_MODIFIER_RIGHT_COMMAND)) != 0;
  HasOption  = (Modifiers & (APPLE_MODIFIER_LEFT_OPTION  | APPLE_MODIFIER_RIGHT_OPTION)) != 0;
  HasEscape  = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyEscape);
  HasKeyP    = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyP);
  HasKeyR    = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyR);
  HasKeyW    = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyW);
  HasKeyX    = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyX);

  if (HasOption && HasCommand && HasKeyP && HasKeyR) {
    DEBUG ((DEBUG_INFO, "OCB: CMD+OPT+P+R causes NVRAM reset\n"));
    Context->PickerCommand = OcPickerResetNvram;
  } else if (HasCommand && HasKeyR) {
    DEBUG ((DEBUG_INFO, "OCB: CMD+R causes recovery to boot\n"));
    Context->PickerCommand = OcPickerBootAppleRecovery;
  } else if (HasKeyW) {
    DEBUG ((DEBUG_INFO, "OCB: W causes Windows to boot\n"));
    Context->PickerCommand = OcPickerBootWindows;
  } else if (HasKeyX) {
    DEBUG ((DEBUG_INFO, "OCB: X causes macOS to boot\n"));
    Context->PickerCommand = OcPickerBootApple;
  } else if (HasOption) {
    DEBUG ((DEBUG_INFO, "OCB: OPT causes picker to show\n"));
    Context->PickerCommand = OcPickerShowPicker;
  } else if (HasEscape) {
    DEBUG ((DEBUG_INFO, "OCB: ESC causes picker to show as OC extension\n"));
    Context->PickerCommand = OcPickerShowPicker;
  } else {
    //
    // In addition to these overrides we always have ShowPicker = YES in config.
    // The following keys are not implemented:
    // C - CD/DVD boot, legacy that is gone now.
    // D - Diagnostics, could implement dumping stuff here in some future,
    //     but we will need to store the data before handling the key.
    //     Should also be DEBUG only for security reasons.
    // N - Network boot, simply not supported (and bad for security).
    // T - Target disk mode, simply not supported (and bad for security).
    //
  }
}

INTN
OcWaitForAppleKeyIndex (
  IN OUT OC_PICKER_CONTEXT  *Context,
  IN UINTN                  Timeout
  )
{
  EFI_STATUS                         Status;
  APPLE_KEY_MAP_AGGREGATOR_PROTOCOL  *KeyMap;
  APPLE_KEY_CODE                     KeyCode;

  UINTN                              NumKeys;
  APPLE_MODIFIER_MAP                 Modifiers;
  APPLE_KEY_CODE                     Keys[8];

  BOOLEAN                            HasCommand;
  BOOLEAN                            HasShift;
  BOOLEAN                            HasKeyC;
  BOOLEAN                            HasKeyK;
  BOOLEAN                            HasKeyS;
  BOOLEAN                            HasKeyV;
  BOOLEAN                            HasKeyMinus;
  BOOLEAN                            WantsZeroSlide;
  UINT32                             CsrActiveConfig;
  UINT64                             CurrTime;
  UINT64                             EndTime;
  UINTN                              CsrActiveConfigSize;

  //
  // These hotkeys are normally parsed by boot.efi, and they work just fine
  // when ShowPicker is disabled. On some BSPs, however, they may fail badly
  // when ShowPicker is enabled, and for this reason we support these hotkeys
  // within picker itself.
  //
  KeyMap = OcAppleKeyMapInstallProtocols (FALSE);
  if (KeyMap == NULL) {
    DEBUG ((DEBUG_ERROR, "OCB: Missing AppleKeyMapAggregator\n"));
    return OC_INPUT_INVALID;
  }

  CurrTime  = GetTimeInNanoSecond (GetPerformanceCounter ());
  EndTime   = CurrTime + Timeout * 1000000000ULL;

  while (Timeout == 0 || CurrTime == 0 || CurrTime < EndTime) {
    NumKeys = ARRAY_SIZE (Keys);
    Status = KeyMap->GetKeyStrokes (
                       KeyMap,
                       &Modifiers,
                       &NumKeys,
                       Keys
                       );

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "OCB: GetKeyStrokes - %r\n", Status));
      return OC_INPUT_INVALID;
    }

    CurrTime    = GetTimeInNanoSecond (GetPerformanceCounter ());

    HasCommand = (Modifiers & (APPLE_MODIFIER_LEFT_COMMAND | APPLE_MODIFIER_RIGHT_COMMAND)) != 0;
    HasShift   = (Modifiers & (APPLE_MODIFIER_LEFT_SHIFT | APPLE_MODIFIER_RIGHT_SHIFT)) != 0;
    HasKeyC    = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyC);
    HasKeyK    = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyK);
    HasKeyS    = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyS);
    HasKeyV    = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyV);
    //
    // Checking for PAD minus is our extension to support more keyboards.
    //
    HasKeyMinus = OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyMinus)
      || OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyPadMinus);

    //
    // Shift is always valid and enables Safe Mode.
    //
    if (HasShift) {
      if (OcGetArgumentFromCmd (Context->AppleBootArgs, "-x", L_STR_LEN ("-x")) == NULL) {
        DEBUG ((DEBUG_INFO, "OCB: Shift means -x\n"));
        OcAppendArgumentToCmd (Context, Context->AppleBootArgs, "-x", L_STR_LEN ("-x"));
      }
      continue;
    }

    //
    // CMD+V is always valid and enables Verbose Mode.
    //
    if (HasCommand && HasKeyV) {
      if (OcGetArgumentFromCmd (Context->AppleBootArgs, "-v", L_STR_LEN ("-v")) == NULL) {
        DEBUG ((DEBUG_INFO, "OCB: CMD+V means -v\n"));
        OcAppendArgumentToCmd (Context, Context->AppleBootArgs, "-v", L_STR_LEN ("-v"));
      }
      continue;
    }

    //
    // CMD+C+MINUS is always valid and disables compatibility check.
    //
    if (HasCommand && HasKeyC && HasKeyMinus) {
      if (OcGetArgumentFromCmd (Context->AppleBootArgs, "-no_compat_check", L_STR_LEN ("-no_compat_check")) == NULL) {
        DEBUG ((DEBUG_INFO, "OCB: CMD+C+MINUS means -no_compat_check\n"));
        OcAppendArgumentToCmd (Context, Context->AppleBootArgs, "-no_compat_check", L_STR_LEN ("-no_compat_check"));
      }
      continue;
    }

    //
    // CMD+K is always valid for new macOS and means force boot to release kernel.
    //
    if (HasCommand && HasKeyK) {
      if (AsciiStrStr (Context->AppleBootArgs, "kcsuffix=release") == NULL) {
        DEBUG ((DEBUG_INFO, "OCB: CMD+K means kcsuffix=release\n"));
        OcAppendArgumentToCmd (Context, Context->AppleBootArgs, "kcsuffix=release", L_STR_LEN ("kcsuffix=release"));
      }
      continue;
    }

    //
    // boot.efi also checks for CMD+X, but I have no idea what it is for.
    //

    //
    // boot.efi requires unrestricted NVRAM just for CMD+S+MINUS, and CMD+S
    // does not work at all on T2 macs. For CMD+S we simulate T2 behaviour with
    // DisableSingleUser Booter quirk if necessary.
    // Ref: https://support.apple.com/HT201573
    //
    if (HasCommand && HasKeyS) {
      WantsZeroSlide = HasKeyMinus;

      if (WantsZeroSlide) {
        CsrActiveConfig     = 0;
        CsrActiveConfigSize = sizeof (CsrActiveConfig);
        Status = gRT->GetVariable (
          L"csr-active-config",
          &gAppleBootVariableGuid,
          NULL,
          &CsrActiveConfigSize,
          &CsrActiveConfig
          );
        //
        // FIXME: CMD+S+Minus behaves as CMD+S when "slide=0" is not supported
        //        by the SIP configuration. This might be an oversight, but is
        //        consistent with the boot.efi implementation.
        //
        WantsZeroSlide = !EFI_ERROR (Status) && (CsrActiveConfig & CSR_ALLOW_UNRESTRICTED_NVRAM) != 0;
      }

      if (WantsZeroSlide) {
        if (AsciiStrStr (Context->AppleBootArgs, "slide=0") == NULL) {
          DEBUG ((DEBUG_INFO, "OCB: CMD+S+MINUS means slide=0\n"));
          OcAppendArgumentToCmd (Context, Context->AppleBootArgs, "slide=0", L_STR_LEN ("slide=0"));
        }
      } else if (OcGetArgumentFromCmd (Context->AppleBootArgs, "-s", L_STR_LEN ("-s")) == NULL) {
        DEBUG ((DEBUG_INFO, "OCB: CMD+S means -s\n"));
        OcAppendArgumentToCmd (Context, Context->AppleBootArgs, "-s", L_STR_LEN ("-s"));
      }
      continue;
    }

    if (OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyEscape)
     || OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyZero)) {
      return OC_INPUT_ABORTED;
    }
    
    if (OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeySpaceBar)) {
      return OC_INPUT_SPACEBAR;
    }
    
    if (OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyEnter)) {
      return OC_INPUT_RETURN;
    }
    
    if (OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyUpArrow)) {
      return OC_INPUT_UP;
    }
    
    if (OcKeyMapHasKey (Keys, NumKeys, AppleHidUsbKbUsageKeyDownArrow)) {
      return OC_INPUT_DOWN;
    }
    //
    // Check exact match on index strokes.
    //
    if (Modifiers == 0 && NumKeys == 1) {
      STATIC_ASSERT (AppleHidUsbKbUsageKeyOne + 8 == AppleHidUsbKbUsageKeyNine, "Unexpected encoding");
      for (KeyCode = AppleHidUsbKbUsageKeyOne; KeyCode <= AppleHidUsbKbUsageKeyNine; ++KeyCode) {
        if (OcKeyMapHasKey (Keys, NumKeys, KeyCode)) {
          return (INTN) (KeyCode - AppleHidUsbKbUsageKeyOne);
        }
      }

      STATIC_ASSERT (AppleHidUsbKbUsageKeyA + 25 == AppleHidUsbKbUsageKeyZ, "Unexpected encoding");
      for (KeyCode = AppleHidUsbKbUsageKeyA; KeyCode <= AppleHidUsbKbUsageKeyZ; ++KeyCode) {
        if (OcKeyMapHasKey (Keys, NumKeys, KeyCode)) {
          return (INTN) (KeyCode - AppleHidUsbKbUsageKeyA + 9);
        }
      }
    }
    //
    // Abort the timeout when unrecognised keys are pressed.
    //
    if (Timeout != 0 && NumKeys != 0) {
      return OC_INPUT_INVALID;
    }

    MicroSecondDelay (10);
  }

  return OC_INPUT_TIMEOUT;
}

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
//
// This function generate and return entry ptr from last booted entry.
//
STATIC
OC_BOOT_ENTRY *
InternalGetLastBootedEntry (
  VOID
  )
{
  EFI_STATUS                       Status;
  OC_BOOT_ENTRY                    *Entry;
  CHAR16                           *DevicePathText;
  EFI_DEVICE_PATH_PROTOCOL         *UefiDevicePath;
  UINTN                            UefiDevicePathSize;
  
  UefiDevicePath = NULL;

  Status = GetVariable2 (
             L"efi-boot-device-data",
             &gAppleBootVariableGuid,
             (VOID **)&UefiDevicePath,
             &UefiDevicePathSize
             );

  if (!EFI_ERROR (Status) && IsDevicePathValid (UefiDevicePath, UefiDevicePathSize)) {
    Entry = AllocateZeroPool (sizeof (OC_BOOT_ENTRY));
    if (Entry == NULL) {
      DEBUG ((DEBUG_INFO, "OCB: Can't allocate memory for Fast Entry\n"));
      FreePool (UefiDevicePath);
      return NULL;
    }
    
    DevicePathText = ConvertDevicePathToText (UefiDevicePath, FALSE, FALSE);
    if (DevicePathText != NULL) {
      if (StrStr(DevicePathText, L"\\EFI\\Microsoft\\Boot") != NULL) {
        Entry->Name = AllocateCopyPool (L_STR_SIZE (L"Last booted Windows"), L"Last booted Windows");
        Entry->Type = OcBootWindows;
      } else if (StrStr(DevicePathText, L"\\System\\Library\\CoreServices\\boot.efi") != NULL) {
        Entry->Name = AllocateCopyPool (L_STR_SIZE (L"Last booted macOS"), L"Last booted macOS");
        Entry->Type = OcBootApple;
      } else {
        FreePool (DevicePathText);
        FreePool (UefiDevicePath);
        return NULL;
      }
      Entry->DevicePath = UefiDevicePath;
      DEBUG ((DEBUG_INFO, "OCB: Found 1: %s\n", DevicePathText));
      FreePool (DevicePathText);
      return Entry;
    }
  }
  
  if (UefiDevicePath != NULL) {
    FreePool (UefiDevicePath);
  }
  
  return NULL;
}

EFI_STATUS
OcRunSimpleBootPicker (
  IN OC_PICKER_CONTEXT  *Context
  )
{
  EFI_STATUS                  Status;
  APPLE_BOOT_POLICY_PROTOCOL  *AppleBootPolicy;
  OC_BOOT_ENTRY               *Chosen;
  OC_BOOT_ENTRY               *Entries;
  UINTN                       EntryCount;
  INTN                        DefaultEntry;

  Chosen = NULL;
  
  AppleBootPolicy = OcAppleBootPolicyInstallProtocol (FALSE);
  if (AppleBootPolicy == NULL) {
    DEBUG ((DEBUG_ERROR, "OCB: AppleBootPolicy locate failure\n"));
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
    OcLoadPickerHotKeys (Context);
  }
  
  if (Context->PickerCommand != OcPickerShowPicker
    && Context->PickerCommand != OcPickerResetNvram) {
    DEBUG ((DEBUG_INFO, "OCB: Checking for last booted entry....\n"));
    Chosen = InternalGetLastBootedEntry();
    if (Chosen !=NULL) {
      if ((Chosen->Type == OcBootApple && Context->PickerCommand == OcPickerBootWindows)
        || (Chosen->Type == OcBootWindows && Context->PickerCommand == OcPickerBootApple)) {
        OcResetBootEntry (Chosen);
        FreePool (Chosen);
        Chosen = NULL;
        DEBUG ((DEBUG_INFO, "OCB: Entry requested does not match!\n"));
      } else {
        DEBUG ((DEBUG_INFO, "OCB: Will boot from last booted entry!\n"));
      }
    }
  }

  while (TRUE) {
    if (Chosen == NULL) {
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
        "OCB: Performing OcShowSimpleBootMenu... %d\n",
        Context->PollAppleHotKeys
        ));
      
      if (Context->PickerCommand == OcPickerDefault) {
        OcLoadPickerHotKeys (Context);
      }
      
      DefaultEntry = OcGetDefaultBootEntry (Context, Entries, EntryCount);

      if (Context->PickerCommand == OcPickerShowPicker) {
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
          "OCB: Should boot from %s (T:%d|F:%d)\n",
          Chosen->Name,
          Chosen->Type,
          Chosen->IsFolder
          ));
      }
    } else {
      Status = EFI_SUCCESS;
    }

    if (!EFI_ERROR (Status)) {
      Status = OcLoadBootEntry (
        AppleBootPolicy,
        Context,
        Chosen,
        gImageHandle
        );

      gBS->Stall (5000000);
    }
    
    if (Chosen != NULL){
      OcResetBootEntry (Chosen);
      FreePool (Chosen);
      Chosen = NULL;
    }
    
    if (Entries != NULL) {
      OcFreeBootEntries (Entries, EntryCount);
    }
  }
}
