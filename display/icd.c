/*
 * OpenGL ICD announcement for the qemu-3dfx passthrough.
 *
 * Copyright (C) 2026 qemu-3dfx-build
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "framebuf.h"
#ifdef QEMU_PASSTHROUGH_PROBE
#include "ptprobe.h"
#endif

/* opengl32.dll asks the display driver twice. First ExtEscape(hdc, QUERYESCSUPPORT, OPENGL_GETINFO)
 * to learn whether we answer at all, then ExtEscape(hdc, OPENGL_GETINFO) for the structure below.
 * What it wants back is not the name of the ICD library but the name of the registry key under
 * OpenGLdrivers that holds it -- opengl32.dll looks the DLL up there and loads it.
 *
 * On NT the name is WCHAR where the 9x flavour uses CHAR, and that is exactly what makes the
 * structure 532 bytes wide, the buffer length opengl32.dll passes in. See
 * qemu-3dfx/wrappers/mesa/src/icddrv.c for the other side of this handshake.
 */

/* QUERYESCSUPPORT (8) comes from wingdi.h, OPENGL_GETINFO (4353 == 0x1101) from winddi.h. */

#define OPENGL_ICD_NAME_CHARS 262

typedef struct _OPENGL_ICD_INFO
{
   LONG  Version;
   LONG  DriverVersion;
   WCHAR RegistryKeyName[OPENGL_ICD_NAME_CHARS];
} OPENGL_ICD_INFO;

/* The same pair vmdisp9x/control.c:102 writes for QEMUFX, and what opengl32.dll checks
 * against. The dump of a real NVIDIA driver at vmdisp9x/control.c:65 has its 0x01 at
 * offset *6* rather than 4, which would read as 0x00010000 here -- tried, changes
 * nothing, so the value that is proven on 9x stays. */
#define OPENGL_ICD_VERSION        2
#define OPENGL_ICD_DRIVER_VERSION 1

/* Matches HKLM\Software\Microsoft\Windows\CurrentVersion\OpenGLdrivers\QEMUFX -> qmfxgl32.dll,
 * the same key name vmdisp9x announces on Windows 9x. */
static const OPENGL_ICD_INFO QemuFxIcdInfo =
{
   OPENGL_ICD_VERSION,
   OPENGL_ICD_DRIVER_VERSION,
   L"QEMUFX"
};

/*
 * DrvEscape
 */

ULONG APIENTRY
DrvEscape(
   SURFOBJ *pso,
   ULONG iEsc,
   ULONG cjIn,
   PVOID pvIn,
   ULONG cjOut,
   PVOID pvOut)
{
   UNREFERENCED_PARAMETER(pso);

#ifdef QEMU_PASSTHROUGH_PROBE
   /* Ahead of the trace line below: the stress run sends this escape thousands of times. */
   if (iEsc == QEMUDISP_ESCAPE_PASSTHROUGH_PROBE)
   {
      PPDEV ppdev = IntScreenDeviceForSurface(pso);

      return IntPassthroughProbeEscape(ppdev, cjIn, pvIn, cjOut, pvOut);
   }
#endif

   DbgPortLineHex("qemudisp: DrvEscape ", iEsc);

   switch (iEsc)
   {
      case QUERYESCSUPPORT:
      {
         ULONG QueriedEscape;

         if (pvIn == NULL || cjIn < sizeof(ULONG))
            return FALSE;

         QueriedEscape = *(ULONG *)pvIn;
         DbgPortLineHex("qemudisp: QUERYESCSUPPORT for ", QueriedEscape);
         if (QueriedEscape == OPENGL_GETINFO)
         {
            DbgPortLine("qemudisp: OPENGL_GETINFO is supported");
            return TRUE;
         }
#ifdef QEMU_PASSTHROUGH_PROBE
         if (QueriedEscape == QEMUDISP_ESCAPE_PASSTHROUGH_PROBE)
            return TRUE;
#endif

         return FALSE;
      }

      case OPENGL_GETINFO:
      {
         if (pvOut == NULL || cjOut < sizeof(OPENGL_ICD_INFO))
         {
            DbgPortLineHex("qemudisp: OPENGL_GETINFO buffer too small, bytes ", cjOut);
            return FALSE;
         }

         memcpy(pvOut, &QemuFxIcdInfo, sizeof(OPENGL_ICD_INFO));
         DbgPortLineHex("qemudisp: handed out the ICD name, bytes ", sizeof(OPENGL_ICD_INFO));
         return TRUE;
      }
   }

   return FALSE;
}
