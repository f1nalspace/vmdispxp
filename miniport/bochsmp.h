/*
 * PROJECT:     ReactOS Bochs graphics card driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Bochs graphics card driver
 * COPYRIGHT:   Copyright 2022 Hervé Poussineau <hpoussin@reactos.org>
 */

#ifndef BOCHS_H
#define BOCHS_H

#include <ntdef.h>
#include <dderror.h>
#include <miniport.h>
#include <video.h>
#include <devioctl.h>
#include <section_attribs.h>
#include "dbgport.h"
#include "fvm3dx_display_page.h"

#define VBE_EDID_SIZE                        0x80

#define VBE_DISPI_IOPORT_INDEX               0x01CE
#define VBE_DISPI_IOPORT_DATA                0x01CF
#define VBE_DISPI_INDEX_ID                   0x00
  #define VBE_DISPI_ID0                        0xB0C0
  #define VBE_DISPI_ID1                        0xB0C1
  #define VBE_DISPI_ID2                        0xB0C2
  #define VBE_DISPI_ID3                        0xB0C3
  #define VBE_DISPI_ID4                        0xB0C4
  #define VBE_DISPI_ID5                        0xB0C5
#define VBE_DISPI_INDEX_XRES                 0x01
#define VBE_DISPI_INDEX_YRES                 0x02
#define VBE_DISPI_INDEX_BPP                  0x03
#define VBE_DISPI_INDEX_ENABLE               0x04
  #define VBE_DISPI_DISABLED                   0x00
  #define VBE_DISPI_ENABLED                    0x01
  #define VBE_DISPI_GETCAPS                    0x02
  #define VBE_DISPI_LFB_ENABLED                0x40
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K     0x0A
/* fvm3dx: read-only, the refresh rates of the host's screen in whole Hz -- their count, then one per index (QEMU's std VGA, property refresh-rates). */
#define VBE_DISPI_INDEX_REFRESH_RATE_COUNT   0x10
#define VBE_DISPI_INDEX_REFRESH_RATE_FIRST   0x11
#define BOCHS_MAX_REFRESH_RATES              15
/* fvm3dx: the rate every mode has and comes first with -- what the driver offered alone before. */
#define BOCHS_DEFAULT_REFRESH_RATE           60

#define BOCHS_TAG 'BCHS'

typedef struct
{
    USHORT XResolution;
    USHORT YResolution;
} BOCHS_SIZE, *PBOCHS_SIZE;

/* fvm3dx: a mode is a resolution plus a colour depth. The original driver offered
 * 32 bpp only, which is enough for a desktop but not for the games of the era -- a
 * DirectDraw title asking for 640x480x16 gets nothing back.
 * And a refresh rate: each of the host screen's rates, so a game that chooses one gets vsync at it. */
typedef struct
{
    USHORT XResolution;
    USHORT YResolution;
    USHORT BitsPerPixel;
    USHORT Frequency;
} BOCHS_MODE, *PBOCHS_MODE;

typedef struct
{
    PUCHAR Mapped;
    PHYSICAL_ADDRESS RangeStart;
    ULONG RangeLength;
    UCHAR RangeInIoSpace;
} BOCHS_ADDRESS_RANGE;

typedef struct
{
    PBOCHS_MODE AvailableModeInfo;
    ULONG AvailableModeCount;
    USHORT CurrentMode;

    BOCHS_ADDRESS_RANGE FrameBuffer;
    BOCHS_ADDRESS_RANGE IoPorts;

    ULONG MaxXResolution;
    ULONG MaxYResolution;
    ULONG VramSize64K;

    /* fvm3dx: the refresh rates every mode is offered with, BOCHS_DEFAULT_REFRESH_RATE first, then the host's in ascending order. */
    USHORT RefreshRates[BOCHS_MAX_REFRESH_RATES + 1];
    ULONG RefreshRateCount;
} BOCHS_DEVICE_EXTENSION, *PBOCHS_DEVICE_EXTENSION;

#endif //BOCHS_H
