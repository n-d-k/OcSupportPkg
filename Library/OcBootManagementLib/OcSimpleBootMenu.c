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
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>

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
  
  String = AllocateZeroPool ((RightColumn - (LeftColumn - 1)) * sizeof (CHAR16));
  ASSERT (String != NULL);
  
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
  
  for (Index = 0; Index < (RightColumn - LeftColumn) ; ++Index) {
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
DrawFrame (
  IN UINTN               Columns,
  IN UINTN               Rows
  )
{
  CHAR16                 Char[2];
  UINTN                  Index;
  UINTN                  Index1;
  
  Char[1] = '\0';
  
  Char[0] = BOXDRAW_DOUBLE_DOWN_RIGHT;
  gST->ConOut->SetCursorPosition (gST->ConOut, 1, 0);
  gST->ConOut->OutputString (gST->ConOut, Char);
  Char[0] = BOXDRAW_DOUBLE_HORIZONTAL;
  for (Index = 2; Index < Columns - 2; ++Index) {
    gST->ConOut->OutputString (gST->ConOut, Char);
  }
  Char[0] = BOXDRAW_DOUBLE_DOWN_LEFT;
  gST->ConOut->OutputString (gST->ConOut, Char);
  
  Char[0] = BOXDRAW_DOUBLE_VERTICAL;
  for (Index = 1; Index < Rows - 1; ++Index) {
    // draw middle double line
    if (Index == (Rows - 3) || Index == 3) {
      Char[0] = BOXDRAW_DOUBLE_VERTICAL_RIGHT;
      gST->ConOut->SetCursorPosition (gST->ConOut, 1, Index);
      gST->ConOut->OutputString (gST->ConOut, Char);
      Char[0] = BOXDRAW_DOUBLE_HORIZONTAL;
      for (Index1 = 2; Index1 < Columns - 2; ++Index1) {
        gST->ConOut->OutputString (gST->ConOut, Char);
      }
      Char[0] = BOXDRAW_DOUBLE_VERTICAL_LEFT;
      gST->ConOut->OutputString (gST->ConOut, Char);
      Char[0] = BOXDRAW_DOUBLE_VERTICAL;
      continue;
    }
    
    gST->ConOut->SetCursorPosition (gST->ConOut, 1, Index);
    gST->ConOut->OutputString (gST->ConOut, Char);
    gST->ConOut->SetCursorPosition (gST->ConOut, Columns - 2, Index);
    gST->ConOut->OutputString (gST->ConOut, Char);
  }
  
  Char[0] = BOXDRAW_DOUBLE_UP_RIGHT;
  gST->ConOut->SetCursorPosition (gST->ConOut, 1, Rows - 1);
  gST->ConOut->OutputString (gST->ConOut, Char);
  Char[0] = BOXDRAW_DOUBLE_HORIZONTAL;
  for (Index = 2; Index < Columns - 2; ++Index) {
    gST->ConOut->OutputString (gST->ConOut, Char);
  }
  Char[0] = BOXDRAW_DOUBLE_UP_LEFT;
  gST->ConOut->OutputString (gST->ConOut, Char);
}

STATIC
VOID
ShowBannerAt (
  IN UINTN               Col,
  IN UINTN               Row
  )
{
  gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row);
  gST->ConOut->OutputString (gST->ConOut,
    L"   _____   ______  _____   __  ___ _____  _____   _____   _____  "
    );
  gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row + 1);
  gST->ConOut->OutputString (gST->ConOut,
    L"  / ___ \\ /   _  )/  __ \\ /  |/  //  ___)/ ___ \\ /  __ \\ /  __ \\ "
    );
  gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row + 2);
  gST->ConOut->OutputString (gST->ConOut,
    L" / /__/ //  /___//  (___//      //  /__ / /__/ //  /_/ //  (___/ "
    );
  gST->ConOut->SetCursorPosition (gST->ConOut, Col, Row + 3);
  gST->ConOut->OutputString (gST->ConOut,
    L" \\_____//__/     \\_____ /__/|__/ \\_____)\\_____//__/ \\__\\\\_____   "
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
  CHAR16             DateStr[24];
  UINTN              Hour;
  CHAR16             *Str;
  
  Str = NULL;
  Hour = 0;
  Status = gRT->GetTime (&DateTime, NULL);
  
  if (!EFI_ERROR (Status)) {
    Hour = (UINTN) DateTime.Hour;
    Str = Hour >= 12 ? L"PM" : L"AM";
    if (Hour > 12) {
      Hour = Hour - 12;
    }
    UnicodeSPrint (DateStr, sizeof (DateStr), L"%02u/%02u/%04u %02u:%02u:%02u %s",
                   DateTime.Month, DateTime.Day, DateTime.Year, Hour, DateTime.Minute, DateTime.Second, Str);
    ClearLines (Col, Col + sizeof (DateStr), Row, Row);
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_DARKGRAY, EFI_BLACK));
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
    gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_DARKGRAY, EFI_BLACK));
    gST->ConOut->SetCursorPosition (gST->ConOut, Col  - (Length + 9), Row);
    gST->ConOut->OutputString (gST->ConOut, L"N-D-K ");
    for (Index = 0; Index < Length; ++Index) {
      Code[0] = String[Index];
      gST->ConOut->OutputString (gST->ConOut, Code);
    }
  }
}

STATIC
BOOLEAN
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
  return !(Timeout > 0);
}

STATIC
VOID
PrintEntry (
  IN UINTN        LeftColumn,
  IN UINTN        RightColumn,
  IN UINTN        Row,
  IN UINTN        Selected,
  IN CHAR16       *Name,
  IN BOOLEAN      Ext,
  IN BOOLEAN      Dmg,
  IN BOOLEAN      Highlighted
  )
{
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL    *ConOut;
  CHAR16                             Code[3];
  
  ConOut  = gST->ConOut;
  Code[0] = 0x20;
  Code[1] = OC_INPUT_STR[Selected];
  Code[2] = '\0';
  
  ClearLines (LeftColumn, RightColumn, Row, Row);
  ConOut->SetCursorPosition (ConOut, LeftColumn, Row);
  ConOut->SetAttribute (ConOut, EFI_TEXT_ATTR (Highlighted ? EFI_BLACK : EFI_LIGHTGRAY, Highlighted ? EFI_LIGHTGRAY : EFI_BLACK));
  ConOut->OutputString (ConOut, Code);
  ConOut->OutputString (ConOut, L". ");
  ConOut->OutputString (ConOut, Name);
  if (Ext) {
    ConOut->OutputString (ConOut, L" (ext)");
  }
  if (Dmg) {
    ConOut->OutputString (ConOut, L" (dmg)");
  }
  while (ConOut->Mode->CursorColumn < RightColumn) {
    ConOut->OutputString (ConOut, L" ");
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
  EFI_STATUS                         Status;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL    *ConOut;
  EFI_SIMPLE_TEXT_OUTPUT_MODE        SavedConsoleMode;
  UINTN                              Index;
  INTN                               KeyIndex;
  UINT32                             TimeOutSeconds;
  UINTN                              Columns;
  UINTN                              Rows;
  UINTN                              VisibleList[Count];
  UINTN                              VisibleIndex;
  BOOLEAN                            ShowAll;
  UINTN                              Selected;
  UINTN                              BannerCol;
  UINTN                              BannerRow;
  UINTN                              ItemCol;
  UINTN                              ItemRow;
  UINTN                              MaxStrWidth;
  UINTN                              StrWidth;
  APPLE_KEY_MAP_AGGREGATOR_PROTOCOL  *KeyMap;
  BOOLEAN                            SetDefault;
  BOOLEAN                            TimeoutExpired;
  
  VisibleIndex   = 0;
  ShowAll        = FALSE;
  MaxStrWidth    = 0;
  TimeOutSeconds = Context->TimeoutSeconds;
  TimeoutExpired = FALSE;
  
  KeyMap = OcAppleKeyMapInstallProtocols (FALSE);
  if (KeyMap == NULL) {
    DEBUG ((DEBUG_ERROR, "OCB: Missing AppleKeyMapAggregator\n"));
    return EFI_UNSUPPORTED;
  }
  
  ConOut = gST->ConOut;
  CopyMem (&SavedConsoleMode, ConOut->Mode, sizeof (SavedConsoleMode));
  
  for (Index = 0; Index < MIN (Count, OC_INPUT_MAX); ++Index) {
    StrWidth = UnicodeStringDisplayLength (BootEntries[Index].Name) + ((BootEntries[Index].IsFolder || BootEntries[Index].IsExternal) ? 11 : 5);
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
  DEBUG ((DEBUG_INFO, "OCSBM: Resetting console mode to %ux%u - %r\n", Columns, Rows, Status));
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  
  BannerCol = (Columns - 65) / 2;
  BannerRow = (Rows - (Count + 16)) / 2;
  ItemCol = (Columns - MaxStrWidth) / 2;
  ItemRow = (Rows - (Count + 2)) / 2;
  
  ConOut->ClearScreen (ConOut);
  ConOut->SetAttribute (ConOut, EFI_TEXT_ATTR (EFI_DARKGRAY, EFI_BLACK));
  DrawFrame (Columns, Rows);
  ShowBannerAt (BannerCol, 1);
  ShowDateAt (3, Rows - 2);
  ShowOcVersionAt (Context->TitleSuffix, Columns, Rows - 2);
  
  while (TRUE) {
    if (!TimeoutExpired) {
      TimeoutExpired = ShowTimeOutMessage (TimeOutSeconds, (Columns - 52) / 2, ItemRow + Count + 2);
      TimeOutSeconds = TimeoutExpired ? 10000 : TimeOutSeconds;
    }
    ClearLines (ItemCol, ItemCol + MaxStrWidth, ItemRow, ItemRow + VisibleIndex);
    for (Index = 0, VisibleIndex = 0; Index < MIN (Count, OC_INPUT_MAX); ++Index) {
      if ((BootEntries[Index].Hidden && !ShowAll)
          || (BootEntries[Index].Type == OcBootSystem && !ShowAll)) {
        continue;
      }
      if (DefaultEntry == Index) {
        Selected = VisibleIndex;
      }
      VisibleList[VisibleIndex] = Index;
      PrintEntry (ItemCol, ItemCol + MaxStrWidth, ItemRow + VisibleIndex, VisibleIndex,
                  BootEntries[Index].Name,
                  BootEntries[Index].IsExternal,
                  BootEntries[Index].IsFolder,
                  DefaultEntry == Index
                  );
      ++VisibleIndex;
    }

    while (TRUE) {
      KeyIndex = OcWaitForAppleKeyIndex (Context, KeyMap, 1, Context->PollAppleHotKeys, &SetDefault);
      --TimeOutSeconds;
      if ((KeyIndex == OC_INPUT_TIMEOUT && TimeOutSeconds == 0) || KeyIndex == OC_INPUT_RETURN) {
        *ChosenBootEntry = &BootEntries[DefaultEntry];
        SetDefault = BootEntries[DefaultEntry].DevicePath != NULL
          && !BootEntries[DefaultEntry].Hidden
          && Context->AllowSetDefault
          && SetDefault;
        if (SetDefault) {
          OcSetDefaultBootEntry (BootEntries[DefaultEntry].DevicePath);
        }
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
        PrintEntry (ItemCol, ItemCol + MaxStrWidth, ItemRow + Selected, Selected,
                    BootEntries[DefaultEntry].Name,
                    BootEntries[DefaultEntry].IsExternal,
                    BootEntries[DefaultEntry].IsFolder,
                    FALSE
                    );
        DefaultEntry = Selected > 0 ? VisibleList[Selected - 1] : VisibleList[VisibleIndex - 1];
        Selected = DefaultEntry;
        PrintEntry (ItemCol, ItemCol + MaxStrWidth, ItemRow + Selected, Selected,
                    BootEntries[DefaultEntry].Name,
                    BootEntries[DefaultEntry].IsExternal,
                    BootEntries[DefaultEntry].IsFolder,
                    TRUE
                    );
        TimeOutSeconds = 0;
      } else if (KeyIndex == OC_INPUT_DOWN) {
        PrintEntry (ItemCol, ItemCol + MaxStrWidth, ItemRow + Selected, Selected,
                    BootEntries[DefaultEntry].Name,
                    BootEntries[DefaultEntry].IsExternal,
                    BootEntries[DefaultEntry].IsFolder,
                    FALSE
                    );
        DefaultEntry = Selected < (VisibleIndex - 1) ? VisibleList[Selected + 1] : 0;
        Selected = DefaultEntry;
        PrintEntry (ItemCol, ItemCol + MaxStrWidth, ItemRow + Selected, Selected,
                    BootEntries[DefaultEntry].Name,
                    BootEntries[DefaultEntry].IsExternal,
                    BootEntries[DefaultEntry].IsFolder,
                    TRUE
                    );
        TimeOutSeconds = 0;
      } else if (KeyIndex != OC_INPUT_INVALID && (UINTN)KeyIndex < VisibleIndex) {
        ASSERT (KeyIndex >= 0);
        *ChosenBootEntry = &BootEntries[VisibleList[KeyIndex]];
        SetDefault = BootEntries[VisibleList[KeyIndex]].DevicePath != NULL
          && !BootEntries[VisibleList[KeyIndex]].Hidden
          && Context->AllowSetDefault
          && SetDefault;
        if (SetDefault) {
          OcSetDefaultBootEntry (BootEntries[VisibleList[KeyIndex]].DevicePath);
        }
        RestoreConsoleMode (SavedConsoleMode);
        return EFI_SUCCESS;
      } else if (KeyIndex != OC_INPUT_TIMEOUT) {
        TimeOutSeconds = 0;
      }
      
      if (!TimeoutExpired) {
        ShowDateAt (3, Rows - 2);
        TimeoutExpired = ShowTimeOutMessage (TimeOutSeconds, (Columns - 52) / 2, ItemRow + Count + 2);
        TimeOutSeconds = TimeoutExpired ? 10000 : TimeOutSeconds;
      } else {
        ShowDateAt (3, Rows - 2);
      }
    }
  }

  ASSERT (FALSE);
}
