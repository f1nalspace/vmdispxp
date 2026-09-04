/*
 * gdibench -- measures what a 2D display driver actually costs.
 *
 * Every test runs for a fixed wall clock slice and counts how often it got through, so
 * the numbers stay comparable between display drivers and between resolutions. The
 * interesting split is write versus read: an emulated frame buffer is usually cheap to
 * write and ruinous to read, and only a measurement says which of the two we are in.
 *
 * Output is German because docs/LOG.md quotes it verbatim; the comments are English.
 */

#include <windows.h>
#include <stdio.h>

/* Long enough that a single slow blit cannot dominate, short enough that the whole run
 * stays under a minute on a driver that draws every pixel by hand. */
static const double secondsPerTest = 2.0;

/* The rectangle a dragged window moves around with. Roughly a Notepad window. */
static const int draggedWindowWidth = 400;
static const int draggedWindowHeight = 300;

/* How far a scroll step moves. One text line. */
static const int scrollStepInPixels = 16;

static const int bytesPerMegabyte = 1024 * 1024;

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

typedef struct
{
   const char *name;
   double operationsPerSecond;
   double megabytesPerSecond;
   double millisecondsPerOperation;
} TestResult;

/* The report is written twice -- once to the console, once to the file the caller named
 * -- so a run over the SMB share leaves something to quote. */
static void ReportLine(FILE *outputFile, const char *text)
{
   fputs(text, stdout);
   fputs("\n", stdout);
   if (outputFile != NULL)
   {
      fputs(text, outputFile);
      fputs("\r\n", outputFile);
   }
}

static void ReportResult(FILE *outputFile, const TestResult *result)
{
   char line[256];
   sprintf(line, "  %-34s %10.1f /s   %8.2f ms   %9.1f MB/s",
           result->name, result->operationsPerSecond, result->millisecondsPerOperation,
           result->megabytesPerSecond);
   ReportLine(outputFile, line);
}

static TestResult MakeResult(const char *name, long operationCount, double elapsedSeconds, double bytesPerOperation)
{
   TestResult result;
   double operationsPerSecond = (elapsedSeconds > 0.0) ? ((double)operationCount / elapsedSeconds) : 0.0;
   double bytesPerSecond = operationsPerSecond * bytesPerOperation;

   result.name = name;
   result.operationsPerSecond = operationsPerSecond;
   result.megabytesPerSecond = bytesPerSecond / (double)bytesPerMegabyte;
   result.millisecondsPerOperation = (operationsPerSecond > 0.0) ? (1000.0 / operationsPerSecond) : 0.0;
   return result;
}

/* A window of our own rather than the desktop DC: nothing else repaints into it while we
 * measure, and the numbers are not polluted by another window's WM_PAINT. */
static LRESULT CALLBACK BenchWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
   if (message == WM_DESTROY)
   {
      PostQuitMessage(0);
      return 0;
   }
   if (message == WM_ERASEBKGND)
   {
      return 1;
   }
   return DefWindowProc(window, message, wParam, lParam);
}

static HWND CreateFullScreenWindow(int screenWidth, int screenHeight)
{
   static const char *windowClassName = "gdibenchWindow";
   WNDCLASSA windowClass;
   HINSTANCE moduleHandle = GetModuleHandle(NULL);
   HWND window;

   ZeroMemory(&windowClass, sizeof(windowClass));
   windowClass.lpfnWndProc = BenchWindowProc;
   windowClass.hInstance = moduleHandle;
   windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
   windowClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
   windowClass.lpszClassName = windowClassName;
   RegisterClassA(&windowClass);

   window = CreateWindowExA(WS_EX_TOPMOST, windowClassName, "gdibench", WS_POPUP | WS_VISIBLE,
                            0, 0, screenWidth, screenHeight, NULL, NULL, moduleHandle, NULL);
   return window;
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

/* A DIB section, because that is what an application blits from: a bitmap it drew into
 * itself. A DDB would let the driver keep it in video memory and measure something else. */
static HBITMAP CreateSourceBitmap(HDC referenceDC, int width, int height, void **bitsOut)
{
   BITMAPINFO bitmapInfo;
   HBITMAP bitmap;
   void *bits = NULL;

   ZeroMemory(&bitmapInfo, sizeof(bitmapInfo));
   bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
   bitmapInfo.bmiHeader.biWidth = width;
   bitmapInfo.bmiHeader.biHeight = -height;
   bitmapInfo.bmiHeader.biPlanes = 1;
   bitmapInfo.bmiHeader.biBitCount = 32;
   bitmapInfo.bmiHeader.biCompression = BI_RGB;

   bitmap = CreateDIBSection(referenceDC, &bitmapInfo, DIB_RGB_COLORS, &bits, NULL, 0);
   *bitsOut = bits;
   return bitmap;
}

static void FillSourceBitmap(void *bits, int width, int height)
{
   unsigned long *pixels = (unsigned long *)bits;
   int y;
   if (pixels == NULL)
   {
      return;
   }
   for (y = 0; y < height; y++)
   {
      int x;
      for (x = 0; x < width; x++)
      {
         unsigned long red = (unsigned long)(x & 0xff);
         unsigned long green = (unsigned long)(y & 0xff);
         unsigned long blue = (unsigned long)((x + y) & 0xff);
         pixels[(long)y * width + x] = (red << 16) | (green << 8) | blue;
      }
   }
}

typedef struct
{
   HDC screenDC;
   HDC memoryDC;
   int screenWidth;
   int screenHeight;
   int bitsPerPixel;
   double screenBytes;
   double draggedWindowBytes;
   TimeSource time;
} BenchContext;

static TestResult RunSolidFillTest(BenchContext *context)
{
   HBRUSH brushes[2];
   long iterationCount = 0;
   double startTime = TimeSourceNow(&context->time);
   double elapsed;

   brushes[0] = CreateSolidBrush(RGB(0, 0, 160));
   brushes[1] = CreateSolidBrush(RGB(160, 0, 0));

   for (;;)
   {
      HBRUSH brush = brushes[iterationCount & 1];
      HGDIOBJ previousBrush = SelectObject(context->screenDC, brush);
      PatBlt(context->screenDC, 0, 0, context->screenWidth, context->screenHeight, PATCOPY);
      SelectObject(context->screenDC, previousBrush);
      iterationCount++;
      elapsed = TimeSourceNow(&context->time) - startTime;
      if (elapsed >= secondsPerTest)
      {
         break;
      }
   }

   DeleteObject(brushes[0]);
   DeleteObject(brushes[1]);
   return MakeResult("Vollflaechig fuellen (PatBlt)", iterationCount, elapsed, context->screenBytes);
}

static TestResult RunMemoryToScreenTest(BenchContext *context)
{
   long iterationCount = 0;
   double startTime = TimeSourceNow(&context->time);
   double elapsed;

   for (;;)
   {
      BitBlt(context->screenDC, 0, 0, context->screenWidth, context->screenHeight,
             context->memoryDC, 0, 0, SRCCOPY);
      iterationCount++;
      elapsed = TimeSourceNow(&context->time) - startTime;
      if (elapsed >= secondsPerTest)
      {
         break;
      }
   }
   return MakeResult("Speicher -> Bildschirm (BitBlt)", iterationCount, elapsed, context->screenBytes);
}

static TestResult RunScreenToMemoryTest(BenchContext *context)
{
   long iterationCount = 0;
   double startTime = TimeSourceNow(&context->time);
   double elapsed;

   for (;;)
   {
      BitBlt(context->memoryDC, 0, 0, context->screenWidth, context->screenHeight,
             context->screenDC, 0, 0, SRCCOPY);
      iterationCount++;
      elapsed = TimeSourceNow(&context->time) - startTime;
      if (elapsed >= secondsPerTest)
      {
         break;
      }
   }
   return MakeResult("Bildschirm -> Speicher (Lesen!)", iterationCount, elapsed, context->screenBytes);
}

static TestResult RunScreenToScreenScrollTest(BenchContext *context)
{
   long iterationCount = 0;
   int scrolledHeight = context->screenHeight - scrollStepInPixels;
   double scrolledBytes = (double)context->screenWidth * scrolledHeight * (context->bitsPerPixel / 8);
   double startTime = TimeSourceNow(&context->time);
   double elapsed;

   for (;;)
   {
      BitBlt(context->screenDC, 0, 0, context->screenWidth, scrolledHeight,
             context->screenDC, 0, scrollStepInPixels, SRCCOPY);
      iterationCount++;
      elapsed = TimeSourceNow(&context->time) - startTime;
      if (elapsed >= secondsPerTest)
      {
         break;
      }
   }
   return MakeResult("Bildschirm -> Bildschirm (Rollen)", iterationCount, elapsed, scrolledBytes);
}

/* Dragging a window is a screen-to-screen copy of the window rectangle plus a repaint of
 * what it uncovered. This is the operation the user described as visibly slow. */
static TestResult RunWindowDragTest(BenchContext *context)
{
   long iterationCount = 0;
   int maximumX = context->screenWidth - draggedWindowWidth;
   int maximumY = context->screenHeight - draggedWindowHeight;
   double startTime = TimeSourceNow(&context->time);
   double elapsed;

   for (;;)
   {
      int stepIndex = (int)(iterationCount & 0x3f);
      int sourceX = (maximumX > 0) ? (stepIndex * maximumX / 64) : 0;
      int sourceY = (maximumY > 0) ? (stepIndex * maximumY / 64) : 0;
      int targetX = (sourceX + 1 <= maximumX) ? (sourceX + 1) : sourceX;
      int targetY = (sourceY + 1 <= maximumY) ? (sourceY + 1) : sourceY;

      BitBlt(context->screenDC, targetX, targetY, draggedWindowWidth, draggedWindowHeight,
             context->screenDC, sourceX, sourceY, SRCCOPY);
      iterationCount++;
      elapsed = TimeSourceNow(&context->time) - startTime;
      if (elapsed >= secondsPerTest)
      {
         break;
      }
   }
   return MakeResult("Fenster schieben (400x300)", iterationCount, elapsed, context->draggedWindowBytes);
}

static TestResult RunTextOutTest(BenchContext *context)
{
   static const char *sampleText = "Der schnelle braune Fuchs springt ueber den faulen Hund. 0123456789";
   int sampleLength = lstrlenA(sampleText);
   int lineHeight = 16;
   int lineCount = context->screenHeight / lineHeight;
   long iterationCount = 0;
   double startTime = TimeSourceNow(&context->time);
   double elapsed;

   SetBkMode(context->screenDC, OPAQUE);
   SetTextColor(context->screenDC, RGB(255, 255, 255));
   SetBkColor(context->screenDC, RGB(0, 0, 80));

   for (;;)
   {
      int lineIndex = (int)(iterationCount % lineCount);
      TextOutA(context->screenDC, 0, lineIndex * lineHeight, sampleText, sampleLength);
      iterationCount++;
      elapsed = TimeSourceNow(&context->time) - startTime;
      if (elapsed >= secondsPerTest)
      {
         break;
      }
   }
   return MakeResult("Textzeile ausgeben (TextOut)", iterationCount, elapsed, 0.0);
}

/* The login fade the user named: a half transparent full screen bitmap over the desktop.
 * AlphaBlend reads the destination, so it is the harshest read test GDI has. */
typedef BOOL (WINAPI *AlphaBlendFunction)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);

static int RunAlphaBlendTest(BenchContext *context, TestResult *resultOut)
{
   HMODULE blendModule = LoadLibraryA("msimg32.dll");
   AlphaBlendFunction alphaBlend;
   BLENDFUNCTION blend;
   long iterationCount = 0;
   double startTime;
   double elapsed;

   if (blendModule == NULL)
   {
      return 0;
   }
   alphaBlend = (AlphaBlendFunction)GetProcAddress(blendModule, "AlphaBlend");
   if (alphaBlend == NULL)
   {
      FreeLibrary(blendModule);
      return 0;
   }

   blend.BlendOp = AC_SRC_OVER;
   blend.BlendFlags = 0;
   blend.SourceConstantAlpha = 128;
   blend.AlphaFormat = 0;

   startTime = TimeSourceNow(&context->time);
   for (;;)
   {
      alphaBlend(context->screenDC, 0, 0, context->screenWidth, context->screenHeight,
                 context->memoryDC, 0, 0, context->screenWidth, context->screenHeight, blend);
      iterationCount++;
      elapsed = TimeSourceNow(&context->time) - startTime;
      if (elapsed >= secondsPerTest)
      {
         break;
      }
   }

   FreeLibrary(blendModule);
   *resultOut = MakeResult("Ueberblenden (AlphaBlend)", iterationCount, elapsed, context->screenBytes);
   return 1;
}

/* A number that is too good usually means nothing was drawn. Two guards against that:
 * the clip box of the DC we measure on, and a read-back comparison after the blit. */
static void ReportClipBox(FILE *outputFile, const BenchContext *context)
{
   char line[256];
   RECT clipBox;
   int clipResult = GetClipBox(context->screenDC, &clipBox);

   sprintf(line, "Zeichenbereich  %ld,%ld - %ld,%ld  (GetClipBox = %d)",
           clipBox.left, clipBox.top, clipBox.right, clipBox.bottom, clipResult);
   ReportLine(outputFile, line);
}

/* Reads the screen back into a second DIB and compares it with what was blitted there.
 * Every mismatch means the measurement above was measuring nothing. */
static void ReportReadBackCheck(FILE *outputFile, BenchContext *context, const void *expectedBits)
{
   char line[256];
   HDC verifyDC = CreateCompatibleDC(context->screenDC);
   void *verifyBits = NULL;
   HBITMAP verifyBitmap = CreateSourceBitmap(context->screenDC, context->screenWidth, context->screenHeight, &verifyBits);
   HGDIOBJ previousBitmap = SelectObject(verifyDC, verifyBitmap);
   long mismatchCount = 0;
   long sampleCount = 0;

   BitBlt(context->screenDC, 0, 0, context->screenWidth, context->screenHeight,
          context->memoryDC, 0, 0, SRCCOPY);
   BitBlt(verifyDC, 0, 0, context->screenWidth, context->screenHeight,
          context->screenDC, 0, 0, SRCCOPY);

   if (verifyBits != NULL && expectedBits != NULL)
   {
      const unsigned long *expectedPixels = (const unsigned long *)expectedBits;
      const unsigned long *readBackPixels = (const unsigned long *)verifyBits;
      long pixelCount = (long)context->screenWidth * context->screenHeight;
      long stepBetweenSamples = 997;
      long index;

      for (index = 0; index < pixelCount; index += stepBetweenSamples)
      {
         unsigned long expected = expectedPixels[index] & 0x00ffffff;
         unsigned long readBack = readBackPixels[index] & 0x00ffffff;
         sampleCount++;
         if (expected != readBack)
         {
            mismatchCount++;
         }
      }
   }

   sprintf(line, "Gegenprobe      %ld von %ld Stichproben weichen ab%s",
           mismatchCount, sampleCount,
           (mismatchCount == 0) ? "  -- es wurde wirklich gezeichnet" : "  -- ACHTUNG");
   ReportLine(outputFile, line);

   SelectObject(verifyDC, previousBitmap);
   DeleteDC(verifyDC);
   DeleteObject(verifyBitmap);
}

static void ReportDisplayMode(FILE *outputFile, const BenchContext *context)
{
   char line[256];
   DEVMODEA displayMode;

   sprintf(line, "Bildschirm  %d x %d, %d Bit", context->screenWidth, context->screenHeight, context->bitsPerPixel);
   ReportLine(outputFile, line);

   ZeroMemory(&displayMode, sizeof(displayMode));
   displayMode.dmSize = sizeof(displayMode);
   if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &displayMode))
   {
      sprintf(line, "Modus       %lu x %lu x %lu bei %lu Hz",
              (unsigned long)displayMode.dmPelsWidth, (unsigned long)displayMode.dmPelsHeight,
              (unsigned long)displayMode.dmBitsPerPel, (unsigned long)displayMode.dmDisplayFrequency);
      ReportLine(outputFile, line);
   }

   {
      DISPLAY_DEVICEA displayDevice;
      ZeroMemory(&displayDevice, sizeof(displayDevice));
      displayDevice.cb = sizeof(displayDevice);
      if (EnumDisplayDevicesA(NULL, 0, &displayDevice, 0))
      {
         sprintf(line, "Adapter     %s", displayDevice.DeviceString);
         ReportLine(outputFile, line);
      }
   }
}

int main(int argc, char **argv)
{
   const char *outputPath = (argc > 1) ? argv[1] : "gdibench.txt";
   FILE *outputFile = fopen(outputPath, "w");
   BenchContext context;
   HWND window;
   HBITMAP sourceBitmap;
   HGDIOBJ previousBitmap;
   void *sourceBits = NULL;
   TestResult alphaResult;
   int haveAlphaResult;

   context.screenWidth = GetSystemMetrics(SM_CXSCREEN);
   context.screenHeight = GetSystemMetrics(SM_CYSCREEN);
   TimeSourceInit(&context.time);

   window = CreateFullScreenWindow(context.screenWidth, context.screenHeight);
   if (window == NULL)
   {
      ReportLine(outputFile, "FEHLER: Fenster liess sich nicht anlegen.");
      if (outputFile != NULL)
      {
         fclose(outputFile);
      }
      return 1;
   }
   SetForegroundWindow(window);
   PumpPendingMessages();

   context.screenDC = GetDC(window);
   context.bitsPerPixel = GetDeviceCaps(context.screenDC, BITSPIXEL);
   context.screenBytes = (double)context.screenWidth * context.screenHeight * (context.bitsPerPixel / 8);
   context.draggedWindowBytes = (double)draggedWindowWidth * draggedWindowHeight * (context.bitsPerPixel / 8);

   sourceBitmap = CreateSourceBitmap(context.screenDC, context.screenWidth, context.screenHeight, &sourceBits);
   FillSourceBitmap(sourceBits, context.screenWidth, context.screenHeight);
   context.memoryDC = CreateCompatibleDC(context.screenDC);
   previousBitmap = SelectObject(context.memoryDC, sourceBitmap);

   ReportLine(outputFile, "gdibench -- 2D-Durchsatz des Anzeigetreibers");
   ReportDisplayMode(outputFile, &context);
   ReportClipBox(outputFile, &context);
   ReportReadBackCheck(outputFile, &context, sourceBits);
   ReportLine(outputFile, "");
   ReportLine(outputFile, "  Test                                 Aufrufe        Dauer      Durchsatz");

   {
      TestResult result = RunSolidFillTest(&context);
      ReportResult(outputFile, &result);
   }
   {
      TestResult result = RunMemoryToScreenTest(&context);
      ReportResult(outputFile, &result);
   }
   {
      TestResult result = RunScreenToMemoryTest(&context);
      ReportResult(outputFile, &result);
   }
   {
      TestResult result = RunScreenToScreenScrollTest(&context);
      ReportResult(outputFile, &result);
   }
   {
      TestResult result = RunWindowDragTest(&context);
      ReportResult(outputFile, &result);
   }
   {
      TestResult result = RunTextOutTest(&context);
      ReportResult(outputFile, &result);
   }
   haveAlphaResult = RunAlphaBlendTest(&context, &alphaResult);
   if (haveAlphaResult)
   {
      ReportResult(outputFile, &alphaResult);
   }
   else
   {
      ReportLine(outputFile, "  Ueberblenden (AlphaBlend)          nicht verfuegbar");
   }

   SelectObject(context.memoryDC, previousBitmap);
   DeleteDC(context.memoryDC);
   DeleteObject(sourceBitmap);
   ReleaseDC(window, context.screenDC);
   DestroyWindow(window);
   PumpPendingMessages();

   ReportLine(outputFile, "");
   ReportLine(outputFile, "fertig.");
   if (outputFile != NULL)
   {
      fclose(outputFile);
   }
   return 0;
}
