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

#ifndef _FRAMEBUF_PCH_
#define _FRAMEBUF_PCH_

#include <stdarg.h>
#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
#include "dbgport.h"
#include <winioctl.h>
#include <ntddvdeo.h>

//#define EXPERIMENTAL_MOUSE_CURSOR_SUPPORT

typedef struct _PDEV
{
   HANDLE hDriver;
   HDEV hDevEng;
   HSURF hSurfEng;
   ULONG ModeIndex;
   ULONG ScreenWidth;
   ULONG ScreenHeight;
   ULONG ScreenDelta;
   BYTE BitsPerPixel;
   ULONG RedMask;
   ULONG GreenMask;
   ULONG BlueMask;
   BYTE PaletteShift;
   PVOID ScreenPtr;
   HPALETTE DefaultPalette;
   PALETTEENTRY *PaletteEntries;

   /* qemu-3dfx: the shadow GDI actually draws on. ScreenPtr stays the adapter's frame
    * buffer and is only ever written, never read, once the shadow is up. */
   ULONG BytesPerPixel;
   HSURF hsurfShadow;
   SURFOBJ *psoShadow;
   PBYTE ShadowBits;

   /* The area GDI announced through DrvSynchronizeSurface before drawing on the shadow by itself,
    * not copied over yet. Flushed on the next flush or timer event, display/accel.c. */
   RECTL PendingDirtyRectangle;
   BOOL HasPendingDirtyRectangle;

#ifdef EXPERIMENTAL_MOUSE_CURSOR_SUPPORT
   VIDEO_POINTER_ATTRIBUTES PointerAttributes;
   XLATEOBJ *PointerXlateObject;
   HSURF PointerColorSurface;
   HSURF PointerMaskSurface;
   HSURF PointerSaveSurface;
   POINTL PointerHotSpot;
#endif

   /* DirectX Support */
   DWORD iDitherFormat;
   ULONG MemHeight;
   ULONG MemWidth;
   DWORD dwHeap;
   VIDEOMEMORY* pvmList;
   BOOL bDDInitialized;
   DDPIXELFORMAT ddpfDisplay;
} PDEV, *PPDEV;

#define DEVICE_NAME	L"framebuf"
#define ALLOC_TAG	'FUBF'


LONG APIENTRY
DrvDescribePixelFormat(
   DHPDEV dhpdev,
   LONG iPixelFormat,
   ULONG cjpfd,
   PIXELFORMATDESCRIPTOR *ppfd);

BOOL APIENTRY
DrvSetPixelFormat(
   SURFOBJ *pso,
   LONG iPixelFormat,
   HWND hwnd);

BOOL APIENTRY
DrvSwapBuffers(
   SURFOBJ *pso,
   WNDOBJ *pwo);

/* qemu-3dfx: the shadow buffer and the drawing functions hooked for it, display/accel.c */

#define QEMUDISP_SHADOW_HOOKS (HOOK_BITBLT | HOOK_STRETCHBLT | HOOK_STRETCHBLTROP | \
                               HOOK_PLGBLT | HOOK_TEXTOUT | HOOK_PAINT | \
                               HOOK_STROKEPATH | HOOK_FILLPATH | HOOK_STROKEANDFILLPATH | \
                               HOOK_LINETO | HOOK_COPYBITS | HOOK_TRANSPARENTBLT | \
                               HOOK_ALPHABLEND | HOOK_GRADIENTFILL | HOOK_SYNCHRONIZE)

VOID
IntRegisterScreenSurface(
   HSURF hsurf,
   PPDEV ppdev);

VOID
IntUnregisterScreenSurface(
   HSURF hsurf);

VOID
IntFlushRectangle(
   PPDEV ppdev,
   const RECTL *pDirtyRectangle);

VOID
IntFlushWholeScreen(
   PPDEV ppdev);

VOID APIENTRY
DrvSynchronizeSurface(
   SURFOBJ *pso,
   RECTL *prcl,
   FLONG fl);

BOOL APIENTRY
DrvBitBlt(
   SURFOBJ *psoTrg,
   SURFOBJ *psoSrc,
   SURFOBJ *psoMask,
   CLIPOBJ *pco,
   XLATEOBJ *pxlo,
   RECTL *prclTrg,
   POINTL *pptlSrc,
   POINTL *pptlMask,
   BRUSHOBJ *pbo,
   POINTL *pptlBrush,
   ROP4 rop4);

BOOL APIENTRY
DrvCopyBits(
   SURFOBJ *psoDest,
   SURFOBJ *psoSrc,
   CLIPOBJ *pco,
   XLATEOBJ *pxlo,
   RECTL *prclDest,
   POINTL *pptlSrc);

BOOL APIENTRY
DrvStretchBlt(
   SURFOBJ *psoDest,
   SURFOBJ *psoSrc,
   SURFOBJ *psoMask,
   CLIPOBJ *pco,
   XLATEOBJ *pxlo,
   COLORADJUSTMENT *pca,
   POINTL *pptlHTOrg,
   RECTL *prclDest,
   RECTL *prclSrc,
   POINTL *pptlMask,
   ULONG iMode);

BOOL APIENTRY
DrvStretchBltROP(
   SURFOBJ *psoDest,
   SURFOBJ *psoSrc,
   SURFOBJ *psoMask,
   CLIPOBJ *pco,
   XLATEOBJ *pxlo,
   COLORADJUSTMENT *pca,
   POINTL *pptlHTOrg,
   RECTL *prclDest,
   RECTL *prclSrc,
   POINTL *pptlMask,
   ULONG iMode,
   BRUSHOBJ *pbo,
   DWORD rop4);

BOOL APIENTRY
DrvPlgBlt(
   SURFOBJ *psoTrg,
   SURFOBJ *psoSrc,
   SURFOBJ *psoMsk,
   CLIPOBJ *pco,
   XLATEOBJ *pxlo,
   COLORADJUSTMENT *pca,
   POINTL *pptlBrushOrg,
   POINTFIX *pptfx,
   RECTL *prcl,
   POINTL *pptl,
   ULONG iMode);

BOOL APIENTRY
DrvTextOut(
   SURFOBJ *pso,
   STROBJ *pstro,
   FONTOBJ *pfo,
   CLIPOBJ *pco,
   RECTL *prclExtra,
   RECTL *prclOpaque,
   BRUSHOBJ *pboFore,
   BRUSHOBJ *pboOpaque,
   POINTL *pptlOrg,
   MIX mix);

BOOL APIENTRY
DrvPaint(
   SURFOBJ *pso,
   CLIPOBJ *pco,
   BRUSHOBJ *pbo,
   POINTL *pptlBrushOrg,
   MIX mix);

BOOL APIENTRY
DrvLineTo(
   SURFOBJ *pso,
   CLIPOBJ *pco,
   BRUSHOBJ *pbo,
   LONG x1,
   LONG y1,
   LONG x2,
   LONG y2,
   RECTL *prclBounds,
   MIX mix);

BOOL APIENTRY
DrvStrokePath(
   SURFOBJ *pso,
   PATHOBJ *ppo,
   CLIPOBJ *pco,
   XFORMOBJ *pxo,
   BRUSHOBJ *pbo,
   POINTL *pptlBrushOrg,
   LINEATTRS *plineattrs,
   MIX mix);

BOOL APIENTRY
DrvFillPath(
   SURFOBJ *pso,
   PATHOBJ *ppo,
   CLIPOBJ *pco,
   BRUSHOBJ *pbo,
   POINTL *pptlBrushOrg,
   MIX mix,
   FLONG flOptions);

BOOL APIENTRY
DrvStrokeAndFillPath(
   SURFOBJ *pso,
   PATHOBJ *ppo,
   CLIPOBJ *pco,
   XFORMOBJ *pxo,
   BRUSHOBJ *pboStroke,
   LINEATTRS *plineattrs,
   BRUSHOBJ *pboFill,
   POINTL *pptlBrushOrg,
   MIX mixFill,
   FLONG flOptions);

BOOL APIENTRY
DrvTransparentBlt(
   SURFOBJ *psoDst,
   SURFOBJ *psoSrc,
   CLIPOBJ *pco,
   XLATEOBJ *pxlo,
   RECTL *prclDst,
   RECTL *prclSrc,
   ULONG iTransColor,
   ULONG ulReserved);

BOOL APIENTRY
DrvAlphaBlend(
   SURFOBJ *psoDest,
   SURFOBJ *psoSrc,
   CLIPOBJ *pco,
   XLATEOBJ *pxlo,
   RECTL *prclDest,
   RECTL *prclSrc,
   BLENDOBJ *pBlendObj);

BOOL APIENTRY
DrvGradientFill(
   SURFOBJ *psoDest,
   CLIPOBJ *pco,
   XLATEOBJ *pxlo,
   TRIVERTEX *pVertex,
   ULONG nVertex,
   PVOID pMesh,
   ULONG nMesh,
   RECTL *prclExtents,
   POINTL *pptlDitherOrg,
   ULONG ulMode);

ULONG APIENTRY
DrvEscape(
   SURFOBJ *pso,
   ULONG iEsc,
   ULONG cjIn,
   PVOID pvIn,
   ULONG cjOut,
   PVOID pvOut);

BOOL APIENTRY
DrvEnableDirectDraw(
    DHPDEV dhpdev,
    DD_CALLBACKS *pCallbacks,
    DD_SURFACECALLBACKS *pSurfaceCallbacks,
    DD_PALETTECALLBACKS *pPaletteCallbacks);

VOID APIENTRY
DrvDisableDirectDraw(
    DHPDEV dhpdev);

DHPDEV APIENTRY
DrvEnablePDEV(
   IN DEVMODEW *pdm,
   IN LPWSTR pwszLogAddress,
   IN ULONG cPat,
   OUT HSURF *phsurfPatterns,
   IN ULONG cjCaps,
   OUT ULONG *pdevcaps,
   IN ULONG cjDevInfo,
   OUT DEVINFO *pdi,
   IN HDEV hdev,
   IN LPWSTR pwszDeviceName,
   IN HANDLE hDriver);

VOID APIENTRY
DrvCompletePDEV(
   IN DHPDEV dhpdev,
   IN HDEV hdev);

VOID APIENTRY
DrvDisablePDEV(
   IN DHPDEV dhpdev);

HSURF APIENTRY
DrvEnableSurface(
   IN DHPDEV dhpdev);

VOID APIENTRY
DrvDisableSurface(
   IN DHPDEV dhpdev);

BOOL APIENTRY
DrvAssertMode(
   IN DHPDEV dhpdev,
   IN BOOL bEnable);

ULONG APIENTRY
DrvGetModes(
   IN HANDLE hDriver,
   IN ULONG cjSize,
   OUT DEVMODEW *pdm);

BOOL APIENTRY
DrvSetPalette(
   IN DHPDEV dhpdev,
   IN PALOBJ *ppalo,
   IN FLONG fl,
   IN ULONG iStart,
   IN ULONG cColors);

ULONG APIENTRY
DrvSetPointerShape(
   IN SURFOBJ *pso,
   IN SURFOBJ *psoMask,
   IN SURFOBJ *psoColor,
   IN XLATEOBJ *pxlo,
   IN LONG xHot,
   IN LONG yHot,
   IN LONG x,
   IN LONG y,
   IN RECTL *prcl,
   IN FLONG fl);

VOID APIENTRY
DrvMovePointer(
   IN SURFOBJ *pso,
   IN LONG x,
   IN LONG y,
   IN RECTL *prcl);

BOOL
IntInitScreenInfo(
   PPDEV ppdev,
   LPDEVMODEW pDevMode,
   PGDIINFO pGdiInfo,
   PDEVINFO pDevInfo);

BOOL
IntInitDefaultPalette(
   PPDEV ppdev,
   PDEVINFO pDevInfo);

BOOL APIENTRY
IntSetPalette(
   IN DHPDEV dhpdev,
   IN PPALETTEENTRY ppalent,
   IN ULONG iStart,
   IN ULONG cColors);

#endif /* _FRAMEBUF_PCH_ */
