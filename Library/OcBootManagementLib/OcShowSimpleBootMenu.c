/** @file
  Copyright (C) 2019. All rights reserved.

  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include <Guid/AppleVariable.h>

#include <Protocol/SimpleTextOut.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcAppleKeyMapLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcConsoleLib.h>
#include <Library/OcStringLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC
VOID
ShowBannerAt (
  IN UINTN               Col,
  IN UINTN               Row
  )
{
  gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row);
  gST->ConOut->OutputString (gST->ConOut,
    L"  _____   ______  _____   __  ___ _____  _____   _____   _____  "
    );
  gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row + 1);
  gST->ConOut->OutputString (gST->ConOut,
    L" / ___ \\ /   _  )/  __ \\ /  |/  //  ___)/ ___ \\ /  __ \\ /  __ \\ "
    );
  gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row + 2);
  gST->ConOut->OutputString (gST->ConOut,
    L"/ /__/ //  /___//  (___//      //  /__ / /__/ //  /_/ //  (___/ "
    );
  gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row + 3);
  gST->ConOut->OutputString (gST->ConOut,
    L"\\_____//__/     \\_____ /__/|__/ \\_____)\\_____//__/ \\__\\\\_____   "
    );
}

STATIC
VOID
ShowDateAt (
  IN UINTN               Col,
  IN UINTN               Row
  )
{
  EFI_STATUS         Status;
  EFI_TIME           DateTime;
  CHAR16             DateStr[11];
  
  Status = gRT->GetTime (&DateTime, NULL);
  if (!EFI_ERROR (Status)) {
    UnicodeSPrint (DateStr, sizeof (DateStr), L"%02u/%02u/%04u", DateTime.Month, DateTime.Day, DateTime.Year);
    gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row);
    gST->ConOut->OutputString (gST->ConOut, DateStr);
  }
}

STATIC
VOID
ShowOcVersionAt (
  IN CONST CHAR8         *String,
  IN UINTN               Col,
  IN UINTN               Row
  )
{
  CHAR16                 Code[2];
  UINTN                  Length;
  UINTN                  Index;
  
  Code[1] = '\0';
  
  if (String != NULL) {
    Length = AsciiStrLen (String);
    gST->ConOut->SetCursorPosition (gST->ConOut, Col  - (Length + 7), Row);
    gST->ConOut->OutputString (gST->ConOut, L"N-D-K ");
    for (Index = 0; Index < Length; ++Index) {
      Code[0] = String[Index];
      gST->ConOut->OutputString (gST->ConOut, Code);
    }
  }
}

STATIC
VOID
ClearLines (
  IN UINTN        LeftColumn,
  IN UINTN        RightColumn,
  IN UINTN        TopRow,
  IN UINTN        BottomRow
  )
{
  CHAR16          *String;
  UINTN           Index;
  
  String = AllocateZeroPool ((RightColumn - LeftColumn) * sizeof (CHAR16));
  ASSERT (String != NULL);
  
  for (Index = 0; Index < (RightColumn - (LeftColumn + 1)) ; ++Index) {
    String[Index] = 0x20;
  }
  
  for (Index = TopRow; Index <= BottomRow; ++Index) {
    gST->ConOut->SetCursorPosition (gST->ConOut, LeftColumn, Index);
    gST->ConOut->OutputString (gST->ConOut, String);
  }
  
  gST->ConOut->SetCursorPosition (gST->ConOut, LeftColumn, TopRow);
  
  FreePool (String);
}

STATIC
VOID
ShowTimeOutMessage (
  IN UINTN           Timeout,
  IN UINTN           Col,
  IN UINTN           Row
  )
{
  CHAR16             Sec[3];
  
  if (Timeout > 0) {
    UnicodeSPrint (Sec, sizeof (Sec), L"%02u", Timeout); //2
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_DARKGRAY, EFI_BLACK));
    gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row);
    gST->ConOut->OutputString (gST->ConOut, L"The default boot selection will start in "); //41
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK));
    gST->ConOut->OutputString (gST->ConOut, Sec);
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_DARKGRAY, EFI_BLACK));
    gST->ConOut->OutputString (gST->ConOut, L" seconds"); //8
  } else {
    gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row);
    ClearLines (Col, Col + 52, Row, Row);
  }
}

STATIC
VOID
RestoreConsoleMode (
  IN EFI_SIMPLE_TEXT_OUTPUT_MODE     SavedConsoleMode
  )
{
  
  gST->ConOut->SetAttribute (gST->ConOut, SavedConsoleMode.Attribute);
  gST->ConOut->EnableCursor (gST->ConOut, SavedConsoleMode.CursorVisible);
  gST->ConOut->SetCursorPosition (gST->ConOut, 0, 0);
  
}
               

EFI_STATUS
OcShowSimpleBootMenu (
  IN OC_PICKER_CONTEXT            *Context,
  IN OC_BOOT_ENTRY                *BootEntries,
  IN UINTN                        Count,
  IN UINTN                        DefaultEntry,
  OUT OC_BOOT_ENTRY               **ChosenBootEntry
  )
{
  EFI_STATUS                      Status;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
  EFI_SIMPLE_TEXT_OUTPUT_MODE     SavedConsoleMode;
  UINTN                           Index;
  INTN                            KeyIndex;
  CHAR16                          Code[2];
  UINT32                          TimeOutSeconds;
  UINTN                           Columns;
  UINTN                           Rows;
  UINTN                           VisibleList[Count];
  UINTN                           VisibleIndex;
  BOOLEAN                         ShowAll;
  UINTN                           Selected;
  UINTN                           BannerCol;
  UINTN                           BannerRow;
  UINTN                           ItemCol;
  UINTN                           ItemRow;
  UINTN                           MaxStrWidth;
  UINTN                           StrWidth;
  APPLE_KEY_CODE                  LastPolled;
  
  Code[1]        = '\0';
  LastPolled     = 0;
  VisibleIndex   = 0;
  ShowAll        = FALSE;
  MaxStrWidth    = 0;
  TimeOutSeconds = Context->TimeoutSeconds;
  
  ConOut = gST->ConOut;
  CopyMem (&SavedConsoleMode, ConOut->Mode, sizeof (SavedConsoleMode));
  
  for (Index = 0; Index < MIN (Count, OC_INPUT_MAX); ++Index) {
    StrWidth = UnicodeStringDisplayLength (BootEntries[Index].Name);
    MaxStrWidth = MaxStrWidth > StrWidth ? MaxStrWidth : StrWidth;
  }
  
  ConOut->QueryMode (
            ConOut,
            SavedConsoleMode.Mode,
            &Columns,
            &Rows
            );
  
  // This is necessary to correct console mode after returning from running any kind of tool.
  Status = SetConsoleMode ((UINT32) Columns, (UINT32) Rows);
  DEBUG ((DEBUG_INFO, "OCSSBM: Resetting console mode to %ux%u - %r\n", Columns, Rows, Status));
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  
  MaxStrWidth = MaxStrWidth + 12;
  BannerCol = (Columns - 64) / 2;
  BannerRow = (Rows - (Count + 16)) / 2;
  ItemCol = (Columns - MaxStrWidth) / 2;
  ItemRow = (Rows - (Count + 2)) / 2;
  
  ConOut->ClearScreen (ConOut);
  ConOut->SetAttribute (ConOut, EFI_TEXT_ATTR (EFI_DARKGRAY, EFI_BLACK));
  ShowBannerAt (BannerCol, BannerRow);
  ShowDateAt (1, Rows - 1);
  ShowOcVersionAt (Context->TitleSuffix, Columns, Rows - 1);
  
  while (TRUE) {
    ShowTimeOutMessage (TimeOutSeconds, (Columns - 52) / 2, ItemRow + Count + 2);
    ClearLines (ItemCol, ItemCol + MaxStrWidth, ItemRow, ItemRow + VisibleIndex);
    VisibleIndex = 0;
    for (Index = 0; Index < MIN (Count, OC_INPUT_MAX); ++Index) {
      if ((BootEntries[Index].Hidden && !ShowAll)
          || (BootEntries[Index].Type == OcBootSystem && !ShowAll)) {
        continue;
      }
      if (DefaultEntry == Index) {
        Selected = VisibleIndex;
        ConOut->SetAttribute (ConOut, EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK));
      } else {
        ConOut->SetAttribute (ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
      }
      ConOut->SetCursorPosition (ConOut, ItemCol, ItemRow + VisibleIndex);
      Code[0] = OC_INPUT_STR[VisibleIndex];
      VisibleList[VisibleIndex] = Index;
      ConOut->OutputString (ConOut, DefaultEntry == Index ? L"* " : L"  ");
      ConOut->OutputString (ConOut, Code);
      ConOut->OutputString (ConOut, L". ");
      ConOut->OutputString (ConOut, BootEntries[Index].Name);
      if (BootEntries[Index].IsExternal) {
        ConOut->OutputString (ConOut, L" (ext)");
      }
      if (BootEntries[Index].IsFolder) {
        ConOut->OutputString (ConOut, L" (dmg)");
      }
      ++VisibleIndex;
    }

    while (TRUE) {
      KeyIndex = OcWaitForAppleKeyIndex (Context, TimeOutSeconds > 0 ? 1 : 0, &LastPolled, Context->PollAppleHotKeys);
      --TimeOutSeconds;
      if ((KeyIndex == OC_INPUT_TIMEOUT && TimeOutSeconds == 0) || KeyIndex == OC_INPUT_RETURN) {
        *ChosenBootEntry = &BootEntries[DefaultEntry];
        RestoreConsoleMode (SavedConsoleMode);
        return EFI_SUCCESS;
      } else if (KeyIndex == OC_INPUT_ABORTED) {
        TimeOutSeconds = 0;
        break;
      } else if (KeyIndex == OC_INPUT_SPACEBAR) {
        ShowAll = !ShowAll;
        while ((BootEntries[DefaultEntry].Hidden && !ShowAll && DefaultEntry > 0)
               || (BootEntries[DefaultEntry].Type == OcBootSystem && !ShowAll && DefaultEntry > 0)) {
          --DefaultEntry;
        }
        TimeOutSeconds = 0;
        break;
      } else if (KeyIndex == OC_INPUT_UP) {
        DefaultEntry = Selected > 0 ? VisibleList[Selected - 1] : VisibleList[VisibleIndex - 1];
        TimeOutSeconds = 0;
        break;
      } else if (KeyIndex == OC_INPUT_DOWN) {
        DefaultEntry = Selected < (VisibleIndex - 1) ? VisibleList[Selected + 1] : 0;
        TimeOutSeconds = 0;
        break;
      } else if (KeyIndex != OC_INPUT_INVALID && (UINTN)KeyIndex < VisibleIndex) {
        ASSERT (KeyIndex >= 0);
        *ChosenBootEntry = &BootEntries[VisibleList[KeyIndex]];
        RestoreConsoleMode (SavedConsoleMode);
        return EFI_SUCCESS;
      } else if (KeyIndex != OC_INPUT_TIMEOUT) {
        TimeOutSeconds = 0;
      }
      
      if (TimeOutSeconds == 0) {
        break;
      }
      
      ShowTimeOutMessage (TimeOutSeconds, (Columns - 52) / 2, ItemRow + Count + 2);
    }
  }

  ASSERT (FALSE);
}
