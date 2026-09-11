/*
 * Shadow framebuffer for the QEMU standard VGA.
 *
 * Copyright (C) 2026 qemu-3dfx-build
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

/* GDI does not draw into the adapter's frame buffer any more, it draws into a bitmap in
 * ordinary system memory. Every drawing function of the DDI is hooked, handed straight
 * back to GDI, and the rectangle it touched is then copied over in one go.
 *
 * The reason is one measurement: on this adapter the frame buffer costs 41 MB/s to write
 * and 46 MB/s to read (docs/LOG.md). Mapping it write-combined fixed the writes; reads
 * stay expensive, because that is what write-combining is. Anything GDI does that reads
 * the destination -- scrolling a window, alpha blending, the pointer's save-under -- is
 * therefore still paid at 46 MB/s, and that is the whole of the complaint. A shadow in
 * cached memory turns those reads into ordinary RAM reads and leaves a single linear
 * write towards the adapter.
 *
 * Nothing here accelerates drawing itself. GDI is as fast as it ever was; what changes is
 * the memory it works on.
 */

#include "framebuf.h"

/* Which PDEV a SURFOBJ belongs to. SURFOBJ.dhpdev is documented for device-managed
 * surfaces, and ours is a GDI-managed bitmap, so it is looked up here instead of
 * trusted. One entry per display device; XP with two adapters uses two. */
#define SCREEN_SURFACE_SLOTS 4

typedef struct _SCREEN_SURFACE_SLOT
{
   HSURF hsurf;
   PPDEV ppdev;
} SCREEN_SURFACE_SLOT;

static SCREEN_SURFACE_SLOT ScreenSurfaceSlots[SCREEN_SURFACE_SLOTS];

VOID
IntRegisterScreenSurface(HSURF hsurf, PPDEV ppdev)
{
   ULONG slotIndex;

   for (slotIndex = 0; slotIndex < SCREEN_SURFACE_SLOTS; slotIndex++)
   {
      if (ScreenSurfaceSlots[slotIndex].hsurf == NULL)
      {
         ScreenSurfaceSlots[slotIndex].hsurf = hsurf;
         ScreenSurfaceSlots[slotIndex].ppdev = ppdev;
         return;
      }
   }
   DbgPortLine("qemudisp: no free screen surface slot");
}

VOID
IntUnregisterScreenSurface(HSURF hsurf)
{
   ULONG slotIndex;

   for (slotIndex = 0; slotIndex < SCREEN_SURFACE_SLOTS; slotIndex++)
   {
      if (ScreenSurfaceSlots[slotIndex].hsurf == hsurf)
      {
         ScreenSurfaceSlots[slotIndex].hsurf = NULL;
         ScreenSurfaceSlots[slotIndex].ppdev = NULL;
         return;
      }
   }
}

static PPDEV
ScreenDeviceForSurface(SURFOBJ *pso)
{
   ULONG slotIndex;

   if (pso == NULL)
   {
      return NULL;
   }
   for (slotIndex = 0; slotIndex < SCREEN_SURFACE_SLOTS; slotIndex++)
   {
      if (ScreenSurfaceSlots[slotIndex].hsurf == pso->hsurf)
      {
         return ScreenSurfaceSlots[slotIndex].ppdev;
      }
   }
   return NULL;
}

#ifdef QEMU_PASSTHROUGH_PROBE
/* The probe escape needs the PDEV for the miniport handle, and DrvEscape only gets a SURFOBJ. */
PPDEV
IntScreenDeviceForSurface(SURFOBJ *pso)
{
   return ScreenDeviceForSurface(pso);
}
#endif

/*
 * IntFlushRectangle
 *
 * Copies one rectangle of the shadow into the adapter's frame buffer. This is the only
 * place in the driver that writes to video memory during normal drawing.
 */

VOID
IntFlushRectangle(PPDEV ppdev, const RECTL *pDirtyRectangle)
{
   RECTL clipped;
   LONG rowIndex;
   ULONG bytesPerRow;
   PBYTE sourceRow;
   PBYTE targetRow;

   if (ppdev == NULL || ppdev->ShadowBits == NULL || ppdev->ScreenPtr == NULL)
   {
      return;
   }

   clipped = *pDirtyRectangle;
   if (clipped.left < 0)
   {
      clipped.left = 0;
   }
   if (clipped.top < 0)
   {
      clipped.top = 0;
   }
   if (clipped.right > (LONG)ppdev->ScreenWidth)
   {
      clipped.right = (LONG)ppdev->ScreenWidth;
   }
   if (clipped.bottom > (LONG)ppdev->ScreenHeight)
   {
      clipped.bottom = (LONG)ppdev->ScreenHeight;
   }
   if (clipped.right <= clipped.left || clipped.bottom <= clipped.top)
   {
      return;
   }

   bytesPerRow = (ULONG)(clipped.right - clipped.left) * ppdev->BytesPerPixel;
   sourceRow = ppdev->ShadowBits + (ULONG)clipped.top * ppdev->ScreenDelta + (ULONG)clipped.left * ppdev->BytesPerPixel;
   targetRow = (PBYTE)ppdev->ScreenPtr + (ULONG)clipped.top * ppdev->ScreenDelta + (ULONG)clipped.left * ppdev->BytesPerPixel;

   for (rowIndex = clipped.top; rowIndex < clipped.bottom; rowIndex++)
   {
      memcpy(targetRow, sourceRow, bytesPerRow);
      sourceRow += ppdev->ScreenDelta;
      targetRow += ppdev->ScreenDelta;
   }
}

VOID
IntFlushWholeScreen(PPDEV ppdev)
{
   RECTL wholeScreen;

   if (ppdev == NULL)
   {
      return;
   }
   wholeScreen.left = 0;
   wholeScreen.top = 0;
   wholeScreen.right = (LONG)ppdev->ScreenWidth;
   wholeScreen.bottom = (LONG)ppdev->ScreenHeight;
   IntFlushRectangle(ppdev, &wholeScreen);
}

/* The bounding rectangle to copy back. Where the DDI hands one over it is used as it is;
 * where it does not, the clip object bounds it, and without a clip object the operation
 * may touch the whole surface. Over-copying costs write bandwidth, which after the
 * write-combined mapping is the cheap half; under-copying leaves stale pixels on screen. */
static VOID
BoundsFromClipObject(PPDEV ppdev, CLIPOBJ *pco, RECTL *pBoundsOut)
{
   if (pco != NULL)
   {
      *pBoundsOut = pco->rclBounds;
      return;
   }
   pBoundsOut->left = 0;
   pBoundsOut->top = 0;
   pBoundsOut->right = (LONG)ppdev->ScreenWidth;
   pBoundsOut->bottom = (LONG)ppdev->ScreenHeight;
}

static VOID
IntersectRectangle(RECTL *pTarget, const RECTL *pOther)
{
   if (pOther->left > pTarget->left)
   {
      pTarget->left = pOther->left;
   }
   if (pOther->top > pTarget->top)
   {
      pTarget->top = pOther->top;
   }
   if (pOther->right < pTarget->right)
   {
      pTarget->right = pOther->right;
   }
   if (pOther->bottom < pTarget->bottom)
   {
      pTarget->bottom = pOther->bottom;
   }
}

static VOID
UnionRectangle(RECTL *pTarget, const RECTL *pOther)
{
   if (pOther->right <= pOther->left || pOther->bottom <= pOther->top)
   {
      return;
   }
   if (pTarget->right <= pTarget->left || pTarget->bottom <= pTarget->top)
   {
      *pTarget = *pOther;
      return;
   }
   if (pOther->left < pTarget->left)
   {
      pTarget->left = pOther->left;
   }
   if (pOther->top < pTarget->top)
   {
      pTarget->top = pOther->top;
   }
   if (pOther->right > pTarget->right)
   {
      pTarget->right = pOther->right;
   }
   if (pOther->bottom > pTarget->bottom)
   {
      pTarget->bottom = pOther->bottom;
   }
}

/*
 * The hooked drawing functions. Each one hands the call straight back to GDI -- which is
 * allowed to do the work on a GDI-managed bitmap and does not call back into the driver --
 * and then copies over what changed.
 */

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
   ROP4 rop4)
{
   PPDEV ppdev = ScreenDeviceForSurface(psoTrg);
   BOOL drawn = EngBitBlt(psoTrg, psoSrc, psoMask, pco, pxlo, prclTrg, pptlSrc, pptlMask, pbo, pptlBrush, rop4);

   if (drawn && ppdev != NULL && prclTrg != NULL)
   {
      IntFlushRectangle(ppdev, prclTrg);
   }
   return drawn;
}

BOOL APIENTRY
DrvCopyBits(
   SURFOBJ *psoDest,
   SURFOBJ *psoSrc,
   CLIPOBJ *pco,
   XLATEOBJ *pxlo,
   RECTL *prclDest,
   POINTL *pptlSrc)
{
   PPDEV ppdev = ScreenDeviceForSurface(psoDest);
   BOOL drawn = EngCopyBits(psoDest, psoSrc, pco, pxlo, prclDest, pptlSrc);

   if (drawn && ppdev != NULL && prclDest != NULL)
   {
      IntFlushRectangle(ppdev, prclDest);
   }
   return drawn;
}

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
   ULONG iMode)
{
   PPDEV ppdev = ScreenDeviceForSurface(psoDest);
   BOOL drawn = EngStretchBlt(psoDest, psoSrc, psoMask, pco, pxlo, pca, pptlHTOrg, prclDest, prclSrc, pptlMask, iMode);

   if (drawn && ppdev != NULL && prclDest != NULL)
   {
      IntFlushRectangle(ppdev, prclDest);
   }
   return drawn;
}

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
   DWORD rop4)
{
   PPDEV ppdev = ScreenDeviceForSurface(psoDest);
   BOOL drawn = EngStretchBltROP(psoDest, psoSrc, psoMask, pco, pxlo, pca, pptlHTOrg, prclDest, prclSrc, pptlMask, iMode, pbo, rop4);

   if (drawn && ppdev != NULL && prclDest != NULL)
   {
      IntFlushRectangle(ppdev, prclDest);
   }
   return drawn;
}

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
   ULONG iMode)
{
   PPDEV ppdev = ScreenDeviceForSurface(psoTrg);
   BOOL drawn = EngPlgBlt(psoTrg, psoSrc, psoMsk, pco, pxlo, pca, pptlBrushOrg, pptfx, prcl, pptl, iMode);

   if (drawn && ppdev != NULL)
   {
      /* prcl bounds the source, not the target -- the target is the parallelogram in
       * pptfx. The clip object is what GDI guarantees it stayed inside. */
      RECTL dirtyBounds;
      BoundsFromClipObject(ppdev, pco, &dirtyBounds);
      IntFlushRectangle(ppdev, &dirtyBounds);
   }
   return drawn;
}

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
   MIX mix)
{
   PPDEV ppdev = ScreenDeviceForSurface(pso);
   BOOL drawn = EngTextOut(pso, pstro, pfo, pco, prclExtra, prclOpaque, pboFore, pboOpaque, pptlOrg, mix);

   if (drawn && ppdev != NULL)
   {
      RECTL dirtyBounds = pstro->rclBkGround;

      if (prclOpaque != NULL)
      {
         UnionRectangle(&dirtyBounds, prclOpaque);
      }
      if (prclExtra != NULL)
      {
         /* An array of underline and strikeout rectangles, ended by an empty one. */
         RECTL *extraRectangle = prclExtra;
         while (extraRectangle->right > extraRectangle->left || extraRectangle->bottom > extraRectangle->top)
         {
            UnionRectangle(&dirtyBounds, extraRectangle);
            extraRectangle++;
         }
      }
      if (pco != NULL)
      {
         IntersectRectangle(&dirtyBounds, &pco->rclBounds);
      }
      IntFlushRectangle(ppdev, &dirtyBounds);
   }
   return drawn;
}

BOOL APIENTRY
DrvPaint(
   SURFOBJ *pso,
   CLIPOBJ *pco,
   BRUSHOBJ *pbo,
   POINTL *pptlBrushOrg,
   MIX mix)
{
   PPDEV ppdev = ScreenDeviceForSurface(pso);
   BOOL drawn = EngPaint(pso, pco, pbo, pptlBrushOrg, mix);

   if (drawn && ppdev != NULL)
   {
      RECTL dirtyBounds;
      BoundsFromClipObject(ppdev, pco, &dirtyBounds);
      IntFlushRectangle(ppdev, &dirtyBounds);
   }
   return drawn;
}

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
   MIX mix)
{
   PPDEV ppdev = ScreenDeviceForSurface(pso);
   BOOL drawn = EngLineTo(pso, pco, pbo, x1, y1, x2, y2, prclBounds, mix);

   if (drawn && ppdev != NULL && prclBounds != NULL)
   {
      IntFlushRectangle(ppdev, prclBounds);
   }
   return drawn;
}

BOOL APIENTRY
DrvStrokePath(
   SURFOBJ *pso,
   PATHOBJ *ppo,
   CLIPOBJ *pco,
   XFORMOBJ *pxo,
   BRUSHOBJ *pbo,
   POINTL *pptlBrushOrg,
   LINEATTRS *plineattrs,
   MIX mix)
{
   PPDEV ppdev = ScreenDeviceForSurface(pso);
   BOOL drawn = EngStrokePath(pso, ppo, pco, pxo, pbo, pptlBrushOrg, plineattrs, mix);

   if (drawn && ppdev != NULL)
   {
      RECTL dirtyBounds;
      BoundsFromClipObject(ppdev, pco, &dirtyBounds);
      IntFlushRectangle(ppdev, &dirtyBounds);
   }
   return drawn;
}

BOOL APIENTRY
DrvFillPath(
   SURFOBJ *pso,
   PATHOBJ *ppo,
   CLIPOBJ *pco,
   BRUSHOBJ *pbo,
   POINTL *pptlBrushOrg,
   MIX mix,
   FLONG flOptions)
{
   PPDEV ppdev = ScreenDeviceForSurface(pso);
   BOOL drawn = EngFillPath(pso, ppo, pco, pbo, pptlBrushOrg, mix, flOptions);

   if (drawn && ppdev != NULL)
   {
      RECTL dirtyBounds;
      BoundsFromClipObject(ppdev, pco, &dirtyBounds);
      IntFlushRectangle(ppdev, &dirtyBounds);
   }
   return drawn;
}

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
   FLONG flOptions)
{
   PPDEV ppdev = ScreenDeviceForSurface(pso);
   BOOL drawn = EngStrokeAndFillPath(pso, ppo, pco, pxo, pboStroke, plineattrs, pboFill, pptlBrushOrg, mixFill, flOptions);

   if (drawn && ppdev != NULL)
   {
      RECTL dirtyBounds;
      BoundsFromClipObject(ppdev, pco, &dirtyBounds);
      IntFlushRectangle(ppdev, &dirtyBounds);
   }
   return drawn;
}

BOOL APIENTRY
DrvTransparentBlt(
   SURFOBJ *psoDst,
   SURFOBJ *psoSrc,
   CLIPOBJ *pco,
   XLATEOBJ *pxlo,
   RECTL *prclDst,
   RECTL *prclSrc,
   ULONG iTransColor,
   ULONG ulReserved)
{
   PPDEV ppdev = ScreenDeviceForSurface(psoDst);
   BOOL drawn = EngTransparentBlt(psoDst, psoSrc, pco, pxlo, prclDst, prclSrc, iTransColor, ulReserved);

   if (drawn && ppdev != NULL && prclDst != NULL)
   {
      IntFlushRectangle(ppdev, prclDst);
   }
   return drawn;
}

BOOL APIENTRY
DrvAlphaBlend(
   SURFOBJ *psoDest,
   SURFOBJ *psoSrc,
   CLIPOBJ *pco,
   XLATEOBJ *pxlo,
   RECTL *prclDest,
   RECTL *prclSrc,
   BLENDOBJ *pBlendObj)
{
   PPDEV ppdev = ScreenDeviceForSurface(psoDest);
   BOOL drawn = EngAlphaBlend(psoDest, psoSrc, pco, pxlo, prclDest, prclSrc, pBlendObj);

   if (drawn && ppdev != NULL && prclDest != NULL)
   {
      IntFlushRectangle(ppdev, prclDest);
   }
   return drawn;
}

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
   ULONG ulMode)
{
   PPDEV ppdev = ScreenDeviceForSurface(psoDest);
   BOOL drawn = EngGradientFill(psoDest, pco, pxlo, pVertex, nVertex, pMesh, nMesh, prclExtents, pptlDitherOrg, ulMode);

   if (drawn && ppdev != NULL && prclExtents != NULL)
   {
      IntFlushRectangle(ppdev, prclExtents);
   }
   return drawn;
}
