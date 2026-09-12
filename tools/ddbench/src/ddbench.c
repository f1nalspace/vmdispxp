/*
 * ddbench -- measures what DirectDraw costs on this display driver.
 *
 * Uses the system's own ddraw.dll, so this is the plain 2D route a game of the era takes:
 * exclusive full screen, a flipping chain, lock the back buffer, write pixels, flip.
 * The wine9x route (winedd -> wined3d -> OpenGL) is a different measurement entirely and
 * is what DDTEST.BAT does; the two must not be confused.
 *
 * Output is German because docs/LOG.md quotes it verbatim; the comments are English.
 */

#include <windows.h>
#include <ddraw.h>
#include <stdio.h>

static const double secondsPerTest = 2.0;
static const int bytesPerMegabyte = 1024 * 1024;

/* The rectangle a sprite blit moves. Roughly a character in a 2D game. */
static const int spriteWidth = 128;
static const int spriteHeight = 128;

typedef struct
{
   LARGE_INTEGER ticksPerSecond;
   int usePerformanceCounter;
} TimeSource;

static void TimeSourceInit(TimeSource *source)
{
   BOOL haveCounter = QueryPerformanceFrequency(&source->ticksPerSecond);
   source->usePerformanceCounter = (haveCounter && source->ticksPerSecond.QuadPart > 0);
}

static double TimeSourceNow(const TimeSource *source)
{
   if (source->usePerformanceCounter)
   {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      return (double)now.QuadPart / (double)source->ticksPerSecond.QuadPart;
   }
   {
      DWORD milliseconds = GetTickCount();
      return (double)milliseconds / 1000.0;
   }
}

static FILE *reportFile;

static void ReportLine(const char *text)
{
   fputs(text, stdout);
   fputs("\n", stdout);
   if (reportFile != NULL)
   {
      fputs(text, reportFile);
      fputs("\r\n", reportFile);
      fflush(reportFile);
   }
}

static void ReportMeasurement(const char *name, long operationCount, double elapsedSeconds, double bytesPerOperation)
{
   char line[256];
   double operationsPerSecond = (elapsedSeconds > 0.0) ? ((double)operationCount / elapsedSeconds) : 0.0;
   double megabytesPerSecond = (operationsPerSecond * bytesPerOperation) / (double)bytesPerMegabyte;
   double millisecondsPerOperation = (operationsPerSecond > 0.0) ? (1000.0 / operationsPerSecond) : 0.0;

   sprintf(line, "  %-34s %10.1f /s   %8.2f ms   %9.1f MB/s",
           name, operationsPerSecond, millisecondsPerOperation, megabytesPerSecond);
   ReportLine(line);
}

static LRESULT CALLBACK BenchWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
   if (message == WM_DESTROY)
   {
      PostQuitMessage(0);
      return 0;
   }
   return DefWindowProc(window, message, wParam, lParam);
}

static HWND CreateExclusiveWindow(void)
{
   static const char *windowClassName = "ddbenchWindow";
   WNDCLASSA windowClass;
   HINSTANCE moduleHandle = GetModuleHandle(NULL);

   ZeroMemory(&windowClass, sizeof(windowClass));
   windowClass.lpfnWndProc = BenchWindowProc;
   windowClass.hInstance = moduleHandle;
   windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
   windowClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
   windowClass.lpszClassName = windowClassName;
   RegisterClassA(&windowClass);

   return CreateWindowExA(WS_EX_TOPMOST, windowClassName, "ddbench", WS_POPUP | WS_VISIBLE,
                          0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                          NULL, NULL, moduleHandle, NULL);
}

static void PumpPendingMessages(void)
{
   MSG message;
   while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE))
   {
      TranslateMessage(&message);
      DispatchMessage(&message);
   }
}

static void ReportDriverCapabilities(LPDIRECTDRAW directDraw)
{
   char line[256];
   DDCAPS hardwareCaps;
   DDCAPS emulationCaps;
   HRESULT result;

   ZeroMemory(&hardwareCaps, sizeof(hardwareCaps));
   ZeroMemory(&emulationCaps, sizeof(emulationCaps));
   hardwareCaps.dwSize = sizeof(hardwareCaps);
   emulationCaps.dwSize = sizeof(emulationCaps);

   result = IDirectDraw_GetCaps(directDraw, &hardwareCaps, &emulationCaps);
   if (FAILED(result))
   {
      sprintf(line, "GetCaps failed: 0x%08lx", (unsigned long)result);
      ReportLine(line);
      return;
   }

   /* DirectDraw says it plainly in a bit of its own: DDCAPS_NOHARDWARE means the display
    * driver brought no DirectDraw callbacks, so everything below runs in the emulation
    * and every surface ends up in system memory. Do not guess this from dwCaps != 0 --
    * DDCAPS_NOHARDWARE is itself a bit, and the guess comes out backwards. */
   if (hardwareCaps.dwCaps & DDCAPS_NOHARDWARE)
   {
      ReportLine("HAL             none (DDCAPS_NOHARDWARE) -- everything through emulation");
   }
   else if (hardwareCaps.dwCaps & DDCAPS_BLT)
   {
      ReportLine("HAL             present, with a blitter");
   }
   else
   {
      ReportLine("HAL             reported, but without a blitter");
   }
   sprintf(line, "Video memory    %lu KB total, %lu KB free",
           (unsigned long)(hardwareCaps.dwVidMemTotal / 1024), (unsigned long)(hardwareCaps.dwVidMemFree / 1024));
   ReportLine(line);
   sprintf(line, "Blitter         dwCaps 0x%08lx, dwCaps2 0x%08lx, FXCaps 0x%08lx",
           (unsigned long)hardwareCaps.dwCaps, (unsigned long)hardwareCaps.dwCaps2, (unsigned long)hardwareCaps.dwFXCaps);
   ReportLine(line);
   sprintf(line, "Emulation       dwCaps 0x%08lx, FXCaps 0x%08lx",
           (unsigned long)emulationCaps.dwCaps, (unsigned long)emulationCaps.dwFXCaps);
   ReportLine(line);
}

/* Writing every pixel of the locked surface is what a software renderer does, and it is
 * the measurement that says whether the surface lives in cached memory or not. */
static void FillLockedSurface(void *bits, long pitch, int width, int height, int bytesPerPixel, unsigned long seed)
{
   int y;
   for (y = 0; y < height; y++)
   {
      unsigned char *row = (unsigned char *)bits + (long)y * pitch;
      if (bytesPerPixel == 4)
      {
         unsigned long *pixels = (unsigned long *)row;
         int x;
         for (x = 0; x < width; x++)
         {
            pixels[x] = seed + (unsigned long)x + (unsigned long)y;
         }
      }
      else
      {
         unsigned short *pixels = (unsigned short *)row;
         int x;
         for (x = 0; x < width; x++)
         {
            pixels[x] = (unsigned short)(seed + (unsigned long)x + (unsigned long)y);
         }
      }
   }
}

int main(int argc, char **argv)
{
   const char *outputPath = (argc > 1) ? argv[1] : "ddbench.txt";
   int requestedWidth = (argc > 2) ? atoi(argv[2]) : 1280;
   int requestedHeight = (argc > 3) ? atoi(argv[3]) : 720;
   int requestedDepth = (argc > 4) ? atoi(argv[4]) : 32;
   int bytesPerPixel = requestedDepth / 8;
   double surfaceBytes = (double)requestedWidth * requestedHeight * bytesPerPixel;
   double spriteBytes = (double)spriteWidth * spriteHeight * bytesPerPixel;

   LPDIRECTDRAW directDraw = NULL;
   LPDIRECTDRAWSURFACE primarySurface = NULL;
   LPDIRECTDRAWSURFACE backSurface = NULL;
   LPDIRECTDRAWSURFACE spriteSurface = NULL;
   DDSURFACEDESC surfaceDescription;
   DDSCAPS backBufferCaps;
   HWND window;
   HRESULT result;
   TimeSource time;
   char line[256];
   int haveFlippingChain = 1;

   reportFile = fopen(outputPath, "w");
   TimeSourceInit(&time);

   ReportLine("ddbench -- DirectDraw 2D through the display driver");
   sprintf(line, "Requested       %d x %d x %d", requestedWidth, requestedHeight, requestedDepth);
   ReportLine(line);

   window = CreateExclusiveWindow();
   SetForegroundWindow(window);
   PumpPendingMessages();

   result = DirectDrawCreate(NULL, &directDraw, NULL);
   if (FAILED(result))
   {
      sprintf(line, "ERROR: DirectDrawCreate 0x%08lx", (unsigned long)result);
      ReportLine(line);
      return 1;
   }

   ReportDriverCapabilities(directDraw);

   result = IDirectDraw_SetCooperativeLevel(directDraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
   if (FAILED(result))
   {
      sprintf(line, "ERROR: SetCooperativeLevel 0x%08lx", (unsigned long)result);
      ReportLine(line);
      return 1;
   }

   {
      double modeChangeStart = TimeSourceNow(&time);
      result = IDirectDraw_SetDisplayMode(directDraw, requestedWidth, requestedHeight, requestedDepth);
      if (FAILED(result))
      {
         sprintf(line, "ERROR: SetDisplayMode %dx%dx%d 0x%08lx",
                 requestedWidth, requestedHeight, requestedDepth, (unsigned long)result);
         ReportLine(line);
         IDirectDraw_Release(directDraw);
         return 1;
      }
      sprintf(line, "Mode change     %.1f ms", (TimeSourceNow(&time) - modeChangeStart) * 1000.0);
      ReportLine(line);
   }

   ZeroMemory(&surfaceDescription, sizeof(surfaceDescription));
   surfaceDescription.dwSize = sizeof(surfaceDescription);
   surfaceDescription.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
   surfaceDescription.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
   surfaceDescription.dwBackBufferCount = 1;
   result = IDirectDraw_CreateSurface(directDraw, &surfaceDescription, &primarySurface, NULL);
   if (FAILED(result))
   {
      sprintf(line, "No flipping chain possible (0x%08lx), single primary surface", (unsigned long)result);
      ReportLine(line);
      haveFlippingChain = 0;
      ZeroMemory(&surfaceDescription, sizeof(surfaceDescription));
      surfaceDescription.dwSize = sizeof(surfaceDescription);
      surfaceDescription.dwFlags = DDSD_CAPS;
      surfaceDescription.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
      result = IDirectDraw_CreateSurface(directDraw, &surfaceDescription, &primarySurface, NULL);
      if (FAILED(result))
      {
         sprintf(line, "ERROR: CreateSurface (primary) 0x%08lx", (unsigned long)result);
         ReportLine(line);
         IDirectDraw_Release(directDraw);
         return 1;
      }
   }

   if (haveFlippingChain)
   {
      ZeroMemory(&backBufferCaps, sizeof(backBufferCaps));
      backBufferCaps.dwCaps = DDSCAPS_BACKBUFFER;
      result = IDirectDrawSurface_GetAttachedSurface(primarySurface, &backBufferCaps, &backSurface);
      if (FAILED(result))
      {
         sprintf(line, "ERROR: GetAttachedSurface 0x%08lx", (unsigned long)result);
         ReportLine(line);
         haveFlippingChain = 0;
      }
   }

   {
      LPDIRECTDRAWSURFACE drawSurface = haveFlippingChain ? backSurface : primarySurface;

      /* Where the surface really is decides everything below. */
      ZeroMemory(&surfaceDescription, sizeof(surfaceDescription));
      surfaceDescription.dwSize = sizeof(surfaceDescription);
      if (SUCCEEDED(IDirectDrawSurface_GetSurfaceDesc(drawSurface, &surfaceDescription)))
      {
         const char *where = (surfaceDescription.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY) ? "video memory" :
                             (surfaceDescription.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) ? "system memory" : "unknown";
         sprintf(line, "Target surface  %lu x %lu, pitch %ld, in %s (dwCaps 0x%08lx)",
                 (unsigned long)surfaceDescription.dwWidth, (unsigned long)surfaceDescription.dwHeight,
                 (long)surfaceDescription.lPitch, where, (unsigned long)surfaceDescription.ddsCaps.dwCaps);
         ReportLine(line);
      }

      ReportLine("");
      ReportLine("  Test                                   Calls     Duration     Throughput");

      /* 1 -- lock, write every pixel, unlock. The software renderer's inner loop. */
      {
         long iterationCount = 0;
         double startTime = TimeSourceNow(&time);
         double elapsed = 0.0;

         for (;;)
         {
            DDSURFACEDESC lockedDescription;
            ZeroMemory(&lockedDescription, sizeof(lockedDescription));
            lockedDescription.dwSize = sizeof(lockedDescription);
            if (SUCCEEDED(IDirectDrawSurface_Lock(drawSurface, NULL, &lockedDescription, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL)))
            {
               FillLockedSurface(lockedDescription.lpSurface, lockedDescription.lPitch,
                                 requestedWidth, requestedHeight, bytesPerPixel, (unsigned long)iterationCount);
               IDirectDrawSurface_Unlock(drawSurface, NULL);
            }
            iterationCount++;
            elapsed = TimeSourceNow(&time) - startTime;
            if (elapsed >= secondsPerTest)
            {
               break;
            }
         }
         ReportMeasurement("Lock, fill, unlock", iterationCount, elapsed, surfaceBytes);
      }

      /* 2 -- a solid colour fill through the blitter. */
      {
         long iterationCount = 0;
         double startTime = TimeSourceNow(&time);
         double elapsed = 0.0;

         for (;;)
         {
            DDBLTFX blitEffects;
            ZeroMemory(&blitEffects, sizeof(blitEffects));
            blitEffects.dwSize = sizeof(blitEffects);
            blitEffects.dwFillColor = (DWORD)iterationCount;
            IDirectDrawSurface_Blt(drawSurface, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &blitEffects);
            iterationCount++;
            elapsed = TimeSourceNow(&time) - startTime;
            if (elapsed >= secondsPerTest)
            {
               break;
            }
         }
         ReportMeasurement("Fill a surface (Blt COLORFILL)", iterationCount, elapsed, surfaceBytes);
      }

      /* 3 -- a sprite blit from an offscreen surface, the other half of a 2D game. */
      ZeroMemory(&surfaceDescription, sizeof(surfaceDescription));
      surfaceDescription.dwSize = sizeof(surfaceDescription);
      surfaceDescription.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
      surfaceDescription.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
      surfaceDescription.dwWidth = spriteWidth;
      surfaceDescription.dwHeight = spriteHeight;
      result = IDirectDraw_CreateSurface(directDraw, &surfaceDescription, &spriteSurface, NULL);
      if (SUCCEEDED(result))
      {
         long iterationCount = 0;
         double startTime = TimeSourceNow(&time);
         double elapsed = 0.0;
         int maximumX = requestedWidth - spriteWidth;
         int maximumY = requestedHeight - spriteHeight;

         for (;;)
         {
            RECT targetRectangle;
            int stepIndex = (int)(iterationCount & 0xff);
            targetRectangle.left = (maximumX > 0) ? (stepIndex * maximumX / 256) : 0;
            targetRectangle.top = (maximumY > 0) ? (stepIndex * maximumY / 256) : 0;
            targetRectangle.right = targetRectangle.left + spriteWidth;
            targetRectangle.bottom = targetRectangle.top + spriteHeight;
            IDirectDrawSurface_Blt(drawSurface, &targetRectangle, spriteSurface, NULL, DDBLT_WAIT, NULL);
            iterationCount++;
            elapsed = TimeSourceNow(&time) - startTime;
            if (elapsed >= secondsPerTest)
            {
               break;
            }
         }
         ReportMeasurement("Blit a sprite (128x128)", iterationCount, elapsed, spriteBytes);
      }
      else
      {
         sprintf(line, "  Blit a sprite                    CreateSurface 0x%08lx", (unsigned long)result);
         ReportLine(line);
      }

      /* 4 -- the flip itself, without drawing anything. */
      if (haveFlippingChain)
      {
         long iterationCount = 0;
         double startTime = TimeSourceNow(&time);
         double elapsed = 0.0;

         for (;;)
         {
            IDirectDrawSurface_Flip(primarySurface, NULL, DDFLIP_WAIT);
            iterationCount++;
            elapsed = TimeSourceNow(&time) - startTime;
            if (elapsed >= secondsPerTest)
            {
               break;
            }
         }
         ReportMeasurement("Switch buffers (Flip)", iterationCount, elapsed, surfaceBytes);
      }
      else
      {
         ReportLine("  Switch buffers (Flip)              no flipping chain");
      }

      /* 5 -- draw and flip together: what the frame rate of a 2D game actually is. */
      {
         long iterationCount = 0;
         double startTime = TimeSourceNow(&time);
         double elapsed = 0.0;

         for (;;)
         {
            DDSURFACEDESC lockedDescription;
            ZeroMemory(&lockedDescription, sizeof(lockedDescription));
            lockedDescription.dwSize = sizeof(lockedDescription);
            if (SUCCEEDED(IDirectDrawSurface_Lock(drawSurface, NULL, &lockedDescription, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL)))
            {
               FillLockedSurface(lockedDescription.lpSurface, lockedDescription.lPitch,
                                 requestedWidth, requestedHeight, bytesPerPixel, (unsigned long)iterationCount);
               IDirectDrawSurface_Unlock(drawSurface, NULL);
            }
            if (haveFlippingChain)
            {
               IDirectDrawSurface_Flip(primarySurface, NULL, DDFLIP_WAIT);
            }
            iterationCount++;
            elapsed = TimeSourceNow(&time) - startTime;
            if (elapsed >= secondsPerTest)
            {
               break;
            }
         }
         ReportMeasurement("Draw a full frame and show it", iterationCount, elapsed, surfaceBytes);
      }
   }

   if (spriteSurface != NULL)
   {
      IDirectDrawSurface_Release(spriteSurface);
   }
   if (backSurface != NULL)
   {
      IDirectDrawSurface_Release(backSurface);
   }
   if (primarySurface != NULL)
   {
      IDirectDrawSurface_Release(primarySurface);
   }
   IDirectDraw_RestoreDisplayMode(directDraw);
   IDirectDraw_SetCooperativeLevel(directDraw, window, DDSCL_NORMAL);
   IDirectDraw_Release(directDraw);

   DestroyWindow(window);
   PumpPendingMessages();

   ReportLine("");
   ReportLine("done.");
   if (reportFile != NULL)
   {
      fclose(reportFile);
   }
   return 0;
}
