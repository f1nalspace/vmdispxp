/*
 * fvm3dx display page -- tells the fvm3dx device what lies on top of a 3D window.
 *
 * The device composes every 3D window's picture over the desktop on the host. Without help it covers whatever Windows draws on top of the
 * window: the pointer GDI draws into the frame buffer, and every menu or window that overlaps it. This file keeps a page at the end of the
 * video memory up to date (layout and protocol in shared/fvm3dx_display_page.h): the pointer's shape and position from GDI's pointer calls, and
 * the visible part of every window an ICD announced with ExtEscape(WNDOBJ_SETUP), from the WNDOBJ callbacks GDI makes on every change.
 * On a 32 bit screen the driver draws the pointer itself, see the pointer section below.
 *
 * GDI calls all of this under its device lock, so there is one writer at a time. The video memory is mapped write-combined, so every seqlock
 * step ends with a fence: it drains the write-combining buffers, and the host never sees the data ahead of an odd sequence number.
 */

#include "framebuf.h"
#include "fvm3dx_display_page.h"

#define BITS_PER_BYTE 8
#define CHANNEL_MAXIMUM 255
#define OPAQUE_BLACK 0xFF000000u
#define OPAQUE_WHITE 0xFFFFFFFFu
#define TRANSPARENT_PIXEL 0x00000000u
#define INVERT_COLOR 0x00FFFFFFu
#define RGB_MASK 0x00FFFFFFu
#define ALPHA_OPAQUE 0xFF000000u
#define ALPHA_SHIFT 24
#define RED_SHIFT 16
#define GREEN_SHIFT 8
#define BLUE_SHIFT 0
#define BITS_PER_PIXEL_32 32
#define BYTES_PER_PIXEL_16 2
#define BYTES_PER_PIXEL_24 3
#define BYTES_PER_PIXEL_32 4
#define NIBBLES_PER_BYTE 2
#define NIBBLE_BITS 4
#define NIBBLE_MASK 0x0Fu
#define MONOCHROME_PLANES 2
#define ENUMERATION_BATCH 8
#define HIDDEN_POSITION (-1)

typedef struct _RECTANGLE_BATCH
{
   ULONG Count;
   RECTL Rectangles[ENUMERATION_BATCH];
} RECTANGLE_BATCH;

/* The page as the screen's PDEV maps it; NULL while no PDEV with an enabled surface maps it, when GDI may still call back. */
static volatile Fvm3dxDisplayPage *DisplayPage;
/* The PDEV whose mapping DisplayPage points into. */
static PPDEV DisplayPageOwner;

/*
 * Every PDEV with an enabled surface maps the video memory on its own, and every mapping reaches the same page. Usually GDI enables the new PDEV's
 * surface on a mode change and then disables the old one. While something still holds the desktop's PDEV -- Painkiller's Direct3D device on the
 * desktop window -- GDI keeps it beside the full screen one and goes back to it with DrvAssertMode alone, without DrvEnableSurface.
 * The page then has to come from that PDEV's mapping again, or the driver loses it until the next new PDEV.
 */
#define FVM3DX_PAGE_MAPPING_COUNT 4

typedef struct _FVM3DX_PAGE_MAPPING
{
   PPDEV Device;
   volatile Fvm3dxDisplayPage *Page;
} FVM3DX_PAGE_MAPPING;

static FVM3DX_PAGE_MAPPING PageMappings[FVM3DX_PAGE_MAPPING_COUNT];
/* Cleared once per boot: a page from before a reboot names drawables that no longer exist. A mode change keeps it. */
static BOOL DisplayPageClearedThisBoot;
/* The device's colour layout, for turning pointer pixels into RGB. */
static ULONG DeviceRedMask;
static ULONG DeviceGreenMask;
static ULONG DeviceBlueMask;

static VOID
Fvm3dxFence(VOID)
{
   __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static VOID
Fvm3dxBeginWrite(volatile uint32_t *Sequence)
{
   uint32_t OddSequence = *Sequence + 1;
   *Sequence = OddSequence;
   Fvm3dxFence();
}

static VOID
Fvm3dxEndWrite(volatile uint32_t *Sequence)
{
   Fvm3dxFence();
   uint32_t EvenSequence = *Sequence + 1;
   *Sequence = EvenSequence;
   Fvm3dxFence();
}

static ULONG
Fvm3dxLowestSetBit(ULONG Mask)
{
   ULONG Shift = 0;
   while (Mask != 0 && (Mask & 1) == 0)
   {
      Mask >>= 1;
      Shift++;
   }
   return Shift;
}

/* One channel of a device colour, widened to eight bits. */
static ULONG
Fvm3dxChannel(ULONG DeviceColor, ULONG Mask)
{
   if (Mask == 0)
   {
      return 0;
   }
   ULONG Shift = Fvm3dxLowestSetBit(Mask);
   ULONG Maximum = Mask >> Shift;
   ULONG Value = (DeviceColor & Mask) >> Shift;
   ULONG Widened = (Value * CHANNEL_MAXIMUM + Maximum / 2) / Maximum;
   return Widened;
}

static ULONG
Fvm3dxDeviceColorToRgb(ULONG DeviceColor)
{
   ULONG Red = Fvm3dxChannel(DeviceColor, DeviceRedMask);
   ULONG Green = Fvm3dxChannel(DeviceColor, DeviceGreenMask);
   ULONG Blue = Fvm3dxChannel(DeviceColor, DeviceBlueMask);
   ULONG Rgb = (Red << RED_SHIFT) | (Green << GREEN_SHIFT) | Blue;
   return Rgb;
}

static PBYTE
Fvm3dxRow(SURFOBJ *Surface, LONG Row)
{
   PBYTE RowStart = (PBYTE)Surface->pvScan0 + (LONG_PTR)Row * Surface->lDelta;
   return RowStart;
}

static ULONG
Fvm3dxMaskBit(SURFOBJ *Mask, LONG Row, LONG Column)
{
   PBYTE RowStart = Fvm3dxRow(Mask, Row);
   BYTE Packed = RowStart[Column / BITS_PER_BYTE];
   ULONG BitPosition = (BITS_PER_BYTE - 1) - (Column % BITS_PER_BYTE);
   ULONG Bit = (Packed >> BitPosition) & 1;
   return Bit;
}

/* The raw value of one pixel of the colour bitmap; FALSE for a format this file does not read. */
static BOOL
Fvm3dxReadPixel(SURFOBJ *Surface, LONG Row, LONG Column, ULONG *Value)
{
   PBYTE RowStart = Fvm3dxRow(Surface, Row);
   switch (Surface->iBitmapFormat)
   {
      case BMF_1BPP:
         *Value = Fvm3dxMaskBit(Surface, Row, Column);
         return TRUE;
      case BMF_4BPP:
      {
         BYTE Packed = RowStart[Column / NIBBLES_PER_BYTE];
         ULONG Shift = ((Column % NIBBLES_PER_BYTE) == 0) ? NIBBLE_BITS : 0;
         *Value = (Packed >> Shift) & NIBBLE_MASK;
         return TRUE;
      }
      case BMF_8BPP:
         *Value = RowStart[Column];
         return TRUE;
      case BMF_16BPP:
         *Value = *(USHORT *)(RowStart + Column * BYTES_PER_PIXEL_16);
         return TRUE;
      case BMF_24BPP:
      {
         PBYTE Pixel = RowStart + Column * BYTES_PER_PIXEL_24;
         *Value = Pixel[0] | (Pixel[1] << GREEN_SHIFT) | (Pixel[2] << RED_SHIFT);
         return TRUE;
      }
      case BMF_32BPP:
         *Value = *(ULONG *)(RowStart + Column * BYTES_PER_PIXEL_32);
         return TRUE;
      default:
         return FALSE;
   }
}

/* The mapping kept for a PDEV, or with NULL a free one; NULL if there is none. */
static FVM3DX_PAGE_MAPPING *
Fvm3dxFindPageMapping(PPDEV ppdev)
{
   for (ULONG Index = 0; Index < FVM3DX_PAGE_MAPPING_COUNT; Index++)
   {
      if (PageMappings[Index].Device == ppdev)
      {
         return &PageMappings[Index];
      }
   }
   return NULL;
}

/* From here on the driver writes the page through this PDEV's mapping, and reads pointer pixels in its colour layout. */
static VOID
Fvm3dxUsePageMapping(const FVM3DX_PAGE_MAPPING *Mapping)
{
   PPDEV ppdev = Mapping->Device;
   DeviceRedMask = ppdev->RedMask;
   DeviceGreenMask = ppdev->GreenMask;
   DeviceBlueMask = ppdev->BlueMask;
   DisplayPage = Mapping->Page;
   DisplayPageOwner = ppdev;
}

VOID
Fvm3dxDisplayPageAttach(PPDEV ppdev, PVOID VideoRamBase, ULONG VideoRamLength, ULONG FrameBufferLength)
{
   volatile Fvm3dxDisplayPage *Page = NULL;
   if (VideoRamLength < FrameBufferLength + FVM3DX_DISPLAY_PAGE_SIZE)
   {
      DbgPortLineHex("qemudisp: no room for the fvm3dx display page, video memory ", VideoRamLength);
   }
   else
   {
      Page = (volatile Fvm3dxDisplayPage *)((PBYTE)VideoRamBase + VideoRamLength - FVM3DX_DISPLAY_PAGE_SIZE);
   }
   if (Page != NULL && !DisplayPageClearedThisBoot)
   {
      Page->magic = 0;
      Fvm3dxFence();
      memset((PVOID)Page, 0, sizeof(Fvm3dxDisplayPage));
      Page->version = FVM3DX_DISPLAY_PAGE_VERSION;
      Page->sizeInBytes = FVM3DX_DISPLAY_PAGE_SIZE;
      Fvm3dxFence();
      Page->magic = FVM3DX_DISPLAY_PAGE_MAGIC;
      Fvm3dxFence();
      DisplayPageClearedThisBoot = TRUE;
      DbgPortLineHex("qemudisp: fvm3dx display page set up at ", (unsigned long)Page);
   }
   FVM3DX_PAGE_MAPPING *Mapping = Fvm3dxFindPageMapping(ppdev);
   if (Mapping == NULL)
   {
      const PPDEV FreeMapping = NULL;
      Mapping = Fvm3dxFindPageMapping(FreeMapping);
   }
   if (Mapping == NULL)
   {
      /* More PDEVs with a surface than GDI ever keeps: this one gets the page, but going back to another cannot find it again. */
      DbgPortLine("qemudisp: fvm3dx display page, no free mapping");
      FVM3DX_PAGE_MAPPING Unkept = { ppdev, Page };
      Fvm3dxUsePageMapping(&Unkept);
      return;
   }
   Mapping->Device = ppdev;
   Mapping->Page = Page;
   Fvm3dxUsePageMapping(Mapping);
}

VOID
Fvm3dxDisplayPageDetach(PPDEV ppdev)
{
   FVM3DX_PAGE_MAPPING *Mapping = Fvm3dxFindPageMapping(ppdev);
   if (Mapping != NULL)
   {
      Mapping->Device = NULL;
      Mapping->Page = NULL;
   }
   if (DisplayPageOwner != ppdev)
   {
      return;
   }
   DisplayPage = NULL;
   DisplayPageOwner = NULL;
   /* A PDEV GDI kept beside this one still maps the same page. */
   for (ULONG Index = 0; Index < FVM3DX_PAGE_MAPPING_COUNT; Index++)
   {
      if (PageMappings[Index].Device != NULL)
      {
         Fvm3dxUsePageMapping(&PageMappings[Index]);
         DbgPortLineHex("qemudisp: fvm3dx display page kept through mode index ", PageMappings[Index].Device->ModeIndex);
         break;
      }
   }
}

/* DrvAssertMode has made this PDEV the screen again, without enabling its surface anew. */
VOID
Fvm3dxDisplayPageAssert(PPDEV ppdev)
{
   FVM3DX_PAGE_MAPPING *Mapping = Fvm3dxFindPageMapping(ppdev);
   if (Mapping == NULL)
   {
      return;
   }
   Fvm3dxUsePageMapping(Mapping);
   DbgPortLineHex("qemudisp: fvm3dx display page back with mode index ", ppdev->ModeIndex);
}

/*
 * --- the pointer ---
 *
 * The driver draws the pointer itself: with the shape handed to GDI (EngSetPointerShape), XP moves its own pointer and never calls DrvMovePointer,
 * so the device would not learn where the pointer is. It is drawn into the frame buffer only, never into the shadow GDI
 * draws on: where it was is restored from the shadow, and after every copy of the shadow that touches it, it is drawn again. 32 bit screens
 * only; on others GDI keeps the pointer, and the device shows none of its own.
 */

/* The pointer in the two planes of Fvm3dxDisplayCursor, FVM3DX_CURSOR_SIDE_MAXIMUM to a row. */
static ULONG PointerBlendPixels[FVM3DX_CURSOR_PIXEL_COUNT];
static ULONG PointerXorPixels[FVM3DX_CURSOR_PIXEL_COUNT];
static LONG PointerWidth;
static LONG PointerHeight;
static LONG PointerHotSpotX;
static LONG PointerHotSpotY;
/* TRUE while the driver, not GDI, has the pointer. */
static BOOL PointerOwned;
static PPDEV PointerDevice;
/* Where the pointer is drawn in the frame buffer: its image's left top corner, and the part on the screen. */
static BOOL PointerDrawn;
static LONG PointerLeft;
static LONG PointerTop;
static RECTL PointerDrawnRectangle;

/* Turns a shape into the two planes; FALSE for a shape that does not fit or a format this file does not read. */
static BOOL
Fvm3dxConvertPointer(SURFOBJ *psoMask, SURFOBJ *psoColor, XLATEOBJ *pxlo, FLONG fl, LONG *WidthOut, LONG *HeightOut)
{
   BOOL WithAlpha = (fl & SPS_ALPHA) != 0 && psoColor != NULL && psoColor->iBitmapFormat == BMF_32BPP;
   SURFOBJ *SizeSource = (psoColor != NULL) ? psoColor : psoMask;
   LONG Width = (SizeSource != NULL) ? SizeSource->sizlBitmap.cx : 0;
   LONG Height = (SizeSource != NULL) ? SizeSource->sizlBitmap.cy : 0;
   if (psoColor == NULL && psoMask != NULL)
   {
      /* A monochrome pointer: the AND mask on top of the XOR mask. */
      Height = psoMask->sizlBitmap.cy / MONOCHROME_PLANES;
   }
   BOOL Fits = SizeSource != NULL && Width > 0 && Height > 0 && Width <= (LONG)FVM3DX_CURSOR_SIDE_MAXIMUM && Height <= (LONG)FVM3DX_CURSOR_SIDE_MAXIMUM;
   BOOL HasMask = psoMask != NULL && psoMask->iBitmapFormat == BMF_1BPP;
   if (!Fits || !(WithAlpha || HasMask))
   {
      return FALSE;
   }
   memset(PointerBlendPixels, 0, sizeof(PointerBlendPixels));
   memset(PointerXorPixels, 0, sizeof(PointerXorPixels));
   for (LONG Row = 0; Row < Height; Row++)
   {
      for (LONG Column = 0; Column < Width; Column++)
      {
         ULONG PixelIndex = (ULONG)Row * FVM3DX_CURSOR_SIDE_MAXIMUM + (ULONG)Column;
         ULONG BlendPixel = TRANSPARENT_PIXEL;
         ULONG XorPixel = TRANSPARENT_PIXEL;
         if (WithAlpha)
         {
            /* Per-pixel alpha comes premultiplied, as GDI's own alpha blend expects it. */
            Fvm3dxReadPixel(psoColor, Row, Column, &BlendPixel);
         }
         else if (psoColor != NULL)
         {
            ULONG AndBit = Fvm3dxMaskBit(psoMask, Row, Column);
            ULONG RawColor = 0;
            BOOL Read = Fvm3dxReadPixel(psoColor, Row, Column, &RawColor);
            if (!Read)
            {
               return FALSE;
            }
            BOOL Translate = pxlo != NULL && (pxlo->flXlate & XO_TRIVIAL) == 0;
            ULONG DeviceColor = Translate ? XLATEOBJ_iXlate(pxlo, RawColor) : RawColor;
            ULONG Rgb = Fvm3dxDeviceColorToRgb(DeviceColor);
            if (AndBit == 0)
            {
               BlendPixel = ALPHA_OPAQUE | Rgb;
            }
            else
            {
               XorPixel = Rgb;
            }
         }
         else
         {
            ULONG AndBit = Fvm3dxMaskBit(psoMask, Row, Column);
            ULONG XorBit = Fvm3dxMaskBit(psoMask, Row + Height, Column);
            if (AndBit == 0)
            {
               BlendPixel = (XorBit != 0) ? OPAQUE_WHITE : OPAQUE_BLACK;
            }
            else if (XorBit != 0)
            {
               XorPixel = INVERT_COLOR;
            }
         }
         PointerBlendPixels[PixelIndex] = BlendPixel;
         PointerXorPixels[PixelIndex] = XorPixel & RGB_MASK;
      }
   }
   *WidthOut = Width;
   *HeightOut = Height;
   return TRUE;
}

/* The pointer's position for the device, and with a new shape the shape as well. */
static VOID
Fvm3dxPublishPointer(LONG x, LONG y, BOOL WithShape)
{
   volatile Fvm3dxDisplayPage *Page = DisplayPage;
   if (Page == NULL)
   {
      return;
   }
   volatile Fvm3dxDisplayCursor *Cursor = &Page->cursor;
   Fvm3dxBeginWrite(&Cursor->sequence);
   if (WithShape)
   {
      for (ULONG PixelIndex = 0; PixelIndex < FVM3DX_CURSOR_PIXEL_COUNT; PixelIndex++)
      {
         Cursor->blendPixels[PixelIndex] = PointerBlendPixels[PixelIndex];
         Cursor->xorPixels[PixelIndex] = PointerXorPixels[PixelIndex];
      }
      Cursor->width = PointerOwned ? (uint32_t)PointerWidth : 0;
      Cursor->height = PointerOwned ? (uint32_t)PointerHeight : 0;
      Cursor->hotSpotX = PointerHotSpotX;
      Cursor->hotSpotY = PointerHotSpotY;
      Cursor->shapeSerial = Cursor->shapeSerial + 1;
   }
   Cursor->x = x;
   Cursor->y = y;
   Cursor->visible = (PointerOwned && x != HIDDEN_POSITION) ? 1 : 0;
   Fvm3dxEndWrite(&Cursor->sequence);
}

static ULONG
Fvm3dxBlendChannel(ULONG Destination, ULONG Source, ULONG InverseAlpha, ULONG Shift)
{
   ULONG DestinationChannel = (Destination >> Shift) & CHANNEL_MAXIMUM;
   ULONG SourceChannel = (Source >> Shift) & CHANNEL_MAXIMUM;
   ULONG Blended = SourceChannel + (DestinationChannel * InverseAlpha + CHANNEL_MAXIMUM / 2) / CHANNEL_MAXIMUM;
   ULONG Clamped = (Blended > CHANNEL_MAXIMUM) ? CHANNEL_MAXIMUM : Blended;
   return Clamped << Shift;
}

/* Draws the shadow's content with the pointer over it into the frame buffer, where Area and the drawn pointer overlap. */
static VOID
Fvm3dxCompositePointer(PPDEV ppdev, const RECTL *Area)
{
   LONG Left = (Area->left > PointerDrawnRectangle.left) ? Area->left : PointerDrawnRectangle.left;
   LONG Top = (Area->top > PointerDrawnRectangle.top) ? Area->top : PointerDrawnRectangle.top;
   LONG Right = (Area->right < PointerDrawnRectangle.right) ? Area->right : PointerDrawnRectangle.right;
   LONG Bottom = (Area->bottom < PointerDrawnRectangle.bottom) ? Area->bottom : PointerDrawnRectangle.bottom;
   for (LONG Row = Top; Row < Bottom; Row++)
   {
      const ULONG *ShadowRow = (const ULONG *)(ppdev->ShadowBits + (ULONG)Row * ppdev->ScreenDelta);
      ULONG *ScreenRow = (ULONG *)((PBYTE)ppdev->ScreenPtr + (ULONG)Row * ppdev->ScreenDelta);
      ULONG PointerRow = (ULONG)(Row - PointerTop) * FVM3DX_CURSOR_SIDE_MAXIMUM;
      for (LONG Column = Left; Column < Right; Column++)
      {
         ULONG PixelIndex = PointerRow + (ULONG)(Column - PointerLeft);
         ULONG Source = PointerBlendPixels[PixelIndex];
         ULONG Destination = ShadowRow[Column];
         ULONG InverseAlpha = CHANNEL_MAXIMUM - (Source >> ALPHA_SHIFT);
         ULONG Red = Fvm3dxBlendChannel(Destination, Source, InverseAlpha, RED_SHIFT);
         ULONG Green = Fvm3dxBlendChannel(Destination, Source, InverseAlpha, GREEN_SHIFT);
         ULONG Blue = Fvm3dxBlendChannel(Destination, Source, InverseAlpha, BLUE_SHIFT);
         ULONG Blended = (Red | Green | Blue) ^ PointerXorPixels[PixelIndex];
         ScreenRow[Column] = Blended | (Destination & ALPHA_OPAQUE);
      }
   }
}

/* Puts the shadow's content back where the pointer is drawn. */
static VOID
Fvm3dxErasePointer(PPDEV ppdev)
{
   if (!PointerDrawn)
   {
      return;
   }
   /* Cleared first, so the copy below does not draw the pointer again. */
   PointerDrawn = FALSE;
   IntFlushRectangle(ppdev, &PointerDrawnRectangle);
}

static VOID
Fvm3dxDrawPointer(PPDEV ppdev, LONG x, LONG y)
{
   PointerLeft = x - PointerHotSpotX;
   PointerTop = y - PointerHotSpotY;
   RECTL Visible;
   Visible.left = (PointerLeft > 0) ? PointerLeft : 0;
   Visible.top = (PointerTop > 0) ? PointerTop : 0;
   Visible.right = (PointerLeft + PointerWidth < (LONG)ppdev->ScreenWidth) ? PointerLeft + PointerWidth : (LONG)ppdev->ScreenWidth;
   Visible.bottom = (PointerTop + PointerHeight < (LONG)ppdev->ScreenHeight) ? PointerTop + PointerHeight : (LONG)ppdev->ScreenHeight;
   if (Visible.right <= Visible.left || Visible.bottom <= Visible.top)
   {
      return;
   }
   PointerDrawnRectangle = Visible;
   PointerDrawn = TRUE;
   Fvm3dxCompositePointer(ppdev, &Visible);
}

/* DrvSetPointerShape: SPS_ACCEPT_NOEXCLUDE when the driver draws the shape, SPS_DECLINE when GDI has to. */
ULONG
Fvm3dxSetPointerShape(SURFOBJ *pso, SURFOBJ *psoMask, SURFOBJ *psoColor, XLATEOBJ *pxlo, LONG xHot, LONG yHot, LONG x, LONG y, FLONG fl)
{
   PPDEV ppdev = (pso != NULL) ? (PPDEV)pso->dhpdev : NULL;
   if (ppdev != NULL && ppdev == PointerDevice)
   {
      Fvm3dxErasePointer(ppdev);
   }
   PointerDrawn = FALSE;
   PointerDevice = ppdev;
   BOOL Usable = ppdev != NULL && ppdev->BitsPerPixel == BITS_PER_PIXEL_32 && ppdev->ShadowBits != NULL && ppdev->ScreenPtr != NULL;
   LONG Width = 0;
   LONG Height = 0;
   BOOL Converted = Usable && Fvm3dxConvertPointer(psoMask, psoColor, pxlo, fl, &Width, &Height);
   PointerOwned = Converted;
   if (!Converted)
   {
      Fvm3dxPublishPointer(HIDDEN_POSITION, HIDDEN_POSITION, TRUE);
      DbgPortLine("qemudisp: fvm3dx pointer shape left to GDI");
      return SPS_DECLINE;
   }
   PointerWidth = Width;
   PointerHeight = Height;
   PointerHotSpotX = xHot;
   PointerHotSpotY = yHot;
   if (x != HIDDEN_POSITION)
   {
      Fvm3dxDrawPointer(ppdev, x, y);
   }
   Fvm3dxPublishPointer(x, y, TRUE);
   DbgPortLineHex("qemudisp: fvm3dx pointer shape taken, width ", (unsigned long)Width);
   return SPS_ACCEPT_NOEXCLUDE;
}

/* DrvMovePointer: FALSE if the pointer is GDI's, which then moves it itself. */
BOOL
Fvm3dxMovePointer(SURFOBJ *pso, LONG x, LONG y)
{
   PPDEV ppdev = (pso != NULL) ? (PPDEV)pso->dhpdev : NULL;
   if (!PointerOwned || ppdev == NULL || ppdev != PointerDevice)
   {
      return FALSE;
   }
   Fvm3dxErasePointer(ppdev);
   if (x != HIDDEN_POSITION)
   {
      Fvm3dxDrawPointer(ppdev, x, y);
   }
   Fvm3dxPublishPointer(x, y, FALSE);
   return TRUE;
}

/* From IntFlushRectangle: the shadow was just copied over Flushed, so the pointer is drawn there again. */
VOID
Fvm3dxPointerAfterFlush(PPDEV ppdev, const RECTL *Flushed)
{
   if (!PointerDrawn || ppdev != PointerDevice)
   {
      return;
   }
   Fvm3dxCompositePointer(ppdev, Flushed);
}

/* Writes the visible part of the window's client area into its entry. */
static VOID
Fvm3dxPublishRegion(WNDOBJ *pwo, volatile Fvm3dxDisplayWindow *Entry)
{
   Fvm3dxBeginWrite(&Entry->sequence);
   Entry->client.left = pwo->rclClient.left;
   Entry->client.top = pwo->rclClient.top;
   Entry->client.right = pwo->rclClient.right;
   Entry->client.bottom = pwo->rclClient.bottom;
   ULONG Count = 0;
   BOOL Complete = TRUE;
   if (pwo->coClient.iDComplexity == DC_TRIVIAL)
   {
      Entry->rectangles[0].left = pwo->rclClient.left;
      Entry->rectangles[0].top = pwo->rclClient.top;
      Entry->rectangles[0].right = pwo->rclClient.right;
      Entry->rectangles[0].bottom = pwo->rclClient.bottom;
      Count = 1;
   }
   else
   {
      const ULONG NoLimit = 0;
      RECTANGLE_BATCH Batch;
      BOOL More;
      WNDOBJ_cEnumStart(pwo, CT_RECTANGLES, CD_ANY, NoLimit);
      do
      {
         More = WNDOBJ_bEnum(pwo, sizeof(Batch), (ULONG *)&Batch);
         for (ULONG Index = 0; Index < Batch.Count; Index++)
         {
            const RECTL *Rectangle = &Batch.Rectangles[Index];
            BOOL Empty = Rectangle->right <= Rectangle->left || Rectangle->bottom <= Rectangle->top;
            if (Empty)
            {
               continue;
            }
            if (Count == FVM3DX_DISPLAY_RECTANGLE_MAXIMUM)
            {
               Complete = FALSE;
               continue;
            }
            Entry->rectangles[Count].left = Rectangle->left;
            Entry->rectangles[Count].top = Rectangle->top;
            Entry->rectangles[Count].right = Rectangle->right;
            Entry->rectangles[Count].bottom = Rectangle->bottom;
            Count++;
         }
      } while (More);
   }
   Entry->rectangleCount = Count;
   Entry->complete = Complete ? 1 : 0;
   Fvm3dxEndWrite(&Entry->sequence);
   DbgPortLineHex("qemudisp: fvm3dx window region, rectangles ", Count);
   DbgPortLineHex("qemudisp: fvm3dx window region, client right ", (ULONG)pwo->rclClient.right);
   DbgPortLineHex("qemudisp: fvm3dx window region, client bottom ", (ULONG)pwo->rclClient.bottom);
   DbgPortLineHex("qemudisp: fvm3dx window region, first right ", (ULONG)Entry->rectangles[0].right);
   DbgPortLineHex("qemudisp: fvm3dx window region, first bottom ", (ULONG)Entry->rectangles[0].bottom);
}

/* The WNDOBJ of every entry, kept so a window announced again after a mode change can get a new one. */
static WNDOBJ *WindowObjects[FVM3DX_DISPLAY_WINDOW_COUNT];

/* GDI's WNDOBJ callback. WOC_CHANGED comes without a window and only says that a batch of changes is complete. */
static VOID APIENTRY
Fvm3dxWindowChanged(WNDOBJ *pwo, FLONG fl)
{
   volatile Fvm3dxDisplayPage *Page = DisplayPage;
   if (pwo == NULL)
   {
      return;
   }
   if (Page == NULL)
   {
      DbgPortLineHex("qemudisp: fvm3dx window callback without a page, flags ", fl);
      return;
   }
   ULONG EntryNumber = (ULONG)(ULONG_PTR)pwo->pvConsumer;
   if (EntryNumber == 0 || EntryNumber > FVM3DX_DISPLAY_WINDOW_COUNT)
   {
      return;
   }
   volatile Fvm3dxDisplayWindow *Entry = &Page->windows[EntryNumber - 1];
   DbgPortLineHex("qemudisp: fvm3dx window callback, flags ", fl);
   if ((fl & WOC_DELETE) != 0)
   {
      WindowObjects[EntryNumber - 1] = NULL;
      Fvm3dxBeginWrite(&Entry->sequence);
      Entry->window = 0;
      Entry->rectangleCount = 0;
      Entry->complete = 0;
      Fvm3dxEndWrite(&Entry->sequence);
      return;
   }
   if ((fl & WOC_RGN_CLIENT) != 0)
   {
      Fvm3dxPublishRegion(pwo, Entry);
   }
}

/* Deletes the WNDOBJ of a window announced before and frees its entry. */
static VOID
Fvm3dxForgetWindow(volatile Fvm3dxDisplayPage *Page, uint32_t Window)
{
   for (ULONG Index = 0; Index < FVM3DX_DISPLAY_WINDOW_COUNT; Index++)
   {
      volatile Fvm3dxDisplayWindow *Entry = &Page->windows[Index];
      if (Entry->window != Window || WindowObjects[Index] == NULL)
      {
         continue;
      }
      WNDOBJ *pwo = WindowObjects[Index];
      WindowObjects[Index] = NULL;
      EngDeleteWnd(pwo);
      Fvm3dxBeginWrite(&Entry->sequence);
      Entry->window = 0;
      Entry->rectangleCount = 0;
      Entry->complete = 0;
      Fvm3dxEndWrite(&Entry->sequence);
      DbgPortLineHex("qemudisp: fvm3dx window renewed, window ", Window);
   }
}

/*
 * ExtEscape(WNDOBJ_SETUP) of the fvm3dx ICD: a WNDOBJ for the window, so GDI reports every change of its visible part.
 * With FVM3DX_WINDOW_SETUP_RENEW the window's WNDOBJ is made anew: after a mode change GDI keeps the visible part clipped to the old screen.
 */
ULONG
Fvm3dxSetUpWindow(SURFOBJ *pso, ULONG cjIn, PVOID pvIn)
{
   volatile Fvm3dxDisplayPage *Page = DisplayPage;
   const Fvm3dxWindowSetup *Setup = (const Fvm3dxWindowSetup *)pvIn;
   if (Page == NULL || pvIn == NULL || cjIn < sizeof(Fvm3dxWindowSetup) || Setup->magic != FVM3DX_DISPLAY_PAGE_MAGIC || Setup->window == 0)
   {
      DbgPortLine("qemudisp: fvm3dx WNDOBJ_SETUP refused");
      return FALSE;
   }
   if ((Setup->flags & FVM3DX_WINDOW_SETUP_RENEW) != 0)
   {
      Fvm3dxForgetWindow(Page, Setup->window);
   }
   ULONG FreeEntry = FVM3DX_DISPLAY_WINDOW_COUNT;
   for (ULONG Index = 0; Index < FVM3DX_DISPLAY_WINDOW_COUNT; Index++)
   {
      if (Page->windows[Index].window == 0)
      {
         FreeEntry = Index;
         break;
      }
   }
   if (FreeEntry == FVM3DX_DISPLAY_WINDOW_COUNT)
   {
      DbgPortLine("qemudisp: fvm3dx WNDOBJ_SETUP, no free entry");
      return FALSE;
   }
   const ULONG NoPixelFormat = 0;
   HWND Window = (HWND)(ULONG_PTR)Setup->window;
   WNDOBJ *pwo = EngCreateWnd(pso, Window, Fvm3dxWindowChanged, WO_RGN_CLIENT, NoPixelFormat);
   if (pwo == NULL)
   {
      DbgPortLine("qemudisp: fvm3dx EngCreateWnd failed");
      return FALSE;
   }
   if (pwo == (WNDOBJ *)-1)
   {
      /* The window has its WNDOBJ already, and with it an entry. */
      DbgPortLine("qemudisp: fvm3dx window set up before");
      return TRUE;
   }
   volatile Fvm3dxDisplayWindow *Entry = &Page->windows[FreeEntry];
   Fvm3dxBeginWrite(&Entry->sequence);
   Entry->window = Setup->window;
   Entry->rectangleCount = 0;
   Entry->complete = 0;
   Fvm3dxEndWrite(&Entry->sequence);
   ULONG EntryNumber = FreeEntry + 1;
   WindowObjects[FreeEntry] = pwo;
   WNDOBJ_vSetConsumer(pwo, (PVOID)(ULONG_PTR)EntryNumber);
   Fvm3dxPublishRegion(pwo, Entry);
   DbgPortLineHex("qemudisp: fvm3dx window set up, window ", Setup->window);
   return TRUE;
}
