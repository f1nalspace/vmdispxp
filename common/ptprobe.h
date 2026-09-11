/*
 * Plan C spike, phases 0 and 1: reach the qemu-3dfx passthrough from kernel mode.
 *
 * Copyright (C) 2026 qemu-3dfx-build
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

/* Shared by three parties: the miniport maps the passthrough pages, the display driver
 * drives the protocol inside DrvEscape, and tools/ptprobe asks it to. Only built with
 * make PASSTHROUGH_PROBE=1; the normal driver never sees this file.
 *
 * The protocol is the wrapper's, taken step by step from wrappers/mesa/src/wrapgl32.c and
 * answered by qemu-1/hw/mesa/mesapt_mm.c. See docs/LOG.md [467] for why it is shaped the
 * way it is.
 */

#ifndef QEMU_PTPROBE_H
#define QEMU_PTPROBE_H

/* The layout comes from the fork itself, not from a copy: MESA_FIFO_BASE, MESA_FBTM_BASE,
 * MGLSHM_SIZE, MGLFBT_SIZE, MAX_FIFO, FIRST_FIFO, MESAGL_MAGIC, DISPTMR_DEFAULT and the
 * FEnum values. */
#include "mglfuncs.h"

/* The one address mglfuncs.h lacks. QEMU places the register page in
 * include/hw/i386/pc.h, and the wrapper repeats it in wrappers/mesa/src/wrapgl32.c:40. */
#define MESAPT_MM_BASE 0xefffe000

/* Registers on that page, named after what mesapt_write() and mesapt_read() do with them. */
#define MESAPT_REGISTER_FUNCTION            0xFC0
#define MESAPT_REGISTER_LIBRARY             0xFBC
#define MESAPT_REGISTER_WINDOW_READY        0xFB8
#define MESAPT_REGISTER_CREATE_CONTEXT      0xFFC
#define MESAPT_REGISTER_MAKE_CURRENT        0xFF8
#define MESAPT_REGISTER_DELETE_CONTEXT      0xFF4
#define MESAPT_REGISTER_CHOOSE_PIXEL_FORMAT 0xFEC
#define MESAPT_REGISTER_SET_PIXEL_FORMAT    0xFE4
#define MESAPT_REGISTER_NAMED_FUNCTION      0xFDC
/* No case in mesapt_write() takes this offset. A write ends in its "Unhandled" warning and
 * nowhere else, which makes it a marker in QEMU's log that changes no state at all. */
#define MESAPT_REGISTER_LOG_MARKER          0xFB0

#define MESAPT_REGISTER_SHIFT               2

/* What the wrapper writes to the library register on attach and detach
 * (wrapgl32.c:17686 and :17705), and how QEMU shapes its answer. */
#define MESAGL_WRAPPER_VERSION              0x320
#define MESAGL_LIBRARY_ATTACH_TAG           0xA0
#define MESAGL_LIBRARY_DETACH_TAG           0xD0
#define MESAGL_LIBRARY_TAG_SHIFT            12
#define MESAGL_LIBRARY_ANSWER_VERSION_SHIFT 8

/* Words at the head of the FIFO and of the data area (wrapgl32.c:72-78). The owner word holds
 * the virtual address of the owner's function register; with a page-aligned mapping its low
 * twelve bits are the register offset, which is all the wrapper checks. */
#define MESAPT_FIFO_FILL_WORD               0
#define MESAPT_FIFO_OWNER_WORD              1
#define MESAPT_FIFO_FIRST_ARGUMENT_WORD     2
#define MESAPT_DATA_FILL_WORD               0
#define MESAPT_DATA_REFCOUNT_WORD           1
#define MESAPT_PAGE_OFFSET_MASK             0xFFF

/* The pieces the miniport maps. The tail covers the return buffer (three pages before the end
 * of the FIFO) and the parameter page (the last one); the frame transfer tail holds the stamp. */
#define PTPROBE_FIFO_TAIL_PAGE_COUNT         3
#define PTPROBE_FIFO_HEAD_PHYSICAL           MESA_FIFO_BASE
#define PTPROBE_DATA_HEAD_PHYSICAL           (MESA_FIFO_BASE + (MAX_FIFO << 2))
#define PTPROBE_FIFO_TAIL_PHYSICAL           (MESA_FIFO_BASE + MGLSHM_SIZE - (PTPROBE_FIFO_TAIL_PAGE_COUNT * PAGE_SIZE))
#define PTPROBE_FRAME_TRANSFER_TAIL_PHYSICAL (MESA_FBTM_BASE + MGLFBT_SIZE - PAGE_SIZE)

#define PTPROBE_RETURN_BUFFER_OFFSET         0
#define PTPROBE_PARAMETER_PAGE_OFFSET        ((PTPROBE_FIFO_TAIL_PAGE_COUNT - 1) * PAGE_SIZE)
#define PTPROBE_STAMP_OFFSET                 (PAGE_SIZE - ALIGNBO(1))
#define PTPROBE_STAMP_BYTES                  ALIGNED(1)

#ifdef CTL_CODE
#define IOCTL_QEMUMP_MAP_PASSTHROUGH CTL_CODE(FILE_DEVICE_VIDEO, 0x8D0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

typedef struct _QEMUMP_PASSTHROUGH_MAPPING
{
   void *TriggerPage;
   void *FifoHeadPage;
   void *DataHeadPage;
   void *FifoTailPages;
   void *FrameTransferTailPage;
} QEMUMP_PASSTHROUGH_MAPPING;

/* A private escape. Values above 0x10000 stay clear of everything GDI defines itself. */
#define QEMUDISP_ESCAPE_PASSTHROUGH_PROBE 0x13DF0

typedef enum _PTPROBE_OPERATION
{
   /* Out: Value[0..4] the kernel addresses of the five pieces. */
   PTPROBE_OPERATION_MAP = 1,
   /* Out: Value[0] library register, [1] window ready register, [2] FIFO fill, [3] FIFO owner,
    * [4] data fill, [5] data refcount; Text the stamp in the frame transfer tail. */
   PTPROBE_OPERATION_READ_STATE,
   /* In: Argument[0] the marker. QEMU logs it as an unhandled write. */
   PTPROBE_OPERATION_MARK_LOG,
   /* In: Argument[0] width, [1] height, sent as wglSetDrawableSize3DFX through the named
    * function register. QEMU logs "guest drawable WxH". */
   PTPROBE_OPERATION_MARK_DRAWABLE,
   /* In: CommitStamp. Claims the passthrough the way DllMain does and asks for a pixel format.
    * Out: Value[0] the library register after attaching, [1] the owner word found before. */
   PTPROBE_OPERATION_SESSION_BEGIN,
   /* Out: Value[0] pixel format, [1] SetPixelFormat answer. Creates the level 0 context and
    * makes it current, the way mglCreateContext and mglMakeCurrent do. */
   PTPROBE_OPERATION_SESSION_CONTEXT,
   /* In: Argument[0] FEnum, [1] its single argument, [2] repeat count, [3] expected result,
    * [4] the bits of the result that count -- a GLboolean only defines the low byte.
    * Out: Value[0] first result, [1] matches, [2] mismatches, [3] first mismatching result,
    * [4] index of the first mismatch. */
   PTPROBE_OPERATION_CALL_REPEATED,
   /* In: Argument[0] FEnum_glGetString, [1] the name. Out: Value[0] the raw return, Text the
    * string QEMU copied into the return buffer. */
   PTPROBE_OPERATION_GET_STRING,
   /* Releases the context and the passthrough the way DLL_PROCESS_DETACH does. */
   PTPROBE_OPERATION_SESSION_END,
} PTPROBE_OPERATION;

typedef enum _PTPROBE_STATUS
{
   PTPROBE_STATUS_OK = 0,
   PTPROBE_STATUS_BAD_REQUEST,
   PTPROBE_STATUS_NO_DEVICE,
   PTPROBE_STATUS_MAP_FAILED,
   PTPROBE_STATUS_OWNED_ELSEWHERE,
   PTPROBE_STATUS_STAMP_REJECTED,
   PTPROBE_STATUS_UNKNOWN_OPERATION,
} PTPROBE_STATUS;

#define PTPROBE_ARGUMENT_COUNT 5
#define PTPROBE_VALUE_COUNT    8
#define PTPROBE_TEXT_BYTES     256

typedef struct _PTPROBE_REQUEST
{
   unsigned long Operation;
   unsigned long Argument[PTPROBE_ARGUMENT_COUNT];
   char CommitStamp[PTPROBE_STAMP_BYTES];
} PTPROBE_REQUEST;

typedef struct _PTPROBE_RESPONSE
{
   unsigned long Status;
   unsigned long Value[PTPROBE_VALUE_COUNT];
   char Text[PTPROBE_TEXT_BYTES];
} PTPROBE_RESPONSE;

#endif /* QEMU_PTPROBE_H */
