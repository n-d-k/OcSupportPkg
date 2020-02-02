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

#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextOut.h>
#include <Protocol/UgaDraw.h>
#include <Protocol/HiiFont.h>


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
#include <Library/OcPngLib.h>
#include <Library/OcFileLib.h>

STATIC
BOOLEAN
mAllowSetDefault;

STATIC
UINTN
mDefaultEntry;

/*========== Graphic UI Begin ==========*/

STATIC
EFI_GRAPHICS_OUTPUT_PROTOCOL *
mGraphicsOutput;

STATIC
EFI_HII_FONT_PROTOCOL *
mHiiFont;

STATIC
EFI_UGA_DRAW_PROTOCOL *
mUgaDraw;

STATIC
INTN
mScreenWidth;

STATIC
INTN
mScreenHeight;

STATIC
INTN
mFontWidth;

STATIC
INTN
mFontHeight;

STATIC
INTN
mTextHeight;

STATIC
INTN
mUiScale = 0;  // not actual scale, will be set after getting screen resolution. (16 will be no scaling, 28 will be for 4k screen)

STATIC
UINTN
mIconSpaceSize = 136;  // Default 136 pixels space to contain icons with size 128x128, 272 for 256x256 icons size (best for 4k screen).

STATIC
EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *
mFileSystem = NULL;

EFI_IMAGE_OUTPUT *mBackgroundImage = NULL;
EFI_IMAGE_OUTPUT *mMenuImage = NULL;


/*============ User's Color Settings Begin ==============*/

EFI_GRAPHICS_OUTPUT_BLT_PIXEL mTan = {0x28, 0x3d, 0x52, 0xff};
EFI_GRAPHICS_OUTPUT_BLT_PIXEL mWhitePixel  = {0xff, 0xff, 0xff, 0xff};
EFI_GRAPHICS_OUTPUT_BLT_PIXEL mBlackPixel  = {0x00, 0x00, 0x00, 0xff};
EFI_GRAPHICS_OUTPUT_BLT_PIXEL mDarkGray = {0x76, 0x81, 0x85, 0xff};
EFI_GRAPHICS_OUTPUT_BLT_PIXEL mLowWhitePixel  = {0xb8, 0xbd, 0xbf, 0xff};
/*
 Example:
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL mNewColor = {0x3d, 0x3c, 0x3b, 0xff}; <- BGRA format
 
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL *mFontColorPixel = &mNewColor;
*/
// Selection and Entry's description font color
EFI_GRAPHICS_OUTPUT_BLT_PIXEL *mFontColorPixel = &mLowWhitePixel;

// Date time, Version, and other color
EFI_GRAPHICS_OUTPUT_BLT_PIXEL *mFontColorPixelAlt = &mDarkGray;

// Background color
EFI_GRAPHICS_OUTPUT_BLT_PIXEL *mBackgroundPixel = &mBlackPixel;

/*============= User's Color Settings End ===============*/

STATIC
VOID
FreeImage (
  IN EFI_IMAGE_OUTPUT    *Image
  )
{
  if (Image != NULL) {
    if (Image->Image.Bitmap != NULL) {
      FreePool (Image->Image.Bitmap);
      Image->Image.Bitmap = NULL;
    }
    FreePool (Image);
  }
}

STATIC
EFI_IMAGE_OUTPUT *
CreateImage (
  IN UINT16       Width,
  IN UINT16       Height
  )
{
  EFI_IMAGE_OUTPUT  *NewImage;
  
  NewImage = (EFI_IMAGE_OUTPUT *) AllocatePool (sizeof (EFI_IMAGE_OUTPUT));
  
  if (NewImage == NULL) {
    return NULL;
  }
  
  if (Width * Height == 0) {
    FreeImage (NewImage);
    return NULL;
  }
  
  NewImage->Image.Bitmap = AllocatePool (Width * Height * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  if (NewImage->Image.Bitmap == NULL) {
    FreePool (NewImage);
    return NULL;
  }
  
  NewImage->Width = Width;
  NewImage->Height = Height;
  
  return NewImage;
}

STATIC
VOID
RestrictImageArea (
  IN     EFI_IMAGE_OUTPUT   *Image,
  IN     INTN               AreaXpos,
  IN     INTN               AreaYpos,
  IN OUT INTN               *AreaWidth,
  IN OUT INTN               *AreaHeight
  )
{
  if (Image == NULL || AreaWidth == NULL || AreaHeight == NULL) {
    DEBUG ((DEBUG_INFO, "OCUI: invalid argument\n"));
    return;
  }

  if (AreaXpos >= Image->Width || AreaYpos >= Image->Height) {
    *AreaWidth  = 0;
    *AreaHeight = 0;
  } else {
    if (*AreaWidth > Image->Width - AreaXpos) {
      *AreaWidth = Image->Width - AreaXpos;
    }
    if (*AreaHeight > Image->Height - AreaYpos) {
      *AreaHeight = Image->Height - AreaYpos;
    }
  }
}

STATIC
VOID
DrawImageArea (
  IN EFI_IMAGE_OUTPUT  *Image,
  IN INTN              AreaXpos,
  IN INTN              AreaYpos,
  IN INTN              AreaWidth,
  IN INTN              AreaHeight,
  IN INTN              ScreenXpos,
  IN INTN              ScreenYpos
  )
{
  EFI_STATUS           Status;
  
  if (Image == NULL) {
    return;
  }
  if (ScreenXpos < 0 || ScreenXpos >= mScreenWidth || ScreenYpos < 0 || ScreenYpos >= mScreenHeight) {
    DEBUG ((DEBUG_INFO, "OCUI: Invalid Screen coordinate requested...x:%d - y:%d \n", ScreenXpos, ScreenYpos));
    return;
  }
  
  if (AreaWidth == 0) {
    AreaWidth = Image->Width;
  }
  
  if (AreaHeight == 0) {
    AreaHeight = Image->Height;
  }
  
  if ((AreaXpos != 0) || (AreaYpos != 0)) {
    RestrictImageArea (Image, AreaXpos, AreaYpos, &AreaWidth, &AreaHeight);
    if (AreaWidth == 0) {
      DEBUG ((DEBUG_INFO, "OCUI: invalid area position requested\n"));
      return;
    }
  }
  
  if (ScreenXpos + AreaWidth > mScreenWidth) {
    AreaWidth = mScreenWidth - ScreenXpos;
  }
  
  if (ScreenYpos + AreaHeight > mScreenHeight) {
    AreaHeight = mScreenHeight - ScreenYpos;
  }
  
  if (mGraphicsOutput != NULL) {
    Status = mGraphicsOutput->Blt(mGraphicsOutput,
                                  Image->Image.Bitmap,
                                  EfiBltBufferToVideo,
                                  (UINTN) AreaXpos,
                                  (UINTN) AreaYpos,
                                  (UINTN) ScreenXpos,
                                  (UINTN) ScreenYpos,
                                  (UINTN) AreaWidth,
                                  (UINTN) AreaHeight,
                                  (UINTN) Image->Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
                                  );
  } else {
    ASSERT (mUgaDraw != NULL);
    Status = mUgaDraw->Blt(mUgaDraw,
                            (EFI_UGA_PIXEL *) Image->Image.Bitmap,
                            EfiUgaBltBufferToVideo,
                            (UINTN) AreaXpos,
                            (UINTN) AreaYpos,
                            (UINTN) ScreenXpos,
                            (UINTN) ScreenYpos,
                            (UINTN) AreaWidth,
                            (UINTN) AreaHeight,
                            (UINTN) Image->Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
                            );
  }
  
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCUI: Draw Image Area...%r\n", Status));
  }
}

STATIC
VOID
RawComposeOnFlat (
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL *CompBasePtr,
  IN     EFI_GRAPHICS_OUTPUT_BLT_PIXEL *TopBasePtr,
  IN     INTN                          Width,
  IN     INTN                          Height,
  IN     INTN                          CompLineOffset,
  IN     INTN                          TopLineOffset
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL        *TopPtr;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL        *CompPtr;
  UINT32                               TopAlpha;
  UINT32                               RevAlpha;
  UINTN                                Temp;
  INT64                                X;
  INT64                                Y;

  if (CompBasePtr == NULL || TopBasePtr == NULL) {
    return;
  }

  for (Y = 0; Y < Height; ++Y) {
    TopPtr = TopBasePtr;
    CompPtr = CompBasePtr;
    for (X = 0; X < Width; ++X) {
      TopAlpha = TopPtr->Reserved;
      RevAlpha = 255 - TopAlpha;

      Temp = ((UINT8) CompPtr->Blue * RevAlpha) + ((UINT8) TopPtr->Blue * TopAlpha);
      CompPtr->Blue = (UINT8) (Temp / 255);

      Temp = ((UINT8) CompPtr->Green * RevAlpha) + ((UINT8) TopPtr->Green * TopAlpha);
      CompPtr->Green = (UINT8) (Temp / 255);

      Temp = ((UINT8) CompPtr->Red * RevAlpha) + ((UINT8) TopPtr->Red * TopAlpha);
      CompPtr->Red = (UINT8) (Temp / 255);

      CompPtr->Reserved = (UINT8)(255);

      TopPtr++;
      CompPtr++;
    }
    TopBasePtr += TopLineOffset;
    CompBasePtr += CompLineOffset;
  }
}

STATIC
VOID
RawCopy (
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL *CompBasePtr,
  IN     EFI_GRAPHICS_OUTPUT_BLT_PIXEL *TopBasePtr,
  IN     INTN                          Width,
  IN     INTN                          Height,
  IN     INTN                          CompLineOffset,
  IN     INTN                          TopLineOffset
  )
{
  INTN       X;
  INTN       Y;

  if (CompBasePtr == NULL || TopBasePtr == NULL) {
    return;
  }

  for (Y = 0; Y < Height; ++Y) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *TopPtr = TopBasePtr;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *CompPtr = CompBasePtr;
    for (X = 0; X < Width; ++X) {
      *CompPtr = *TopPtr;
      TopPtr++;
      CompPtr++;
    }
    TopBasePtr += TopLineOffset;
    CompBasePtr += CompLineOffset;
  }
}

STATIC
VOID
RawCompose (
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL *CompBasePtr,
  IN     EFI_GRAPHICS_OUTPUT_BLT_PIXEL *TopBasePtr,
  IN     INTN                          Width,
  IN     INTN                          Height,
  IN     INTN                          CompLineOffset,
  IN     INTN                          TopLineOffset
  )
{
  INT64                                X;
  INT64                                Y;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL        *TopPtr;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL        *CompPtr;
  INTN                                 TopAlpha;
  INTN                                 Alpha;
  INTN                                 CompAlpha;
  INTN                                 RevAlpha;
  INTN                                 TempAlpha;

  if (CompBasePtr == NULL || TopBasePtr == NULL) {
    return;
  }

  for (Y = 0; Y < Height; ++Y) {
    TopPtr = TopBasePtr;
    CompPtr = CompBasePtr;
    for (X = 0; X < Width; ++X) {
      TopAlpha = TopPtr->Reserved & 0xFF;
      
      if (TopAlpha == 255) {
        CompPtr->Blue  = TopPtr->Blue;
        CompPtr->Green = TopPtr->Green;
        CompPtr->Red   = TopPtr->Red;
        CompPtr->Reserved = (UINT8) TopAlpha;
      } else if (TopAlpha != 0) {
        CompAlpha = CompPtr->Reserved & 0xFF;
        RevAlpha = 255 - TopAlpha;
        TempAlpha = CompAlpha * RevAlpha;
        TopAlpha *= 255;
        Alpha = TopAlpha + TempAlpha;

        CompPtr->Blue = (UINT8) ((TopPtr->Blue * TopAlpha + CompPtr->Blue * TempAlpha) / Alpha);
        CompPtr->Green = (UINT8) ((TopPtr->Green * TopAlpha + CompPtr->Green * TempAlpha) / Alpha);
        CompPtr->Red = (UINT8) ((TopPtr->Red * TopAlpha + CompPtr->Red * TempAlpha) / Alpha);
        CompPtr->Reserved = (UINT8) (Alpha / 255);
      }
      TopPtr++;
      CompPtr++;
    }
    TopBasePtr += TopLineOffset;
    CompBasePtr += CompLineOffset;
  }
}

STATIC
VOID
ComposeImage (
  IN OUT EFI_IMAGE_OUTPUT    *Image,
  IN     EFI_IMAGE_OUTPUT    *TopImage,
  IN     INTN                Xpos,
  IN     INTN                Ypos,
  IN     BOOLEAN             ImageIsAlpha,
  IN     BOOLEAN             TopImageIsAlpha
  )
{
  INTN                       CompWidth;
  INTN                       CompHeight;
  
  if (TopImage == NULL || Image == NULL) {
    return;
  }

  CompWidth  = TopImage->Width;
  CompHeight = TopImage->Height;
  RestrictImageArea (Image, Xpos, Ypos, &CompWidth, &CompHeight);

  if (CompWidth > 0) {
    if (TopImageIsAlpha) {
      if (ImageIsAlpha) {
        RawCompose (Image->Image.Bitmap + Ypos * Image->Width + Xpos,
                    TopImage->Image.Bitmap,
                    CompWidth,
                    CompHeight,
                    Image->Width,
                    TopImage->Width
                    );
      } else {
        RawComposeOnFlat (Image->Image.Bitmap + Ypos * Image->Width + Xpos,
                          TopImage->Image.Bitmap,
                          CompWidth,
                          CompHeight,
                          Image->Width,
                          TopImage->Width
                          );
      }
    } else {
      RawCopy (Image->Image.Bitmap + Ypos * Image->Width + Xpos,
               TopImage->Image.Bitmap,
               CompWidth,
               CompHeight,
               Image->Width,
               TopImage->Width
               );
    }
  }
}

STATIC
VOID
FillImage (
  IN OUT EFI_IMAGE_OUTPUT              *Image,
  IN     EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color,
  IN     BOOLEAN                       IsAlpha
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL        FillColor;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL        *PixelPtr;
  INTN                                 Index;
  
  if (Image == NULL || Color == NULL) {
    return;
  }
  

  if (IsAlpha) {
    FillColor.Reserved = 0;
  }

  FillColor = *Color;

  PixelPtr = Image->Image.Bitmap;
  for (Index = 0; Index < Image->Width * Image->Height; ++Index) {
    *PixelPtr++ = FillColor;
  }
}

EFI_IMAGE_OUTPUT *
CreateFilledImage (
  IN INTN                          Width,
  IN INTN                          Height,
  IN BOOLEAN                       IsAlpha,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color
  )
{
  EFI_IMAGE_OUTPUT      *NewImage;
  
  NewImage = CreateImage (Width, Height);
  if (NewImage == NULL) {
    return NULL;
  }
  
  FillImage (NewImage, Color, IsAlpha);
  
  return NewImage;
}

STATIC
VOID
OcGetGlyph (
  VOID
  )
{
  EFI_STATUS            Status;
  EFI_IMAGE_OUTPUT      *Blt;
  
  Blt = NULL;
  
  Status = mHiiFont->GetGlyph (mHiiFont,
                               L'Z',
                               NULL,
                               &Blt,
                               NULL
                               );
  if (!EFI_ERROR (Status)) {
    mFontWidth = Blt->Width;
    mFontHeight = Blt->Height;
    mTextHeight = mFontHeight + 1;
    DEBUG ((DEBUG_INFO, "OCUI: Got system fontsize - w:%dxh:%d\n", Blt->Width, Blt->Height));
    FreeImage (Blt);
  }
}
STATIC
EFI_IMAGE_OUTPUT *
CopyImage (
  IN EFI_IMAGE_OUTPUT   *Image
  )
{
  EFI_IMAGE_OUTPUT      *NewImage;
  if (Image == NULL || (Image->Width * Image->Height) == 0) {
    return NULL;
  }

  NewImage = CreateImage (Image->Width, Image->Height);
  if (NewImage == NULL) {
    return NULL;
  }
  
  CopyMem (NewImage->Image.Bitmap, Image->Image.Bitmap, (UINTN) (Image->Width * Image->Height * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)));
  return NewImage;
}

STATIC
EFI_IMAGE_OUTPUT *
CopyScaledImage (
  IN EFI_IMAGE_OUTPUT  *OldImage,
  IN INTN              Ratio
  )
{
  BOOLEAN                             Grey = FALSE;
  EFI_IMAGE_OUTPUT                    *NewImage;
  INTN                                x, x0, x1, x2, y, y0, y1, y2;
  INTN                                NewH, NewW;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL       *Dest;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL       *Src;
  INTN                                OldW;

  if (Ratio < 0) {
    Ratio = -Ratio;
    Grey = TRUE;
  }

  if (OldImage == NULL) {
    return NULL;
  }
  Src =  OldImage->Image.Bitmap;
  OldW = OldImage->Width;

  NewW = (OldImage->Width * Ratio) >> 4;
  NewH = (OldImage->Height * Ratio) >> 4;


  if (Ratio == 16) {
    NewImage = CopyImage (OldImage);
  } else {
    NewImage = CreateImage (NewW, NewH);
    if (NewImage == NULL)
      return NULL;

    Dest = NewImage->Image.Bitmap;
    for (y = 0; y < NewH; y++) {
      y1 = (y << 4) / Ratio;
      y0 = ((y1 > 0) ? (y1-1) : y1) * OldW;
      y2 = ((y1 < (OldImage->Height - 1)) ? (y1+1) : y1) * OldW;
      y1 *= OldW;
      for (x = 0; x < NewW; x++) {
        x1 = (x << 4) / Ratio;
        x0 = (x1 > 0) ? (x1 - 1) : x1;
        x2 = (x1 < (OldW - 1)) ? (x1+1) : x1;
        Dest->Blue = (UINT8)(((INTN)Src[x1+y1].Blue * 2 + Src[x0+y1].Blue +
                           Src[x2+y1].Blue + Src[x1+y0].Blue + Src[x1+y2].Blue) / 6);
        Dest->Green = (UINT8)(((INTN)Src[x1+y1].Green * 2 + Src[x0+y1].Green +
                           Src[x2+y1].Green + Src[x1+y0].Green + Src[x1+y2].Green) / 6);
        Dest->Red = (UINT8)(((INTN)Src[x1+y1].Red * 2 + Src[x0+y1].Red +
                           Src[x2+y1].Red + Src[x1+y0].Red + Src[x1+y2].Red) / 6);
        Dest->Reserved = Src[x1+y1].Reserved;
        Dest++;
      }
    }
  }
  if (Grey) {
    Dest = NewImage->Image.Bitmap;
    for (y = 0; y < NewH; y++) {
      for (x = 0; x < NewW; x++) {
        Dest->Blue = (UINT8)((INTN)((UINTN)Dest->Blue + (UINTN)Dest->Green + (UINTN)Dest->Red) / 3);
        Dest->Green = Dest->Red = Dest->Blue;
        Dest++;
      }
    }
  }

  return NewImage;
}

STATIC
VOID
TakeImage (
  IN EFI_IMAGE_OUTPUT  *Image,
  IN INTN              ScreenXpos,
  IN INTN              ScreenYpos,
  IN INTN              AreaWidth,
  IN INTN              AreaHeight
  )
{
  EFI_STATUS           Status;
  
  if (ScreenXpos + AreaWidth > mScreenWidth) {
    AreaWidth = mScreenWidth - ScreenXpos;
  }
  
  if (ScreenYpos + AreaHeight > mScreenHeight) {
    AreaHeight = mScreenHeight - ScreenYpos;
  }
    
  if (mGraphicsOutput != NULL) {
    Status = mGraphicsOutput->Blt(mGraphicsOutput,
                                  (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *) Image->Image.Bitmap,
                                  EfiBltVideoToBltBuffer,
                                  ScreenXpos,
                                  ScreenYpos,
                                  0,
                                  0,
                                  AreaWidth,
                                  AreaHeight,
                                  (UINTN) Image->Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
                                  );
  } else {
    ASSERT (mUgaDraw != NULL);
    Status = mUgaDraw->Blt(mUgaDraw,
                           (EFI_UGA_PIXEL *) Image->Image.Bitmap,
                           EfiUgaVideoToBltBuffer,
                           ScreenXpos,
                           ScreenYpos,
                           0,
                           0,
                           AreaWidth,
                           AreaHeight,
                           (UINTN) Image->Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
                           );
  }
  
  DEBUG ((DEBUG_INFO, "OCUI: Take Image...%r\n", Status));
}

STATIC
VOID
CreateMenuImage (
  IN EFI_IMAGE_OUTPUT    *Icon,
  IN UINTN               IconCount
  )
{
  EFI_IMAGE_OUTPUT       *NewImage;
  UINT16                 Width;
  UINT16                 Height;
  BOOLEAN                IsTwoRow;
  UINTN                  IconsPerRow;
  INTN                   Xpos;
  INTN                   Ypos;
  
  NewImage = NULL;
  Xpos = 0;
  Ypos = 0;
  
  if (mMenuImage == NULL) {
    mMenuImage = Icon;
    return;
  }
  
  Width = mMenuImage->Width;
  Height = mMenuImage->Height;
  IsTwoRow = mMenuImage->Height > Icon->Height;
  
  if (IsTwoRow) {
    IconsPerRow = mMenuImage->Width / Icon->Width;
    Xpos = (IconCount - IconsPerRow) * Icon->Width;
    Ypos = Icon->Height;
  } else {
    if (mMenuImage->Width + (Icon->Width * 2) <= mScreenWidth) {
      Width = mMenuImage->Width + Icon->Width;
      Xpos = mMenuImage->Width;
    } else {
      Height = mMenuImage->Height + Icon->Height;
      Ypos = Icon->Height;
    }
  }
  
  NewImage = CreateFilledImage (Width, Height, FALSE, mBackgroundPixel);
  if (NewImage == NULL) {
    return;
  }
  
  ComposeImage (NewImage, mMenuImage, 0, 0, FALSE, TRUE);
  if (mMenuImage != NULL) {
    FreeImage (mMenuImage);
  }
  
  ComposeImage (NewImage, Icon, Xpos, Ypos, FALSE, TRUE);
  if (Icon != NULL) {
    FreeImage (Icon);
  }
  
  mMenuImage = NewImage;
}

STATIC
VOID
BltImage (
  IN EFI_IMAGE_OUTPUT    *Image,
  IN INTN                Xpos,
  IN INTN                Ypos
  )
{
  if (Image == NULL) {
    return;
  }
  
  DrawImageArea (Image, 0, 0, 0, 0, Xpos, Ypos);
}

STATIC
EFI_IMAGE_OUTPUT *
CreatTextImage (
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Foreground,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Background,
  IN CHAR16                           *Buffer,
  IN BOOLEAN                           Scale
  )
{
  EFI_STATUS                          Status;
  EFI_IMAGE_OUTPUT                    *Blt;
  EFI_IMAGE_OUTPUT                    *ScaledBlt;
  EFI_FONT_DISPLAY_INFO               FontDisplayInfo;
  EFI_HII_ROW_INFO                    *RowInfoArray;
  UINTN                               RowInfoArraySize;
  
  RowInfoArray  = NULL;
  ScaledBlt = NULL;

  Blt = (EFI_IMAGE_OUTPUT *) AllocateZeroPool (sizeof (EFI_IMAGE_OUTPUT));
  if (Blt == NULL) {
    DEBUG ((DEBUG_INFO, "OCUI: Failed to allocate memory pool.\n"));
    return NULL;
  }
  
  Blt->Width = StrLen (Buffer) * mFontWidth;
  Blt->Height = mFontHeight;
  
  Blt->Image.Bitmap = AllocateZeroPool (Blt->Width * Blt->Height * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  
  if (Blt->Image.Bitmap == NULL) {
    DEBUG ((DEBUG_INFO, "OCUI: Failed to allocate memory pool for Bitmap.\n"));
    FreeImage (Blt);
    return NULL;
  }
  
  ZeroMem (&FontDisplayInfo, sizeof (EFI_FONT_DISPLAY_INFO));
  CopyMem (&FontDisplayInfo.ForegroundColor, Foreground, sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  CopyMem (&FontDisplayInfo.BackgroundColor, Background, sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  
  Status = mHiiFont->StringToImage (
                          mHiiFont,
                          EFI_HII_IGNORE_IF_NO_GLYPH | EFI_HII_OUT_FLAG_CLIP |
                          EFI_HII_OUT_FLAG_CLIP_CLEAN_X | EFI_HII_OUT_FLAG_CLIP_CLEAN_Y |
                          EFI_HII_IGNORE_LINE_BREAK,
                          Buffer,
                          &FontDisplayInfo,
                          &Blt,
                          0,
                          0,
                          &RowInfoArray,
                          &RowInfoArraySize,
                          NULL
                          );
  
  if (!EFI_ERROR (Status) && !Scale) {
    if (RowInfoArray != NULL) {
      FreePool (RowInfoArray);
    }
    return Blt;
  }
  
  if (!EFI_ERROR (Status)) {
    ScaledBlt = CopyScaledImage (Blt, mUiScale);
    if (ScaledBlt == NULL) {
      DEBUG ((DEBUG_INFO, "OCUI: Failed to scale image!\n"));
      if (RowInfoArray != NULL) {
        FreePool (RowInfoArray);
      }
      FreeImage (Blt);
    }
  }
  
  if (RowInfoArray != NULL) {
    FreePool (RowInfoArray);
  }
    
  FreeImage (Blt);
    
  return ScaledBlt;
}

STATIC
VOID
PrintTextGraphicXY (
  IN UINTN                            PointX,
  IN UINTN                            PointY,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Foreground,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Background,
  IN CHAR16                           *Buffer
  )
{
  EFI_STATUS                          Status;
  EFI_IMAGE_OUTPUT                    *Blt;
  EFI_IMAGE_OUTPUT                    *ScaledBlt;
  EFI_FONT_DISPLAY_INFO               FontDisplayInfo;
  EFI_HII_ROW_INFO                    *RowInfoArray;
  UINTN                               RowInfoArraySize;
  
  RowInfoArray  = NULL;
  ScaledBlt = NULL;

  Blt = (EFI_IMAGE_OUTPUT *) AllocateZeroPool (sizeof (EFI_IMAGE_OUTPUT));
  if (Blt == NULL) {
    DEBUG ((DEBUG_INFO, "OCUI: Failed to allocate memory pool.\n"));
    return;
  }
  
  Blt->Width = StrLen (Buffer) * mFontWidth;
  Blt->Height = mFontHeight;
  
  Blt->Image.Bitmap = AllocateZeroPool (Blt->Width * Blt->Height * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  
  if (Blt->Image.Bitmap == NULL) {
    DEBUG ((DEBUG_INFO, "OCUI: Failed to allocate memory pool for Bitmap.\n"));
    FreeImage (Blt);
    return;
  }
  
  ZeroMem (&FontDisplayInfo, sizeof (EFI_FONT_DISPLAY_INFO));
  CopyMem (&FontDisplayInfo.ForegroundColor, Foreground, sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  CopyMem (&FontDisplayInfo.BackgroundColor, Background, sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  
  Status = mHiiFont->StringToImage (
                          mHiiFont,
                          EFI_HII_IGNORE_IF_NO_GLYPH | EFI_HII_OUT_FLAG_CLIP |
                          EFI_HII_OUT_FLAG_CLIP_CLEAN_X | EFI_HII_OUT_FLAG_CLIP_CLEAN_Y |
                          EFI_HII_IGNORE_LINE_BREAK,
                          Buffer,
                          &FontDisplayInfo,
                          &Blt,
                          0,
                          0,
                          &RowInfoArray,
                          &RowInfoArraySize,
                          NULL
                          );
  
  if (!EFI_ERROR (Status)) {
    ScaledBlt = CopyScaledImage (Blt, mUiScale);
    if (ScaledBlt == NULL) {
      DEBUG ((DEBUG_INFO, "OCUI: Failed to scale image!\n"));
      if (RowInfoArray != NULL) {
        FreePool (RowInfoArray);
      }
      FreeImage (Blt);
    }
    
    if ((PointY + ScaledBlt->Height + 5) > mScreenHeight) {
      PointY = mScreenHeight - (ScaledBlt->Height + 5);
    }
    
    if ((PointX + ScaledBlt->Width + 10) > mScreenWidth) {
      PointX = mScreenWidth - (ScaledBlt->Width + 10);
    }
    
    if (mGraphicsOutput != NULL) {
      Status = mGraphicsOutput->Blt(mGraphicsOutput,
                                    (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *) ScaledBlt->Image.Bitmap,
                                    EfiBltBufferToVideo,
                                    0,
                                    0,
                                    PointX,
                                    PointY,
                                    ScaledBlt->Width,
                                    ScaledBlt->Height,
                                    ScaledBlt->Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
                                    );
    } else {
      ASSERT (mUgaDraw != NULL);
      Status = mUgaDraw->Blt(mUgaDraw,
                              (EFI_UGA_PIXEL *) Blt->Image.Bitmap,
                              EfiUgaBltBufferToVideo,
                              0,
                              0,
                              PointX,
                              PointY,
                              ScaledBlt->Width,
                              ScaledBlt->Height,
                              ScaledBlt->Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
                              );
    }
  }
  
  if (RowInfoArray != NULL) {
    FreePool (RowInfoArray);
  }
  
  FreeImage (Blt);
  FreeImage (ScaledBlt);
}

EFI_IMAGE_OUTPUT *
DecodePNGFile (
  IN CHAR16                       *FilePath
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  VOID                             *Buffer;
  UINT32                           BufferSize;
  EFI_IMAGE_OUTPUT                 *NewImage;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Pixel;
  VOID                             *Data;
  UINT32                           Width;
  UINT32                           Height;
  UINT8                            *DataWalker;
  UINTN                            X;
  UINTN                            Y;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;

  BufferSize = 0;
  HandleCount = 0;
  FileSystem = NULL;
  Buffer = NULL;
  
  if (mFileSystem == NULL) {
    Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiPartTypeSystemPartGuid, NULL, &HandleCount, &Handles);
    if (!EFI_ERROR (Status) && HandleCount > 0) {
      for (Index = 0; Index < HandleCount; ++Index) {
        Status = gBS->HandleProtocol (
                        Handles[Index],
                        &gEfiSimpleFileSystemProtocolGuid,
                        (VOID **) &FileSystem
                        );
        if (EFI_ERROR (Status)) {
          FileSystem = NULL;
          continue;
        }
        
        Buffer = ReadFile (FileSystem, FilePath, &BufferSize, BASE_16MB);
        if (Buffer != NULL) {
          mFileSystem = FileSystem;
          DEBUG ((DEBUG_INFO, "OCUI: FileSystem found!  Handle(%d) \n", Index));
          break;
        }
        FileSystem = NULL;
      }
      
      if (Handles != NULL) {
        FreePool (Handles);
      }
    }
    
  } else {
    Buffer = ReadFile (mFileSystem, FilePath, &BufferSize, BASE_16MB);
  }
  
  if (Buffer == NULL) {
    DEBUG ((DEBUG_ERROR, "OCUI: Failed to locate valid png file - %p!\n", Buffer));
    return NULL;
  }
  
  Status = DecodePng (
               Buffer,
               BufferSize,
               &Data,
               &Width,
               &Height,
               NULL
              );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCUI: DecodePNG...%r\n", Status));
    if (Buffer != NULL) {
      FreePool (Buffer);
    }
    return NULL;
  }
    
  NewImage = CreateImage ((INTN) Width, (INTN) Height);
  if (NewImage == NULL) {
    if (Buffer != NULL) {
      FreePool (Buffer);
    }
    return NULL;
  }
  
  Pixel = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *) NewImage->Image.Bitmap;
  DataWalker = (UINT8 *) Data;
  for (Y = 0; Y < NewImage->Height; Y++) {
    for (X = 0; X < NewImage->Width; X++) {
      Pixel->Red = *DataWalker++;
      Pixel->Green = *DataWalker++;
      Pixel->Blue = *DataWalker++;
      Pixel->Reserved = 255 - *DataWalker++;
      Pixel++;
    }
  }
  if (Buffer != NULL) {
    FreePool (Buffer);
  }
  FreePng (Data);
  DEBUG ((DEBUG_INFO, "OCUI: DecodePNG...%r\n", Status));
  return NewImage;
}

STATIC
VOID
CreateIcon (
  IN CHAR16               *Name,
  IN OC_BOOT_ENTRY_TYPE   Type,
  IN UINTN                IconCount,
  IN BOOLEAN              IsDefault,
  IN BOOLEAN              Ext,
  IN BOOLEAN              Dmg,
  IN BOOLEAN              Selected
  )
{
  CHAR16                 *FilePath;
  EFI_IMAGE_OUTPUT       *Icon;
  EFI_IMAGE_OUTPUT       *NewImage;
  
  Icon = NULL;
  
  switch (Type) {
    case OcBootWindows:
      if (StrStr (Name, L"10") != NULL) {
        FilePath = L"EFI\\OC\\Icons\\os_win10.icns";
      } else {
        FilePath = L"EFI\\OC\\Icons\\os_win.icns";
      }
      break;
    case OcBootApple:
      if (StrStr (Name, L"Cata") != NULL) {
        FilePath = L"EFI\\OC\\Icons\\os_cata.icns";
      } else if (StrStr (Name, L"Moja") != NULL) {
        FilePath = L"EFI\\OC\\Icons\\os_moja.icns";
      } else {
        FilePath = L"EFI\\OC\\Icons\\os_mac.icns";
      }
      break;
    case OcBootAppleRecovery:
      FilePath = L"EFI\\OC\\Icons\\os_recovery.icns";
      break;
    case OcBootCustom:
      if (StrStr (Name, L"Free") != NULL) {
        FilePath = L"EFI\\OC\\Icons\\os_freebsd.icns";
      } else if (StrStr (Name, L"Linux") != NULL) {
        FilePath = L"EFI\\OC\\Icons\\os_linux.icns";
      } else if (StrStr (Name, L"Redhat") != NULL) {
        FilePath = L"EFI\\OC\\Icons\\os_redhat.icns";
      } else if (StrStr (Name, L"Ubuntu") != NULL) {
        FilePath = L"EFI\\OC\\Icons\\os_ubuntu.icns";
      } else if (StrStr (Name, L"Fedora") != NULL) {
        FilePath = L"EFI\\OC\\Icons\\os_fedora.icns";
      } else if (StrStr (Name, L"Shell") != NULL) {
        FilePath = L"EFI\\OC\\Icons\\tool_shell.icns";
      } else {
        FilePath = L"EFI\\OC\\Icons\\os_custom.icns";
      }
      break;
    case OcBootSystem:
      FilePath = L"EFI\\OC\\Icons\\func_resetnvram.icns";
      break;
    case OcBootUnknown:
      FilePath = L"EFI\\OC\\Icons\\os_unknown.icns";
      break;
      
    default:
      FilePath = L"EFI\\OC\\Icons\\os_unknown.icns";
      break;
  }
  
  NewImage = CreateFilledImage (mIconSpaceSize, mIconSpaceSize, FALSE, Selected ? mFontColorPixel : mBackgroundPixel);
  if (NewImage == NULL) {
    DEBUG ((DEBUG_INFO, "OCUI: Failed create Dummy Icon\n"));
    return;
  }
  
  Icon = DecodePNGFile (FilePath);
  
  if (Icon != NULL) {
    ComposeImage (NewImage, Icon, 4, 4, FALSE, FALSE);
    if (Icon != NULL) {
     FreeImage (Icon);
    }
  }
  CreateMenuImage (NewImage, IconCount);
}

STATIC
VOID
SwitchIconSelection (
  IN UINTN               IconCount,
  IN UINTN               IconIndex,
  IN BOOLEAN             Selected
  )
{
  EFI_IMAGE_OUTPUT       *NewImage;
  EFI_IMAGE_OUTPUT       *Icon;
  BOOLEAN                IsTwoRow;
  INTN                   Xpos;
  INTN                   Ypos;
  UINT16                 Width;
  UINT16                 Height;
  UINTN                  IconsPerRow;
  
  /* Begin Calulating Xpos and Ypos of current selected icon on screen*/
  NewImage = NULL;
  Icon = NULL;
  IsTwoRow = FALSE;
  Xpos = 0;
  Ypos = 0;
  Width = mIconSpaceSize;
  Height = mIconSpaceSize; // Assuming it's only 1 row first.
  IconsPerRow = 1; // At least 1 first.
  
  for (IconsPerRow = 1; IconsPerRow < IconCount; ++IconsPerRow) {
    Width = Width + mIconSpaceSize;
    if ((Width + (mIconSpaceSize * 2)) >= mScreenWidth) {
      break;
    }
  }
  
  if (IconsPerRow < IconCount) {
    IsTwoRow = TRUE;  // Definitely 2 rows here
    Height = mIconSpaceSize * 2;  // Only 2 rows max, more than that menu will be busted anyway due to too many entries to handle.
    if (IconIndex <= IconsPerRow) {
      // It's probably on first row.
      Xpos = (mScreenWidth - Width) / 2 + (mIconSpaceSize * IconIndex);
      Ypos = (mScreenHeight / 2) - mIconSpaceSize;
    } else {
      Xpos = (mScreenWidth - Width) / 2 + (mIconSpaceSize * (IconIndex - (IconsPerRow + 1)));
      Ypos = mScreenHeight / 2;
    }
  } else {
    Xpos = (mScreenWidth - Width) / 2 + (mIconSpaceSize * IconIndex);
    Ypos = (mScreenHeight / 2) - mIconSpaceSize;
  }
  /* Done Calulating Xpos and Ypos of current selected icon on screen*/
  
  Icon = CreateImage (mIconSpaceSize - 8, mIconSpaceSize - 8);
  if (Icon == NULL) {
    return;
  }
  TakeImage (Icon, Xpos + 4, Ypos + 4, Icon->Width, Icon->Height);
  
  NewImage = CreateFilledImage (mIconSpaceSize, mIconSpaceSize, FALSE, Selected ? mFontColorPixel : mBackgroundPixel);
  ComposeImage (NewImage, Icon, 4, 4, FALSE, FALSE);
  if (Icon != NULL) {
    FreeImage (Icon);
  }
  
  BltImage (NewImage, Xpos, Ypos);
  if (NewImage != NULL) {
    FreeImage (NewImage);
  }
}

STATIC
VOID
OcClearScreen (
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Color
  )
{
  if (mBackgroundImage != NULL && (mBackgroundImage->Width != mScreenWidth || mBackgroundImage->Height != mScreenHeight)) {
    FreeImage(mBackgroundImage);
    mBackgroundImage = NULL;
  }
  
  if (mBackgroundImage == NULL) {
    mBackgroundImage = CreateFilledImage (mScreenWidth, mScreenHeight, FALSE, Color);
    if (mBackgroundImage != NULL) {
      BltImage (mBackgroundImage, 0, 0);
    }
  }
}

STATIC
VOID
OcClearScreenArea (
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Color,
  IN INTN                           AreaXpos,
  IN INTN                           AreaYpos,
  IN INTN                           AreaWidth,
  IN INTN                           AreaHeight
  )
{
  EFI_STATUS                        Status;
  EFI_UGA_PIXEL                     FillColor;
  
  FillColor.Red      = Color->Red;
  FillColor.Green    = Color->Green;
  FillColor.Blue     = Color->Blue;
  FillColor.Reserved = 0;
  
  if (mGraphicsOutput != NULL) {
    Status = mGraphicsOutput->Blt(mGraphicsOutput,
                                  (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *) &FillColor,
                                  EfiBltVideoFill,
                                  0,
                                  0,
                                  AreaXpos,
                                  AreaYpos,
                                  AreaWidth,
                                  AreaHeight,
                                  0
                                  );
  } else {
    ASSERT (mUgaDraw != NULL);
    Status = mUgaDraw->Blt(mUgaDraw,
                            &FillColor,
                            EfiUgaVideoFill,
                            0,
                            0,
                            AreaXpos,
                            AreaYpos,
                            AreaWidth,
                            AreaHeight,
                            0
                            );
  }
  
  DEBUG ((DEBUG_INFO, "OCUI: Clearing Graphic Screen Area...%r\n", Status));
}

STATIC
VOID
InitScreen (
  VOID
  )
{
  EFI_STATUS    Status;
  EFI_HANDLE    Handle;
  UINT32        ColorDepth;
  UINT32        RefreshRate;
  UINT32        ScreenWidth;
  UINT32        ScreenHeight;
  
  
  
  Handle = NULL;
  mUgaDraw = NULL;
  //
  // Try to open GOP first
  //
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **) &mGraphicsOutput);
  if (EFI_ERROR (Status)) {
    mGraphicsOutput = NULL;
    //
    // Open GOP failed, try to open UGA
    //
    Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiUgaDrawProtocolGuid, (VOID **) &mUgaDraw);
    if (EFI_ERROR (Status)) {
      mUgaDraw = NULL;
    }
    
  }
  
  if (mGraphicsOutput != NULL) {
    mScreenWidth = mGraphicsOutput->Mode->Info->HorizontalResolution;
    mScreenHeight = mGraphicsOutput->Mode->Info->VerticalResolution;
    mUiScale = (mUiScale == 0 && mScreenHeight >= 2160) ? 28 : 16;
  } else {
    ASSERT (mUgaDraw != NULL);
    Status = mUgaDraw->GetMode (mUgaDraw, &ScreenWidth, &ScreenHeight, &ColorDepth, &RefreshRate);
    mScreenWidth = ScreenWidth;
    mScreenHeight = ScreenHeight;
  }
  DEBUG ((DEBUG_INFO, "OCUI: Initialize Graphic Screen...%r\n", Status));
  
  Status = gBS->LocateProtocol (&gEfiHiiFontProtocolGuid, NULL, (VOID **) &mHiiFont);
  
  if (EFI_ERROR (Status)) {
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Handle,
                    &gEfiHiiFontProtocolGuid,
                    &mHiiFont,
                    NULL
                    );
    DEBUG ((DEBUG_INFO, "OCUI: No HiiFont found, installing...%r\n", Status));
    if (EFI_ERROR (Status)) {
      mHiiFont = NULL;
    }
  }
  
  DEBUG ((DEBUG_INFO, "OCUI: Initialize HiiFont...%r\n", Status));
  
  Status = OcConsoleControlSetBehaviour (OcConsoleControlGraphics);
  
  DEBUG ((DEBUG_INFO, "OCUI: Set ConsoleControlGraphic...%r\n", Status));
  OcGetGlyph ();
}

STATIC
VOID
PrintDateTime (
  VOID
  )
{
  EFI_STATUS         Status;
  EFI_TIME           DateTime;
  CHAR16             DateStr[11];
  CHAR16             TimeStr[11];
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
    UnicodeSPrint (DateStr, sizeof (DateStr), L"%02u/%02u/%04u", DateTime.Month, DateTime.Day, DateTime.Year);
    UnicodeSPrint (TimeStr, sizeof (TimeStr), L"%02u:%02u:%02u%s", Hour, DateTime.Minute, DateTime.Second, Str);
    PrintTextGraphicXY (mScreenWidth - ((StrLen(DateStr) * mFontWidth) + 10), 5, mFontColorPixelAlt, mBackgroundPixel, DateStr);
    PrintTextGraphicXY (mScreenWidth - ((StrLen(DateStr) * mFontWidth) + 10), (mUiScale == 16) ? (mFontHeight + 5 + 2) : ((mFontHeight * 2) + 5 + 2), mFontColorPixelAlt, mBackgroundPixel, TimeStr);
  }
}

STATIC
VOID
PrintOcVersion (
  IN CONST CHAR8         *String
  )
{
  CHAR16                 *NewString;
  
  NewString = AsciiStrCopyToUnicode (String, 0);
  
  if (String != NULL) {
    PrintTextGraphicXY (mScreenWidth - ((StrLen(NewString) * mFontWidth) + 10), mScreenHeight - (mFontHeight + 5), mFontColorPixelAlt, mBackgroundPixel, NewString);
  }
}

STATIC
VOID
PrintDefaultBootMode (
  VOID
  )
{
  CHAR16             String[17];
  
  UnicodeSPrint (String, sizeof (String), L"Auto default:%s", mAllowSetDefault ? L"Off" : L"On");
  PrintTextGraphicXY (10, mScreenHeight - (mFontHeight + 5), mFontColorPixelAlt, mBackgroundPixel, String);
}

STATIC
BOOLEAN
PrintTimeOutMessage (
  IN UINTN           Timeout
  )
{
  EFI_IMAGE_OUTPUT     *TextImage;
  EFI_IMAGE_OUTPUT     *NewImage;
  CHAR16               String[52];
  
  TextImage = NULL;
  NewImage = NULL;
  
  if (Timeout > 0) {
    UnicodeSPrint (String, sizeof (String), L"%s %02u %s.", L"The default boot selection will start in", Timeout, L"seconds"); //52
    TextImage = CreatTextImage (mFontColorPixelAlt, mBackgroundPixel, String, TRUE);
    NewImage = CreateFilledImage (mScreenWidth, TextImage->Height, FALSE, mBackgroundPixel);
    if (NewImage == NULL) {
      FreeImage (TextImage);
      return !(Timeout > 0);
    }
    ComposeImage (NewImage, TextImage, (NewImage->Width - TextImage->Width) / 2, 0, FALSE, FALSE);
    if (TextImage != NULL) {
      FreeImage (TextImage);
    }
    BltImage (NewImage, (mScreenWidth - NewImage->Width) / 2, (mScreenHeight / 4) * 3);
    if (NewImage != NULL) {
      FreeImage (NewImage);
    }
  } else {
    OcClearScreenArea (mBackgroundPixel, 0, ((mScreenHeight / 4) * 3) - 4, mScreenWidth, mFontHeight * 2);
  }
  return !(Timeout > 0);
}

STATIC
VOID
PrintTextDesrciption (
  IN UINTN        MaxStrWidth,
  IN UINTN        Selected,
  IN CHAR16       *Name,
  IN BOOLEAN      Ext,
  IN BOOLEAN      Dmg
  )
{
  EFI_IMAGE_OUTPUT                   *TextImage;
  EFI_IMAGE_OUTPUT                   *NewImage;
  CHAR16                             Code[3];
  CHAR16                             String[MaxStrWidth + 1];
  
  Code[0] = 0x20;
  Code[1] = OC_INPUT_STR[Selected];
  Code[2] = '\0';
  
  UnicodeSPrint (String, sizeof (String), L" %s%s%s%s%s ",
                 Code,
                 (mAllowSetDefault && mDefaultEntry == Selected) ? L".*" : L". ",
                 Name,
                 Ext ? L" (ext)" : L"",
                 Dmg ? L" (dmg)" : L""
                 );
  
  TextImage = CreatTextImage (mFontColorPixel, mBackgroundPixel, String, TRUE);
  NewImage = CreateFilledImage (mScreenWidth, TextImage->Height, FALSE, mBackgroundPixel);
  if (NewImage == NULL) {
    FreeImage (TextImage);
    return;
  }
  
  ComposeImage (NewImage, TextImage, (NewImage->Width - TextImage->Width) / 2, 0, FALSE, FALSE);
  if (TextImage != NULL) {
    FreeImage (TextImage);
  }
  BltImage (NewImage, (mScreenWidth - NewImage->Width) / 2, (mScreenHeight / 2) + mIconSpaceSize);
  if (NewImage != NULL) {
    FreeImage (NewImage);
  }
}

STATIC
VOID
RestoreConsoleMode (
  IN EFI_SIMPLE_TEXT_OUTPUT_MODE     SavedConsoleMode
  )
{
  gST->ConOut->ClearScreen (gST->ConOut);
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
  UINTN                              MaxStrWidth;
  UINTN                              StrWidth;
  APPLE_KEY_MAP_AGGREGATOR_PROTOCOL  *KeyMap;
  BOOLEAN                            SetDefault;
  BOOLEAN                            NewDefault;
  BOOLEAN                            TimeoutExpired;
  
  VisibleIndex     = 0;
  ShowAll          = FALSE;
  MaxStrWidth      = 0;
  TimeoutExpired   = FALSE;
  TimeOutSeconds   = Context->TimeoutSeconds;
  mAllowSetDefault = Context->AllowSetDefault;
  mDefaultEntry    = DefaultEntry;
  
  KeyMap = OcAppleKeyMapInstallProtocols (FALSE);
  if (KeyMap == NULL) {
    DEBUG ((DEBUG_ERROR, "OCSBM: Missing AppleKeyMapAggregator\n"));
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
  ConOut->EnableCursor (ConOut, FALSE);
  ConOut->ClearScreen (ConOut);
  
  
  InitScreen ();
  OcClearScreen (mBackgroundPixel);
  PrintDateTime ();
  PrintDefaultBootMode ();
  PrintOcVersion (Context->TitleSuffix);
  
  while (TRUE) {
    if (!TimeoutExpired) {
      TimeoutExpired = PrintTimeOutMessage (TimeOutSeconds);
      TimeOutSeconds = TimeoutExpired ? 10000 : TimeOutSeconds;
    }
    for (Index = 0, VisibleIndex = 0; Index < MIN (Count, OC_INPUT_MAX); ++Index) {
      if ((BootEntries[Index].Hidden && !ShowAll)
          || (BootEntries[Index].Type == OcBootSystem && !ShowAll)) {
        DefaultEntry = DefaultEntry == Index ? 0 : DefaultEntry;
        continue;
      }
      if (DefaultEntry == Index) {
        Selected = VisibleIndex;
      }
      VisibleList[VisibleIndex] = Index;
      CreateIcon (BootEntries[Index].Name,
                  BootEntries[Index].Type,
                  VisibleIndex,
                  VisibleIndex,
                  BootEntries[Index].IsExternal,
                  BootEntries[Index].IsFolder,
                  DefaultEntry == Index
                  );
      ++VisibleIndex;
    }
    
    OcClearScreenArea (mBackgroundPixel, 0, (mScreenHeight / 2) - mIconSpaceSize, mScreenWidth, mIconSpaceSize * 2);
    BltImage (mMenuImage, (mScreenWidth - mMenuImage->Width) / 2, (mScreenHeight / 2) - mIconSpaceSize);
    PrintTextDesrciption (MaxStrWidth,
                          Selected,
                          BootEntries[Selected].Name,
                          BootEntries[Selected].IsExternal,
                          BootEntries[Selected].IsFolder
                          );
    
    if (mMenuImage != NULL) {
      FreeImage (mMenuImage);
      mMenuImage = NULL;
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
        NewDefault = BootEntries[DefaultEntry].DevicePath != NULL
          && !BootEntries[DefaultEntry].Hidden
          && !Context->AllowSetDefault
          && mDefaultEntry != DefaultEntry;
        
        if (SetDefault || NewDefault) {
          Status = OcSetDefaultBootEntry (Context, &BootEntries[DefaultEntry]);
          DEBUG ((DEBUG_INFO, "OCSBM: Setting default - %r\n", Status));
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
        SwitchIconSelection (VisibleIndex, Selected, FALSE);
        DefaultEntry = Selected > 0 ? VisibleList[Selected - 1] : VisibleList[VisibleIndex - 1];
        Selected = Selected > 0 ? --Selected : VisibleIndex - 1;
        SwitchIconSelection (VisibleIndex, Selected, TRUE);
        PrintTextDesrciption (MaxStrWidth,
                              Selected,
                              BootEntries[DefaultEntry].Name,
                              BootEntries[DefaultEntry].IsExternal,
                              BootEntries[DefaultEntry].IsFolder
                              );
        TimeOutSeconds = 0;
      } else if (KeyIndex == OC_INPUT_DOWN) {
        SwitchIconSelection (VisibleIndex, Selected, FALSE);
        DefaultEntry = Selected < (VisibleIndex - 1) ? VisibleList[Selected + 1] : 0;
        Selected = Selected < (VisibleIndex - 1) ? ++Selected : 0;
        SwitchIconSelection (VisibleIndex, Selected, TRUE);
        PrintTextDesrciption (MaxStrWidth,
                              Selected,
                              BootEntries[DefaultEntry].Name,
                              BootEntries[DefaultEntry].IsExternal,
                              BootEntries[DefaultEntry].IsFolder
                              );
        TimeOutSeconds = 0;
      } else if (KeyIndex != OC_INPUT_INVALID && (UINTN)KeyIndex < VisibleIndex) {
        ASSERT (KeyIndex >= 0);
        *ChosenBootEntry = &BootEntries[VisibleList[KeyIndex]];
        SetDefault = BootEntries[VisibleList[KeyIndex]].DevicePath != NULL
          && !BootEntries[VisibleList[KeyIndex]].Hidden
          && Context->AllowSetDefault
          && SetDefault;
        NewDefault = BootEntries[VisibleList[KeyIndex]].DevicePath != NULL
          && !BootEntries[VisibleList[KeyIndex]].Hidden
          && !Context->AllowSetDefault
          && mDefaultEntry != VisibleList[KeyIndex];
        if (SetDefault || NewDefault) {
          Status = OcSetDefaultBootEntry (Context, &BootEntries[VisibleList[KeyIndex]]);
          DEBUG ((DEBUG_INFO, "OCSBM: Setting default - %r\n", Status));
        }
        RestoreConsoleMode (SavedConsoleMode);
        return EFI_SUCCESS;
      } else if (KeyIndex != OC_INPUT_TIMEOUT) {
        TimeOutSeconds = 0;
      }

      if (!TimeoutExpired) {
        PrintDateTime ();
        TimeoutExpired = PrintTimeOutMessage (TimeOutSeconds);
        TimeOutSeconds = TimeoutExpired ? 10000 : TimeOutSeconds;
      } else {
        PrintDateTime ();
      }
    }
  }

  ASSERT (FALSE);
}
