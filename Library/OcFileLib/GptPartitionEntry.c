/** @file
  Copyright (C) 2019, Download-Fritz.  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include <Uefi.h>

#include <Guid/Gpt.h>

#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>
#include <Protocol/DiskIo.h>
#include <Protocol/DiskIo2.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/OcDevicePathLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcGuardLib.h>
#include <Library/UefiBootServicesTableLib.h>

typedef struct {
  UINT32              NumPartitions;
  UINT32              PartitionEntrySize;
  EFI_PARTITION_ENTRY FirstEntry[];
} INTERNAL_PARTITION_ENTRIES;

STATIC EFI_GUID mInternalDiskPartitionEntriesProtocolGuid = {
  0x1A81704, 0x3442, 0x4A7D, { 0x87, 0x40, 0xF4, 0xEC, 0x5B, 0xBE, 0x59, 0x77 }
};

STATIC EFI_GUID mInternalPartitionEntryProtocolGuid = {
  0x9FC6B19, 0xB8A1, 0x4A01, { 0x8D, 0xB1, 0x87, 0x94, 0xE7, 0x63, 0x4C, 0xA5 }
};

STATIC
VOID
InternalDebugPrintPartitionEntry (
  IN UINTN                      ErrorLevel,
  IN CONST CHAR8                *Message,
  IN CONST EFI_PARTITION_ENTRY  *PartitionEntry
  )
{
  ASSERT (PartitionEntry != NULL);

  DEBUG ((
    ErrorLevel,
    "%a:\n"
    "- PartitionTypeGUID: %g\n"
    "- UniquePartitionGUID: %g\n"
    "- StartingLBA: %lx\n"
    "- EndingLBA: %lx\n"
    "- Attributes: %lx\n"
    "- PartitionName: %s\n",
    Message,
    PartitionEntry->PartitionTypeGUID,
    PartitionEntry->UniquePartitionGUID,
    PartitionEntry->StartingLBA,
    PartitionEntry->EndingLBA,
    PartitionEntry->Attributes,
    PartitionEntry->PartitionName
    ));
}

STATIC
EFI_STATUS
InternalReadDisk (
  IN  EFI_DISK_IO_PROTOCOL    *DiskIo,
  IN  EFI_DISK_IO2_PROTOCOL   *DiskIo2,
  IN  EFI_BLOCK_IO_PROTOCOL   *BlockIo,
  IN  EFI_BLOCK_IO2_PROTOCOL  *BlockIo2,
  IN  EFI_LBA                 LBA,
  IN  UINTN                   BufferSize,
  OUT VOID                    *Buffer
  )
{
  EFI_STATUS Status;

  ASSERT (DiskIo2 != NULL || DiskIo != NULL);
  ASSERT (BlockIo2 != NULL || BlockIo != NULL);
  ASSERT (Buffer != NULL);
  //
  // The Disk I/O stack of some vendor UEFI implementations is really buggy.
  // Try multiple Block and Disk I/O protocol combinations till one succeeds.
  // This prefers Disk I/O 2 over Disk I/O and prefers pairing Disk I/O 2 with
  // Block I/O 2 and Disk I/O with Block I/O for inner consistency.
  //
  if (DiskIo2 != NULL && BlockIo2 != NULL) {
    Status = DiskIo2->ReadDiskEx (
                        DiskIo2,
                        BlockIo2->Media->MediaId,
                        MultU64x32 (LBA, BlockIo2->Media->BlockSize),
                        NULL,
                        BufferSize,
                        Buffer
                        );
    if (!EFI_ERROR (Status)) {
      return Status;
    }

    DEBUG ((DEBUG_INFO, "OCPI: Read DiskIo2/BlockIo2 %r\n", Status));
  }

  if (DiskIo != NULL && BlockIo != NULL) {
    Status = DiskIo->ReadDisk (
                       DiskIo,
                       BlockIo->Media->MediaId,
                       MultU64x32 (LBA, BlockIo->Media->BlockSize),
                       BufferSize,
                       Buffer
                       );
    if (!EFI_ERROR (Status)) {
      return Status;
    }

    DEBUG ((DEBUG_INFO, "OCPI: Read DiskIo/BlockIo %r\n", Status));
  }

  if (DiskIo2 != NULL && BlockIo != NULL) {
    Status = DiskIo2->ReadDiskEx (
                        DiskIo2,
                        BlockIo->Media->MediaId,
                        MultU64x32 (LBA, BlockIo->Media->BlockSize),
                        NULL,
                        BufferSize,
                        Buffer
                        );
    if (!EFI_ERROR (Status)) {
      return Status;
    }

    DEBUG ((DEBUG_INFO, "OCPI: Read DiskIo2/BlockIo %r\n", Status));
  }

  if (DiskIo != NULL && BlockIo2 != NULL) {
    Status = DiskIo->ReadDisk (
                       DiskIo,
                       BlockIo2->Media->MediaId,
                       MultU64x32 (LBA, BlockIo2->Media->BlockSize),
                       BufferSize,
                       Buffer
                       );
    if (!EFI_ERROR (Status)) {
      return Status;
    }
    
    DEBUG ((DEBUG_INFO, "OCPI: Read DiskIo/BlockIo2 %r\n", Status));
  }

  return Status;
}

STATIC
EFI_HANDLE
InternalPartitionGetDiskHandle (
  IN  EFI_DEVICE_PATH_PROTOCOL  *HdDevicePath,
  IN  UINTN                     HdNodeOffset
  )
{
  EFI_HANDLE               DiskHandle;

  EFI_STATUS               Status;

  EFI_DEVICE_PATH_PROTOCOL *PrefixPath;
  EFI_DEVICE_PATH_PROTOCOL *TempPath;

  ASSERT (HdDevicePath != NULL);
  ASSERT (HdNodeOffset < GetDevicePathSize (HdDevicePath));

  PrefixPath = DuplicateDevicePath (HdDevicePath);
  if (PrefixPath == NULL) {
    DEBUG ((DEBUG_INFO, "OCPI: DP allocation error\n"));
    return NULL;
  }
  //
  // Strip the HD node in order to retrieve the last node supporting Block I/O
  // before it, which is going to be its disk.
  //
  TempPath = (EFI_DEVICE_PATH_PROTOCOL *)((UINTN)PrefixPath + HdNodeOffset);
  SetDevicePathEndNode (TempPath);
  
  TempPath = PrefixPath;
  Status = gBS->LocateDevicePath (
                  &gEfiBlockIo2ProtocolGuid,
                  &TempPath,
                  &DiskHandle
                  );

  if (EFI_ERROR (Status)) {
    TempPath = PrefixPath;
    Status = gBS->LocateDevicePath (
                    &gEfiBlockIoProtocolGuid,
                    &TempPath,
                    &DiskHandle
                    );
  }

  if (EFI_ERROR (Status)) {
    DebugPrintDevicePath (
      DEBUG_INFO,
      "OCPI: Failed to locate disk",
      PrefixPath
      );

    DiskHandle = NULL;
  }

  FreePool (PrefixPath);

  return DiskHandle;
}

/**
  Retrieve the disk's device handle from a partition's Device Path.

  @param[in] HdDevicePath  The Device Path of the partition.

**/
EFI_HANDLE
OcPartitionGetDiskHandle (
  IN EFI_DEVICE_PATH_PROTOCOL  *HdDevicePath
  )
{
  CONST HARDDRIVE_DEVICE_PATH *HdNode;

  ASSERT (HdDevicePath != NULL);

  HdNode = (HARDDRIVE_DEVICE_PATH *)(
             FindDevicePathNodeWithType (
               HdDevicePath,
               MEDIA_DEVICE_PATH,
               MEDIA_HARDDRIVE_DP
               )
             );
  if (HdNode == NULL) {
    return NULL;
  }

  return InternalPartitionGetDiskHandle (
           HdDevicePath,
           (UINTN)HdNode - (UINTN)HdDevicePath
           );
}

/**
  Locate the disk's EFI System Partition.

  @param[in]  DiskDevicePath     The Device Path of the disk to scan.
  @param[out] EspDevicePathSize  The size of the returned Device Path.

**/
EFI_DEVICE_PATH_PROTOCOL *
OcDiskFindSystemPartitionPath (
  IN  CONST EFI_DEVICE_PATH_PROTOCOL  *DiskDevicePath,
  OUT UINTN                           *EspDevicePathSize
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *EspDevicePath;

  EFI_STATUS                Status;
  BOOLEAN                   Result;
  INTN                      CmpResult;

  UINTN                     Index;
  UINTN                     NumHandles;
  EFI_HANDLE                *Handles;
  EFI_HANDLE                Handle;

  UINTN                     DiskDpSize;
  UINTN                     DiskDpCmpSize;
  EFI_DEVICE_PATH_PROTOCOL *HdDevicePath;
  UINTN                     HdDpSize;

  CONST EFI_PARTITION_ENTRY *PartEntry;

  ASSERT (DiskDevicePath != NULL);
  ASSERT (EspDevicePathSize != NULL);

  DebugPrintDevicePath (
    DEBUG_INFO,
    "OCPI: Locating disk's ESP",
    (EFI_DEVICE_PATH_PROTOCOL *)DiskDevicePath
    );

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &NumHandles,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCPI: Failed to locate FS handles\n"));
    return NULL;
  }

  EspDevicePath = NULL;

  DiskDpSize = GetDevicePathSize (DiskDevicePath);
  //
  // The partition's Device Path must be at least as big as the disk's (prefix)
  // plus an additional HardDrive node.
  //
  Result = OcOverflowAddUN (
             DiskDpSize,
             sizeof (HARDDRIVE_DEVICE_PATH),
             &DiskDpCmpSize
             );
  if (Result) {
    DEBUG ((DEBUG_INFO, "OCPI: HD node would overflow DP\n"));
    return NULL;
  }

  for (Index = 0; Index < NumHandles; ++Index) {
    Handle = Handles[Index];

    HdDevicePath = DevicePathFromHandle (Handle);
    if (HdDevicePath == NULL) {
      continue;
    }

    HdDpSize = GetDevicePathSize (HdDevicePath);
    if (HdDpSize < DiskDpCmpSize) {
      continue;
    }
    //
    // Verify the partition's Device Path has the disk's prefixed.
    //
    CmpResult = CompareMem (
                  HdDevicePath,
                  DiskDevicePath,
                  DiskDpSize - END_DEVICE_PATH_LENGTH
                  );
    if (CmpResult != 0) {
      continue;
    }

    DebugPrintDevicePath (DEBUG_INFO, "OCPI: Discovered HD DP", HdDevicePath);

    PartEntry = OcGetGptPartitionEntry (Handle);
    if (PartEntry == NULL) {
      continue;
    }

    InternalDebugPrintPartitionEntry (
      DEBUG_INFO,
      "OCPI: Discovered PartEntry",
      PartEntry
      );

    if (CompareGuid (&PartEntry->PartitionTypeGUID, &gEfiPartTypeSystemPartGuid)) {
      EspDevicePath = HdDevicePath;
      *EspDevicePathSize = HdDpSize;
      break;
    }
  }

  FreePool (Handles);

  return EspDevicePath;
}

STATIC
CONST INTERNAL_PARTITION_ENTRIES *
InternalGetDiskPartitions (
  IN EFI_HANDLE  DiskHandle
  )
{
  INTERNAL_PARTITION_ENTRIES *PartEntries;

  EFI_STATUS                 Status;
  BOOLEAN                    Result;

  EFI_BLOCK_IO_PROTOCOL      *BlockIo;
  EFI_BLOCK_IO2_PROTOCOL     *BlockIo2;
  EFI_DISK_IO_PROTOCOL       *DiskIo;
  EFI_DISK_IO2_PROTOCOL      *DiskIo2;

  EFI_LBA                    PartEntryLBA;
  UINT32                     NumPartitions;
  UINT32                     PartEntrySize;
  UINTN                      PartEntriesSize;
  UINTN                      PartEntriesStructSize;
  EFI_PARTITION_TABLE_HEADER *GptHeader;

  ASSERT (DiskHandle != NULL);
  //
  // Retrieve the Block I/O protocol.
  //
  Status = gBS->HandleProtocol (
                  DiskHandle,
                  &mInternalDiskPartitionEntriesProtocolGuid,
                  (VOID **)&PartEntries
                  );
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCPI: Located cached partition entries\n"));
    return PartEntries;
  }

  BlockIo2 = NULL;
  BlockIo  = NULL;

  gBS->HandleProtocol (
         DiskHandle,
         &gEfiBlockIo2ProtocolGuid,
         (VOID **)&BlockIo2
         );
  gBS->HandleProtocol (
         DiskHandle,
         &gEfiBlockIoProtocolGuid,
         (VOID **)&BlockIo
         );
  if (BlockIo2 == NULL && BlockIo == NULL) {
    DEBUG ((
      DEBUG_INFO,
      "OCPI: Block I/O protocol is not present\n"
      ));
    return NULL;
  }
  //
  // Retrieve the Disk I/O protocol.
  //
  DiskIo2 = NULL;
  DiskIo  = NULL;

  gBS->HandleProtocol (
         DiskHandle,
         &gEfiDiskIo2ProtocolGuid,
         (VOID **)&DiskIo2
         );

  gBS->HandleProtocol (
         DiskHandle,
         &gEfiDiskIoProtocolGuid,
         (VOID **)&DiskIo
         );

  if (DiskIo2 == NULL && DiskIo == NULL) {
    DEBUG ((
      DEBUG_INFO,
      "OCPI: Disk I/O protocol is not present\n"
      ));
    return NULL;
  }
  //
  // Retrieve the GPT header.
  //
  GptHeader = AllocatePool (sizeof (*GptHeader));
  if (GptHeader == NULL) {
    DEBUG ((DEBUG_INFO, "OCPI: GPT header allocation error\n"));
    return NULL;
  }

  Status = InternalReadDisk (
             DiskIo,
             DiskIo2,
             BlockIo,
             BlockIo2,
             PRIMARY_PART_HEADER_LBA,
             sizeof (*GptHeader),
             GptHeader
             );
  if (EFI_ERROR (Status)) {
    FreePool (GptHeader);
    DEBUG ((DEBUG_INFO, "OCPI: ReadDisk1 %r\n", Status));
    return NULL;
  }

  if (GptHeader->Header.Signature != EFI_PTAB_HEADER_ID) {
    FreePool (GptHeader);
    DEBUG ((DEBUG_INFO, "OCPI: Partition table not supported\n"));
    return NULL;
  }

  PartEntrySize = GptHeader->SizeOfPartitionEntry;
  if (PartEntrySize < sizeof (EFI_PARTITION_ENTRY)) {
    FreePool (GptHeader);
    DEBUG ((DEBUG_INFO, "OCPI: GPT header is malformed\n"));
    return NULL;
  }

  NumPartitions = GptHeader->NumberOfPartitionEntries;
  PartEntryLBA  = GptHeader->PartitionEntryLBA;

  FreePool (GptHeader);

  Result = OcOverflowMulUN (NumPartitions, PartEntrySize, &PartEntriesSize);
  if (Result) {
    DEBUG ((DEBUG_INFO, "OCPI: Partition entries size overflows\n"));
    return NULL;
  }

  Result = OcOverflowAddUN (
             sizeof (PartEntries),
             PartEntriesSize,
             &PartEntriesStructSize
             );
  if (Result) {
    DEBUG ((DEBUG_INFO, "OCPI: Partition entries struct size overflows\n"));
    return NULL;
  }
  //
  // Retrieve the GPT partition entries.
  //
  PartEntries = AllocatePool (PartEntriesStructSize);
  if (PartEntries == NULL) {
    DEBUG ((DEBUG_INFO, "OCPI: Partition entries allocation error\n"));
    return NULL;
  }

  Status = InternalReadDisk (
             DiskIo,
             DiskIo2,
             BlockIo,
             BlockIo2,
             PartEntryLBA,
             PartEntriesSize,
             PartEntries->FirstEntry
             );
  if (EFI_ERROR (Status)) {
    FreePool (PartEntries);
    DEBUG ((DEBUG_INFO, "OCPI: ReadDisk2 %r\n", Status));
    return NULL;
  }

  PartEntries->NumPartitions      = NumPartitions;
  PartEntries->PartitionEntrySize = PartEntrySize;
  //
  // FIXME: This causes the handle to be dangling if the device is detached.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &DiskHandle,
                  &mInternalDiskPartitionEntriesProtocolGuid,
                  PartEntries,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCPI: Failed to cache partition entries\n"));
    FreePool (PartEntries);
    return NULL;
  }

  return PartEntries;
}

/**
  Retrieve the partition's GPT information, if applicable

  @param[in] FsHandle  The device handle of the partition to retrieve info of.

**/
CONST EFI_PARTITION_ENTRY *
OcGetGptPartitionEntry (
  IN EFI_HANDLE  FsHandle
  )
{
  CONST EFI_PARTITION_ENTRY        *PartEntry;
  CONST INTERNAL_PARTITION_ENTRIES *Partitions;

  EFI_STATUS                       Status;
  EFI_DEVICE_PATH_PROTOCOL         *FsDevicePath;
  CONST HARDDRIVE_DEVICE_PATH      *HdNode;
  EFI_HANDLE                       DiskHandle;
  UINTN                            Offset;

  ASSERT (FsHandle != NULL);

  Status = gBS->HandleProtocol (
                  FsHandle,
                  &mInternalPartitionEntryProtocolGuid,
                  (VOID **)&PartEntry
                  );
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCPI: Located cached partition entry\n"));
    return PartEntry;
  }
  //
  // Retrieve the partition Device Path information.
  //
  FsDevicePath = DevicePathFromHandle (FsHandle);
  if (FsDevicePath == NULL) {
    DEBUG ((DEBUG_INFO, "OCPI: Failed to retrieve Device Path\n"));
    return NULL;
  }

  HdNode = (HARDDRIVE_DEVICE_PATH *)(
             FindDevicePathNodeWithType (
               FsDevicePath,
               MEDIA_DEVICE_PATH,
               MEDIA_HARDDRIVE_DP
               )
             );
  if (HdNode == NULL) {
    DEBUG ((DEBUG_INFO, "OCPI: Device Path does not describe a partition\n"));
    return NULL;
  }

  DiskHandle = InternalPartitionGetDiskHandle (
                 FsDevicePath,
                 (UINTN)HdNode - (UINTN)FsDevicePath
                 );
  if (DiskHandle == NULL) {
    DebugPrintDevicePath (
      DEBUG_INFO,
      "OCPI: Could not locate partition's disk",
      FsDevicePath
      );
    return NULL;
  }
  //
  // Get the disk's GPT partition entries.
  //
  Partitions = InternalGetDiskPartitions (DiskHandle);
  if (Partitions == NULL) {
    DEBUG ((DEBUG_INFO, "OCPI: Failed to retrieve disk info\n"));
    return NULL;
  }

  if (HdNode->PartitionNumber > Partitions->NumPartitions) {
    DEBUG ((DEBUG_INFO, "OCPI: Partition is OOB\n"));
    return NULL;
  }

  ASSERT (HdNode->PartitionNumber > 0);
  Offset = ((UINTN)(HdNode->PartitionNumber - 1) * Partitions->PartitionEntrySize);
  PartEntry = (EFI_PARTITION_ENTRY *)((UINTN)Partitions->FirstEntry + Offset);
  //
  // FIXME: This causes the handle to be dangling if the device is detached.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &FsHandle,
                  &mInternalPartitionEntryProtocolGuid,
                  PartEntry,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCPI: Failed to cache partition entry\n"));
    return NULL;
  }

  return PartEntry;
}
