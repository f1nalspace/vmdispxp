/*
 * Plan C spike, phases 0 and 1: drive the qemu-3dfx passthrough from kernel mode.
 *
 * Copyright (C) 2026 qemu-3dfx-build
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

/* What the wrapper does from user mode to own the passthrough, done here from inside
 * DrvEscape: the same registers and the same shared words, in the same order. Nothing in QEMU
 * or in the wrapper changes for it. tools/ptprobe asks for one step per escape.
 *
 * The pages come from the miniport, because MmMapIoSpace is an ntoskrnl import and this DLL
 * may import from win32k.sys alone (docs/LOG.md [348]).
 *
 * Every pointer into the passthrough is volatile. QEMU rewrites these words while it handles a
 * register write, and a compiler that forwards a value it just stored would never see that.
 *
 * Only built with make PASSTHROUGH_PROBE=1. See docs/LOG.md [467].
 */

#include "framebuf.h"
#include "ptprobe.h"

/* mglCreateContext passes the guest HDC here, and QEMU only asks whether it is a pbuffer
 * handle (mglcntx_linux.c:486). Anything else will do. */
#define PTPROBE_DEVICE_CONTEXT_TOKEN 0x0DDC0DE0

/* The wrapper's empty data area starts after one aligned header word (wrapgl32.c:77). */
#define PTPROBE_EMPTY_DATA_FILL (ALIGNED(1) >> 2)

/* The same pixel format glcube asks for. */
#define PTPROBE_PIXEL_COLOR_BITS 32
#define PTPROBE_PIXEL_DEPTH_BITS 24

/* The wrapper leaves its build time after the stamp, and QEMU prints both in its
 * "wglMakeCurrent" line. This makes the session in the log say whose it is. */
static const char ProbeBuildText[] = "vmdispxp kernel probe ";

static QEMUMP_PASSTHROUGH_MAPPING Passthrough;
static BOOL PassthroughMapped;

static VOID
WriteRegister(ULONG registerOffset, ULONG value)
{
   volatile ULONG *registerWords = (volatile ULONG *)Passthrough.TriggerPage;
   ULONG wordIndex = registerOffset >> MESAPT_REGISTER_SHIFT;

   registerWords[wordIndex] = value;
}

static ULONG
ReadRegister(ULONG registerOffset)
{
   volatile ULONG *registerWords = (volatile ULONG *)Passthrough.TriggerPage;
   ULONG wordIndex = registerOffset >> MESAPT_REGISTER_SHIFT;

   return registerWords[wordIndex];
}

static volatile ULONG *
ParameterWords(VOID)
{
   PBYTE tailPages = (PBYTE)Passthrough.FifoTailPages;

   return (volatile ULONG *)(tailPages + PTPROBE_PARAMETER_PAGE_OFFSET);
}

static PBYTE
StampBytes(VOID)
{
   PBYTE frameTransferTail = (PBYTE)Passthrough.FrameTransferTailPage;

   return frameTransferTail + PTPROBE_STAMP_OFFSET;
}

static VOID
FillPixelFormat(PIXELFORMATDESCRIPTOR *pixelFormat)
{
   RtlZeroMemory(pixelFormat, sizeof(*pixelFormat));
   pixelFormat->nSize = sizeof(*pixelFormat);
   pixelFormat->nVersion = 1;
   pixelFormat->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
   pixelFormat->iPixelType = PFD_TYPE_RGBA;
   pixelFormat->cColorBits = PTPROBE_PIXEL_COLOR_BITS;
   pixelFormat->cDepthBits = PTPROBE_PIXEL_DEPTH_BITS;
   pixelFormat->iLayerType = PFD_MAIN_PLANE;
}

/* Drops everything that marks the passthrough as ours, the tail end of DLL_PROCESS_DETACH. */
static VOID
ReleaseClaim(VOID)
{
   volatile ULONG *fifoWords = (volatile ULONG *)Passthrough.FifoHeadPage;
   volatile ULONG *dataWords = (volatile ULONG *)Passthrough.DataHeadPage;
   PBYTE stampBytes = StampBytes();

   RtlZeroMemory(stampBytes, PTPROBE_STAMP_BYTES);
   fifoWords[MESAPT_FIFO_OWNER_WORD] = 0;
   dataWords[MESAPT_DATA_REFCOUNT_WORD] = 0;
}

static PTPROBE_STATUS
ProbeMap(PPDEV ppdev, PTPROBE_RESPONSE *response)
{
   QEMUMP_PASSTHROUGH_MAPPING mapping;
   DWORD bytesReturned = 0;
   DWORD ioControlResult;

   if (!PassthroughMapped)
   {
      RtlZeroMemory(&mapping, sizeof(mapping));
      ioControlResult = EngDeviceIoControl(ppdev->hDriver, IOCTL_QEMUMP_MAP_PASSTHROUGH, NULL, 0, &mapping, sizeof(mapping), &bytesReturned);
      DbgPortLineHex("qemudisp: map passthrough, result ", ioControlResult);
      if (ioControlResult != NO_ERROR || bytesReturned < sizeof(mapping))
         return PTPROBE_STATUS_MAP_FAILED;

      Passthrough = mapping;
      PassthroughMapped = TRUE;
   }

   response->Value[0] = (ULONG)Passthrough.TriggerPage;
   response->Value[1] = (ULONG)Passthrough.FifoHeadPage;
   response->Value[2] = (ULONG)Passthrough.DataHeadPage;
   response->Value[3] = (ULONG)Passthrough.FifoTailPages;
   response->Value[4] = (ULONG)Passthrough.FrameTransferTailPage;
   return PTPROBE_STATUS_OK;
}

static PTPROBE_STATUS
ProbeReadState(PTPROBE_RESPONSE *response)
{
   volatile ULONG *fifoWords = (volatile ULONG *)Passthrough.FifoHeadPage;
   volatile ULONG *dataWords = (volatile ULONG *)Passthrough.DataHeadPage;
   PBYTE stampBytes = StampBytes();
   ULONG byteIndex;

   response->Value[0] = ReadRegister(MESAPT_REGISTER_LIBRARY);
   response->Value[1] = ReadRegister(MESAPT_REGISTER_WINDOW_READY);
   response->Value[2] = fifoWords[MESAPT_FIFO_FILL_WORD];
   response->Value[3] = fifoWords[MESAPT_FIFO_OWNER_WORD];
   response->Value[4] = dataWords[MESAPT_DATA_FILL_WORD];
   response->Value[5] = dataWords[MESAPT_DATA_REFCOUNT_WORD];

   for (byteIndex = 0; byteIndex < PTPROBE_STAMP_BYTES; byteIndex++)
      response->Text[byteIndex] = (char)stampBytes[byteIndex];
   response->Text[PTPROBE_STAMP_BYTES] = '\0';
   return PTPROBE_STATUS_OK;
}

/* mesapt_mm.c:2542 reads the name and the two sizes from the parameter page. */
static PTPROBE_STATUS
ProbeMarkDrawable(const PTPROBE_REQUEST *request)
{
   static const char FunctionName[] = "wglSetDrawableSize3DFX";
   volatile ULONG *parameterWords = ParameterWords();
   PBYTE parameterBytes = (PBYTE)parameterWords;
   const ULONG nameBytes = sizeof(FunctionName);
   /* strnlen without the terminator, then ALIGNED -- held in a variable of its own, because
    * the macro does not parenthesise its argument. */
   const ULONG nameLength = nameBytes - 1;
   const ULONG argumentOffset = ALIGNED(nameLength);
   volatile ULONG *argumentWords = (volatile ULONG *)(parameterBytes + argumentOffset);

   RtlZeroMemory(parameterBytes, argumentOffset);
   memcpy(parameterBytes, FunctionName, nameBytes);
   argumentWords[0] = request->Argument[0];
   argumentWords[1] = request->Argument[1];
   WriteRegister(MESAPT_REGISTER_NAMED_FUNCTION, MESAGL_MAGIC);
   return PTPROBE_STATUS_OK;
}

/* DllMain's DLL_PROCESS_ATTACH and InitMesaPTMMBase (wrapgl32.c:60-80, :17645-17693), then
 * wglChoosePixelFormat (:17459). Waiting for the host window is left to the caller, which
 * polls the window ready register through PTPROBE_OPERATION_READ_STATE. */
static PTPROBE_STATUS
ProbeSessionBegin(const PTPROBE_REQUEST *request, PTPROBE_RESPONSE *response)
{
   volatile ULONG *fifoWords = (volatile ULONG *)Passthrough.FifoHeadPage;
   volatile ULONG *dataWords = (volatile ULONG *)Passthrough.DataHeadPage;
   volatile ULONG *parameterWords = ParameterWords();
   PBYTE stampBytes = StampBytes();
   PIXELFORMATDESCRIPTOR pixelFormat;
   const ULONG ownerBefore = fifoWords[MESAPT_FIFO_OWNER_WORD];
   const ULONG ownerRegisterOffset = ownerBefore & MESAPT_PAGE_OFFSET_MASK;
   const ULONG ownFunctionRegister = (ULONG)Passthrough.TriggerPage + MESAPT_REGISTER_FUNCTION;
   const ULONG attachRequest = (MESAGL_LIBRARY_ATTACH_TAG << MESAGL_LIBRARY_TAG_SHIFT) | MESAGL_WRAPPER_VERSION;
   const ULONG expectedAnswer = (MESAGL_WRAPPER_VERSION << MESAGL_LIBRARY_ANSWER_VERSION_SHIFT) | MESAGL_LIBRARY_ATTACH_TAG;
   ULONG attachAnswer;

   response->Value[1] = ownerBefore;
   if (ownerRegisterOffset == MESAPT_REGISTER_FUNCTION)
   {
      DbgPortLineHex("qemudisp: passthrough already owned, owner word ", ownerBefore);
      return PTPROBE_STATUS_OWNED_ELSEWHERE;
   }

   memcpy(stampBytes, request->CommitStamp, PTPROBE_STAMP_BYTES);
   fifoWords[MESAPT_FIFO_FILL_WORD] = FIRST_FIFO;
   dataWords[MESAPT_DATA_FILL_WORD] = PTPROBE_EMPTY_DATA_FILL;
   fifoWords[MESAPT_FIFO_OWNER_WORD] = ownFunctionRegister;
   dataWords[MESAPT_DATA_REFCOUNT_WORD] = 1;

   WriteRegister(MESAPT_REGISTER_LIBRARY, attachRequest);
   attachAnswer = ReadRegister(MESAPT_REGISTER_LIBRARY);
   response->Value[0] = attachAnswer;
   DbgPortLineHex("qemudisp: library attach answered ", attachAnswer);
   if (attachAnswer != expectedAnswer)
   {
      ReleaseClaim();
      return PTPROBE_STATUS_STAMP_REJECTED;
   }

   FillPixelFormat(&pixelFormat);
   /* No MSAA, scaler, flip or buffer object option, and the default display timer: what
    * PPFD_CONFIG() produces without a wrapgl32.ext (wrapgl32.c:17453). */
   parameterWords[0] = 0;
   parameterWords[1] = DISPTMR_DEFAULT;
   memcpy((PBYTE)&parameterWords[2], &pixelFormat, sizeof(pixelFormat));
   WriteRegister(MESAPT_REGISTER_CHOOSE_PIXEL_FORMAT, MESAGL_MAGIC);
   return PTPROBE_STATUS_OK;
}

/* wglSetPixelFormat (wrapgl32.c:17510), mglCreateContext (:17219), mglMakeCurrent (:17242). */
static PTPROBE_STATUS
ProbeSessionContext(const PTPROBE_REQUEST *request, PTPROBE_RESPONSE *response)
{
   volatile ULONG *parameterWords = ParameterWords();
   PBYTE parameterBytes = (PBYTE)parameterWords;
   PIXELFORMATDESCRIPTOR pixelFormat;
   const ULONG pixelFormatIndex = ReadRegister(MESAPT_REGISTER_CHOOSE_PIXEL_FORMAT);
   const ULONG stampOffset = sizeof(ULONG);
   const ULONG buildTextOffset = stampOffset + PTPROBE_STAMP_BYTES;
   ULONG setAnswer;

   response->Value[0] = pixelFormatIndex;

   FillPixelFormat(&pixelFormat);
   parameterWords[0] = pixelFormatIndex;
   parameterWords[1] = (ULONG)Passthrough.TriggerPage;
   memcpy((PBYTE)&parameterWords[2], &pixelFormat, sizeof(pixelFormat));
   WriteRegister(MESAPT_REGISTER_SET_PIXEL_FORMAT, MESAGL_MAGIC);
   setAnswer = ReadRegister(MESAPT_REGISTER_SET_PIXEL_FORMAT);
   response->Value[1] = setAnswer;
   DbgPortLineHex("qemudisp: SetPixelFormat answered ", setAnswer);

   parameterWords[0] = PTPROBE_DEVICE_CONTEXT_TOKEN;
   WriteRegister(MESAPT_REGISTER_CREATE_CONTEXT, MESAGL_MAGIC);

   parameterWords[0] = MESAGL_MAGIC;
   memcpy(parameterBytes + stampOffset, request->CommitStamp, PTPROBE_STAMP_BYTES);
   memcpy(parameterBytes + buildTextOffset, ProbeBuildText, sizeof(ProbeBuildText));
   WriteRegister(MESAPT_REGISTER_MAKE_CURRENT, MESAGL_MAGIC);
   return PTPROBE_STATUS_OK;
}

/* A synchronous call with one argument, the shape of every generated stub that returns a value
 * (glIsTexture, wrapgl32.c:6666): argument into the FIFO's first argument word, function number
 * into the function register, result read back from it. */
static PTPROBE_STATUS
ProbeCallRepeated(const PTPROBE_REQUEST *request, PTPROBE_RESPONSE *response)
{
   volatile ULONG *fifoWords = (volatile ULONG *)Passthrough.FifoHeadPage;
   const ULONG functionNumber = request->Argument[0];
   const ULONG functionArgument = request->Argument[1];
   const ULONG repeatCount = request->Argument[2];
   const ULONG expectedResult = request->Argument[3];
   const ULONG resultMask = request->Argument[4];
   ULONG matchCount = 0;
   ULONG mismatchCount = 0;
   ULONG callIndex;

   for (callIndex = 0; callIndex < repeatCount; callIndex++)
   {
      ULONG result;
      ULONG significantResult;

      fifoWords[MESAPT_FIFO_FIRST_ARGUMENT_WORD] = functionArgument;
      WriteRegister(MESAPT_REGISTER_FUNCTION, functionNumber);
      result = ReadRegister(MESAPT_REGISTER_FUNCTION);
      significantResult = result & resultMask;

      if (callIndex == 0)
         response->Value[0] = result;
      if (significantResult == expectedResult)
      {
         matchCount++;
      }
      else
      {
         if (mismatchCount == 0)
         {
            response->Value[3] = result;
            response->Value[4] = callIndex;
         }
         mismatchCount++;
      }
   }

   response->Value[1] = matchCount;
   response->Value[2] = mismatchCount;
   return PTPROBE_STATUS_OK;
}

/* processFRet copies the string into the return buffer (mesapt_mm.c:2134). */
static PTPROBE_STATUS
ProbeGetString(const PTPROBE_REQUEST *request, PTPROBE_RESPONSE *response)
{
   volatile ULONG *fifoWords = (volatile ULONG *)Passthrough.FifoHeadPage;
   PBYTE returnBuffer = (PBYTE)Passthrough.FifoTailPages + PTPROBE_RETURN_BUFFER_OFFSET;
   const ULONG lastTextIndex = PTPROBE_TEXT_BYTES - 1;
   ULONG textIndex;
   ULONG result;

   fifoWords[MESAPT_FIFO_FIRST_ARGUMENT_WORD] = request->Argument[1];
   WriteRegister(MESAPT_REGISTER_FUNCTION, request->Argument[0]);
   result = ReadRegister(MESAPT_REGISTER_FUNCTION);
   response->Value[0] = result;
   if (result == 0)
      return PTPROBE_STATUS_OK;

   for (textIndex = 0; textIndex < lastTextIndex; textIndex++)
   {
      const char character = (char)returnBuffer[textIndex];

      if (character == '\0')
         break;
      response->Text[textIndex] = character;
   }
   response->Text[textIndex] = '\0';
   return PTPROBE_STATUS_OK;
}

/* DLL_PROCESS_DETACH with a current context (wrapgl32.c:17694-17711). */
static PTPROBE_STATUS
ProbeSessionEnd(VOID)
{
   volatile ULONG *parameterWords = ParameterWords();
   const ULONG detachRequest = (MESAGL_LIBRARY_DETACH_TAG << MESAGL_LIBRARY_TAG_SHIFT) | MESAGL_WRAPPER_VERSION;

   parameterWords[0] = 0;
   WriteRegister(MESAPT_REGISTER_MAKE_CURRENT, MESAGL_MAGIC);
   WriteRegister(MESAPT_REGISTER_DELETE_CONTEXT, MESAGL_MAGIC);
   WriteRegister(MESAPT_REGISTER_LIBRARY, detachRequest);
   ReleaseClaim();
   return PTPROBE_STATUS_OK;
}

ULONG
IntPassthroughProbeEscape(
   PPDEV ppdev,
   ULONG cjIn,
   PVOID pvIn,
   ULONG cjOut,
   PVOID pvOut)
{
   const PTPROBE_REQUEST *request = (const PTPROBE_REQUEST *)pvIn;
   PTPROBE_RESPONSE *response = (PTPROBE_RESPONSE *)pvOut;
   PTPROBE_STATUS status;

   if (pvIn == NULL || cjIn < sizeof(PTPROBE_REQUEST) || pvOut == NULL || cjOut < sizeof(PTPROBE_RESPONSE))
      return FALSE;

   RtlZeroMemory(response, sizeof(*response));
   if (ppdev == NULL)
   {
      response->Status = PTPROBE_STATUS_NO_DEVICE;
      return TRUE;
   }

   status = ProbeMap(ppdev, response);
   if (status != PTPROBE_STATUS_OK || request->Operation == PTPROBE_OPERATION_MAP)
   {
      response->Status = status;
      return TRUE;
   }

   /* Not for the repeated call: the stress run issues it thousands of times, and every byte
    * on port 0xE9 is a VM exit of its own. */
   if (request->Operation != PTPROBE_OPERATION_CALL_REPEATED)
      DbgPortLineHex("qemudisp: passthrough probe operation ", request->Operation);

   switch (request->Operation)
   {
      case PTPROBE_OPERATION_READ_STATE:
         status = ProbeReadState(response);
         break;
      case PTPROBE_OPERATION_MARK_LOG:
         WriteRegister(MESAPT_REGISTER_LOG_MARKER, request->Argument[0]);
         status = PTPROBE_STATUS_OK;
         break;
      case PTPROBE_OPERATION_MARK_DRAWABLE:
         status = ProbeMarkDrawable(request);
         break;
      case PTPROBE_OPERATION_SESSION_BEGIN:
         status = ProbeSessionBegin(request, response);
         break;
      case PTPROBE_OPERATION_SESSION_CONTEXT:
         status = ProbeSessionContext(request, response);
         break;
      case PTPROBE_OPERATION_CALL_REPEATED:
         status = ProbeCallRepeated(request, response);
         break;
      case PTPROBE_OPERATION_GET_STRING:
         status = ProbeGetString(request, response);
         break;
      case PTPROBE_OPERATION_SESSION_END:
         status = ProbeSessionEnd();
         break;
      default:
         status = PTPROBE_STATUS_UNKNOWN_OPERATION;
         break;
   }

   response->Status = status;
   return TRUE;
}
