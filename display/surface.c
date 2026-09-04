/*
 * ReactOS Generic Framebuffer display driver
 *
 * Copyright (C) 2004 Filip Navara
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "framebuf.h"

/*
 * DrvEnableSurface
 *
 * Create engine bitmap around frame buffer and set the video mode requested
 * when PDEV was initialized.
 *
 * Status
 *    @implemented
 */

HSURF APIENTRY
DrvEnableSurface(
   IN DHPDEV dhpdev)
{
   PPDEV ppdev = (PPDEV)dhpdev;
   HSURF hSurface;
   ULONG BitmapType;
   SIZEL ScreenSize;
   VIDEO_MEMORY VideoMemory;
   VIDEO_MEMORY_INFORMATION VideoMemoryInfo;
   ULONG ulTemp;

   /*
    * Set video mode of our adapter.
    */

   DbgPortLineHex("qemudisp: DrvEnableSurface, mode index ", ppdev->ModeIndex);

   if (EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_SET_CURRENT_MODE,
                          &(ppdev->ModeIndex), sizeof(ULONG), NULL, 0,
                          &ulTemp))
   {
      DbgPortLine("qemudisp: IOCTL_VIDEO_SET_CURRENT_MODE failed");
      return NULL;
   }

   /*
    * Map the framebuffer into our memory.
    */

   VideoMemory.RequestedVirtualAddress = NULL;
   if (EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_MAP_VIDEO_MEMORY,
                          &VideoMemory, sizeof(VIDEO_MEMORY),
                          &VideoMemoryInfo, sizeof(VIDEO_MEMORY_INFORMATION),
                          &ulTemp))
   {
      DbgPortLine("qemudisp: IOCTL_VIDEO_MAP_VIDEO_MEMORY failed");
      return NULL;
   }

   ppdev->ScreenPtr = VideoMemoryInfo.FrameBufferBase;

   DbgPortLineHex("qemudisp: framebuffer at ", (unsigned long)ppdev->ScreenPtr);
   DbgPortLineHex("qemudisp: framebuffer length ", VideoMemoryInfo.FrameBufferLength);
   DbgPortLineHex("qemudisp: bits per pixel ", ppdev->BitsPerPixel);
   DbgPortLineHex("qemudisp: screen width ", ppdev->ScreenWidth);
   DbgPortLineHex("qemudisp: screen height ", ppdev->ScreenHeight);
   DbgPortLineHex("qemudisp: screen delta ", ppdev->ScreenDelta);

   switch (ppdev->BitsPerPixel)
   {
      case 8:
         IntSetPalette(dhpdev, ppdev->PaletteEntries, 0, 256);
         BitmapType = BMF_8BPP;
         break;

      case 16:
         BitmapType = BMF_16BPP;
         break;

      case 24:
         BitmapType = BMF_24BPP;
         break;

      case 32:
         BitmapType = BMF_32BPP;
         break;

      default:
         DbgPortLineHex("qemudisp: unsupported depth ", ppdev->BitsPerPixel);
         return NULL;
   }

   ppdev->iDitherFormat = BitmapType;
   ppdev->BytesPerPixel = ppdev->BitsPerPixel / 8;

   ScreenSize.cx = ppdev->ScreenWidth;
   ScreenSize.cy = ppdev->ScreenHeight;

   /* The surface GDI draws on is a bitmap in system memory, not the frame buffer. Passing
    * NULL for the bits lets GDI allocate them; the stride is kept identical to the
    * adapter's so that copying a rectangle over is a row-for-row memcpy.
    *
    * The frame buffer is deliberately not handed to GDI any more. It is written to in
    * IntFlushRectangle and nowhere else -- reading it back costs 46 MB/s, see accel.c. */
   hSurface = (HSURF)EngCreateBitmap(ScreenSize, ppdev->ScreenDelta, BitmapType,
                                     (ppdev->ScreenDelta > 0) ? BMF_TOPDOWN : 0,
                                     NULL);
   if (hSurface == NULL)
   {
      DbgPortLine("qemudisp: EngCreateBitmap failed");
      return NULL;
   }

   ppdev->psoShadow = EngLockSurface(hSurface);
   if (ppdev->psoShadow == NULL)
   {
      DbgPortLine("qemudisp: EngLockSurface failed");
      EngDeleteSurface(hSurface);
      return NULL;
   }
   ppdev->ShadowBits = (PBYTE)ppdev->psoShadow->pvBits;
   ppdev->hsurfShadow = hSurface;
   DbgPortLineHex("qemudisp: shadow buffer at ", (unsigned long)ppdev->ShadowBits);

   /*
    * Associate the surface with our device, and hook every drawing function of the DDI:
    * anything left unhooked would let GDI change the shadow without the driver noticing,
    * and the change would never reach the screen.
    */

   if (!EngAssociateSurface(hSurface, ppdev->hDevEng, QEMUDISP_SHADOW_HOOKS))
   {
      DbgPortLine("qemudisp: EngAssociateSurface failed");
      EngUnlockSurface(ppdev->psoShadow);
      ppdev->psoShadow = NULL;
      ppdev->ShadowBits = NULL;
      ppdev->hsurfShadow = NULL;
      EngDeleteSurface(hSurface);
      return NULL;
   }

   ppdev->hSurfEng = hSurface;
   IntRegisterScreenSurface(hSurface, ppdev);

   /* GDI hands out a zeroed bitmap, the adapter shows whatever was left in its memory.
    * One copy puts the two in step before the first drawing call arrives. */
   IntFlushWholeScreen(ppdev);

   DbgPortLine("qemudisp: DrvEnableSurface ok");
   return hSurface;
}

/*
 * DrvDisableSurface
 *
 * Used by GDI to notify a driver that the surface created by DrvEnableSurface
 * for the current device is no longer needed.
 *
 * Status
 *    @implemented
 */

VOID APIENTRY
DrvDisableSurface(
   IN DHPDEV dhpdev)
{
   DWORD ulTemp;
   VIDEO_MEMORY VideoMemory;
   PPDEV ppdev = (PPDEV)dhpdev;

   IntUnregisterScreenSurface(ppdev->hSurfEng);
   if (ppdev->psoShadow != NULL)
   {
      EngUnlockSurface(ppdev->psoShadow);
      ppdev->psoShadow = NULL;
   }
   ppdev->ShadowBits = NULL;
   ppdev->hsurfShadow = NULL;

   EngDeleteSurface(ppdev->hSurfEng);
   ppdev->hSurfEng = NULL;

#ifdef EXPERIMENTAL_MOUSE_CURSOR_SUPPORT
   /* Clear all mouse pointer surfaces. */
   DrvSetPointerShape(NULL, NULL, NULL, NULL, 0, 0, 0, 0, NULL, 0);
#endif

   /*
    * Unmap the framebuffer.
    */

   VideoMemory.RequestedVirtualAddress = ((PPDEV)dhpdev)->ScreenPtr;
   EngDeviceIoControl(((PPDEV)dhpdev)->hDriver, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY,
                      &VideoMemory, sizeof(VIDEO_MEMORY), NULL, 0, &ulTemp);
}

/*
 * DrvAssertMode
 *
 * Sets the mode of the specified physical device to either the mode specified
 * when the PDEV was initialized or to the default mode of the hardware.
 *
 * Status
 *    @implemented
 */

BOOL APIENTRY
DrvAssertMode(
   IN DHPDEV dhpdev,
   IN BOOL bEnable)
{
   PPDEV ppdev = (PPDEV)dhpdev;
   ULONG ulTemp;

   if (bEnable)
   {
      /*
       * Reinitialize the device to a clean state.
       */
      if (EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_SET_CURRENT_MODE,
                             &(ppdev->ModeIndex), sizeof(ULONG), NULL, 0,
                             &ulTemp))
      {
          /* We failed, bail out */
          return FALSE;
      }
      if (ppdev->BitsPerPixel == 8)
      {
	     IntSetPalette(dhpdev, ppdev->PaletteEntries, 0, 256);
      }

      /* Coming back from a full screen DOS box or another desktop, the adapter's memory
       * holds whatever that left behind. The shadow still has the truth. */
      IntFlushWholeScreen(ppdev);

      return TRUE;
   }
   else
   {
      /*
       * Call the miniport driver to reset the device to a known state.
       */
      return !EngDeviceIoControl(ppdev->hDriver, IOCTL_VIDEO_RESET_DEVICE,
                                 NULL, 0, NULL, 0, &ulTemp);
   }
}
