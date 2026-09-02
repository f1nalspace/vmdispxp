/*
 * The pixel format side of the OpenGL DDI.
 *
 * Copyright (C) 2026 qemu-3dfx-build
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "framebuf.h"

/* An application asks GDI, not opengl32, for its pixel formats: ChoosePixelFormat and
 * DescribePixelFormat in GDI32 end up at DrvDescribePixelFormat of the *display driver*.
 * A driver that offers none leaves the application with GDI's generic software formats,
 * and opengl32 then has no reason to go near an ICD -- which is what this is about.
 *
 * ReactOS' framebuf never had these, it was not written for OpenGL.
 */

#define PIXEL_FORMAT_COLOR_BITS   32
#define PIXEL_FORMAT_DEPTH_BITS   24
#define PIXEL_FORMAT_STENCIL_BITS 8
#define PIXEL_FORMAT_DEPTH_BITS_SMALL 16

/* BGRA in memory, which is what the 32 bpp mode of the miniport hands out. Nobody
 * fills these in for the driver -- a format that leaves them at zero describes a
 * surface with no colour in it. */
#define PIXEL_FORMAT_RED_BITS     8
#define PIXEL_FORMAT_RED_SHIFT    16
#define PIXEL_FORMAT_GREEN_BITS   8
#define PIXEL_FORMAT_GREEN_SHIFT  8
#define PIXEL_FORMAT_BLUE_BITS    8
#define PIXEL_FORMAT_BLUE_SHIFT   0
#define PIXEL_FORMAT_ALPHA_BITS   8
#define PIXEL_FORMAT_ALPHA_SHIFT  24

#define PIXEL_FORMAT_CHANNELS \
   PIXEL_FORMAT_RED_BITS,   PIXEL_FORMAT_RED_SHIFT, \
   PIXEL_FORMAT_GREEN_BITS, PIXEL_FORMAT_GREEN_SHIFT, \
   PIXEL_FORMAT_BLUE_BITS,  PIXEL_FORMAT_BLUE_SHIFT, \
   PIXEL_FORMAT_ALPHA_BITS, PIXEL_FORMAT_ALPHA_SHIFT

/* Deliberately short: these are the shapes a game asks for. The ICD holds the real list
 * the host GPU offers; this one only has to be good enough that an application finds
 * something and stops at a hardware format instead of a generic one. */
static const PIXELFORMATDESCRIPTOR SupportedPixelFormats[] =
{
   /* double buffered, full depth and stencil -- the common case */
   {
      sizeof(PIXELFORMATDESCRIPTOR), 1,
      PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
      PFD_TYPE_RGBA,
      PIXEL_FORMAT_COLOR_BITS,
      PIXEL_FORMAT_CHANNELS,
      0, 0, 0, 0, 0,             /* accumulation buffer */
      PIXEL_FORMAT_DEPTH_BITS,
      PIXEL_FORMAT_STENCIL_BITS,
      0,                         /* auxiliary buffers */
      PFD_MAIN_PLANE,
      0, 0, 0, 0
   },
   /* same, single buffered */
   {
      sizeof(PIXELFORMATDESCRIPTOR), 1,
      PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL,
      PFD_TYPE_RGBA,
      PIXEL_FORMAT_COLOR_BITS,
      PIXEL_FORMAT_CHANNELS,
      0, 0, 0, 0, 0,
      PIXEL_FORMAT_DEPTH_BITS,
      PIXEL_FORMAT_STENCIL_BITS,
      0,
      PFD_MAIN_PLANE,
      0, 0, 0, 0
   },
   /* double buffered with a 16 bit depth buffer and no stencil */
   {
      sizeof(PIXELFORMATDESCRIPTOR), 1,
      PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
      PFD_TYPE_RGBA,
      PIXEL_FORMAT_COLOR_BITS,
      PIXEL_FORMAT_CHANNELS,
      0, 0, 0, 0, 0,
      PIXEL_FORMAT_DEPTH_BITS_SMALL,
      0,
      0,
      PFD_MAIN_PLANE,
      0, 0, 0, 0
   }
};

#define SUPPORTED_PIXEL_FORMAT_COUNT ((LONG)(sizeof(SupportedPixelFormats) / sizeof(SupportedPixelFormats[0])))

/*
 * DrvDescribePixelFormat
 */

LONG APIENTRY
DrvDescribePixelFormat(
   DHPDEV dhpdev,
   LONG iPixelFormat,
   ULONG cjpfd,
   PIXELFORMATDESCRIPTOR *ppfd)
{
   LONG FormatIndex = iPixelFormat - 1;

   UNREFERENCED_PARAMETER(dhpdev);

   DbgPortLineHex("qemudisp: DrvDescribePixelFormat, format ", iPixelFormat);

   /* A NULL buffer means the caller only wants to know how many formats there are. */
   if (ppfd == NULL || cjpfd < sizeof(PIXELFORMATDESCRIPTOR))
      return SUPPORTED_PIXEL_FORMAT_COUNT;

   if (FormatIndex < 0 || FormatIndex >= SUPPORTED_PIXEL_FORMAT_COUNT)
      return 0;

   memcpy(ppfd, &SupportedPixelFormats[FormatIndex], sizeof(PIXELFORMATDESCRIPTOR));

   return SUPPORTED_PIXEL_FORMAT_COUNT;
}

/*
 * DrvSetPixelFormat
 */

BOOL APIENTRY
DrvSetPixelFormat(
   SURFOBJ *pso,
   LONG iPixelFormat,
   HWND hwnd)
{
   LONG FormatIndex = iPixelFormat - 1;

   UNREFERENCED_PARAMETER(pso);
   UNREFERENCED_PARAMETER(hwnd);

   DbgPortLineHex("qemudisp: DrvSetPixelFormat, format ", iPixelFormat);

   if (FormatIndex < 0 || FormatIndex >= SUPPORTED_PIXEL_FORMAT_COUNT)
      return FALSE;

   /* Nothing to set up on this side: the ICD talks to the host on its own, and the
    * format it ends up using is the one it picked there. */
   return TRUE;
}

/*
 * DrvSwapBuffers
 */

BOOL APIENTRY
DrvSwapBuffers(
   SURFOBJ *pso,
   WNDOBJ *pwo)
{
   UNREFERENCED_PARAMETER(pso);
   UNREFERENCED_PARAMETER(pwo);

   DbgPortLine("qemudisp: DrvSwapBuffers");

   /* The ICD swaps on the host and never gets here; GDI only asks for the generic path. */
   return TRUE;
}
