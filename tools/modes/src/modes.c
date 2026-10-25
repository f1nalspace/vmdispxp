/*
 * modes -- lists the display modes the driver offers, and switches to one.
 *
 * Without arguments it writes the whole list; with a resolution it switches the desktop
 * there. Both are needed to work on a display driver without clicking through the
 * display applet, which is awkward to drive over QMP and, at the wrong moment, drops back
 * to the old mode by itself.
 *
 * With -check it switches nothing: for each common game resolution it says whether Windows lists it, whether the driver offers it
 * (EDS_RAWMODE, before Windows drops what the monitor supposedly cannot show), and what ChangeDisplaySettings answers with CDS_TEST.
 * It names the refresh rates of each resolution too, as Windows lists them and as the driver offers them.
 *
 * A switch takes a refresh rate after the depth; without one Windows chooses it.
 *
 * With -pruning 0 or 1 it writes PruningMode into the display adapter's settings -- the box "Hide modes that this monitor cannot display"
 * of the display applet -- and then lists and checks as -check does.
 *
 * Output is German; the comments are English.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

static FILE *reportFile;

#define CHECK_SWITCH          "-check"
#define PRUNING_SWITCH        "-pruning"
/* EnumDisplayDevices names the adapter's settings key from the registry's root; RegOpenKeyEx wants it below HKEY_LOCAL_MACHINE. */
#define REGISTRY_MACHINE_PREFIX "\\Registry\\Machine\\"
#define CHECK_BITS_PER_PIXEL  32

typedef struct CheckedResolution
{
   DWORD width;
   DWORD height;
} CheckedResolution;

/* What games of the time offer in 4:3, 5:4, 16:10 and 16:9, up to 4K. */
static const CheckedResolution checkedResolutions[] =
{
   { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 }, { 1280, 960 }, { 1280, 1024 },
   { 1360, 768 }, { 1366, 768 }, { 1368, 768 }, { 1400, 1050 }, { 1440, 900 }, { 1600, 900 }, { 1600, 1200 }, { 1680, 1050 },
   { 1920, 1080 }, { 1920, 1200 }, { 1920, 1440 }, { 2048, 1152 }, { 2048, 1536 }, { 2560, 1440 }, { 2560, 1600 }, { 2560, 2048 },
   { 3200, 2400 }, { 3840, 2160 },
};

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

static const char *ChangeDisplaySettingsResultText(LONG result)
{
   switch (result)
   {
      case DISP_CHANGE_SUCCESSFUL: return "successful";
      case DISP_CHANGE_RESTART:    return "restart needed";
      case DISP_CHANGE_BADFLAGS:   return "invalid flags";
      case DISP_CHANGE_BADPARAM:   return "invalid parameter";
      case DISP_CHANGE_FAILED:     return "rejected by the driver";
      case DISP_CHANGE_BADMODE:    return "mode is not offered";
      case DISP_CHANGE_NOTUPDATED: return "registry not written";
      default:                     return "unknown";
   }
}

static void ListModes(void)
{
   char line[256];
   DEVMODEA displayMode;
   DWORD modeIndex = 0;
   DWORD listedCount = 0;

   ZeroMemory(&displayMode, sizeof(displayMode));
   displayMode.dmSize = sizeof(displayMode);
   if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &displayMode))
   {
      sprintf(line, "Current     %lu x %lu x %lu at %lu Hz",
              (unsigned long)displayMode.dmPelsWidth, (unsigned long)displayMode.dmPelsHeight,
              (unsigned long)displayMode.dmBitsPerPel, (unsigned long)displayMode.dmDisplayFrequency);
      ReportLine(line);
   }

   {
      DISPLAY_DEVICEA displayDevice;
      ZeroMemory(&displayDevice, sizeof(displayDevice));
      displayDevice.cb = sizeof(displayDevice);
      if (EnumDisplayDevicesA(NULL, 0, &displayDevice, 0))
      {
         sprintf(line, "Adapter     %s", displayDevice.DeviceString);
         ReportLine(line);
      }
      ZeroMemory(&displayDevice, sizeof(displayDevice));
      displayDevice.cb = sizeof(displayDevice);
      if (EnumDisplayDevicesA(NULL, 1, &displayDevice, 0))
      {
         sprintf(line, "Secondary   %s", displayDevice.DeviceString);
         ReportLine(line);
      }
   }

   ReportLine("");
   ReportLine("Modes offered:");
   for (;;)
   {
      ZeroMemory(&displayMode, sizeof(displayMode));
      displayMode.dmSize = sizeof(displayMode);
      if (!EnumDisplaySettingsA(NULL, modeIndex, &displayMode))
      {
         break;
      }
      sprintf(line, "  %4lu x %4lu  x %2lu bit  %3lu Hz",
              (unsigned long)displayMode.dmPelsWidth, (unsigned long)displayMode.dmPelsHeight,
              (unsigned long)displayMode.dmBitsPerPel, (unsigned long)displayMode.dmDisplayFrequency);
      ReportLine(line);
      listedCount++;
      modeIndex++;
   }
   sprintf(line, "%lu modes in total.", (unsigned long)listedCount);
   ReportLine(line);
}

static int SwitchMode(int width, int height, int bitsPerPixel, int refreshRate)
{
   char line[256];
   char rateText[32] = "";
   DEVMODEA displayMode;
   LONG testResult;
   LONG applyResult;

   ZeroMemory(&displayMode, sizeof(displayMode));
   displayMode.dmSize = sizeof(displayMode);
   displayMode.dmPelsWidth = (DWORD)width;
   displayMode.dmPelsHeight = (DWORD)height;
   displayMode.dmBitsPerPel = (DWORD)bitsPerPixel;
   displayMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
   /* A rate of 0 leaves the choice to Windows. */
   if (refreshRate > 0)
   {
      displayMode.dmDisplayFrequency = (DWORD)refreshRate;
      displayMode.dmFields |= DM_DISPLAYFREQUENCY;
      sprintf(rateText, " at %d Hz", refreshRate);
   }

   testResult = ChangeDisplaySettingsA(&displayMode, CDS_TEST);
   sprintf(line, "Test        %d x %d x %d%s  ->  %s (%ld)",
           width, height, bitsPerPixel, rateText, ChangeDisplaySettingsResultText(testResult), (long)testResult);
   ReportLine(line);
   if (testResult != DISP_CHANGE_SUCCESSFUL)
   {
      return 1;
   }

   /* CDS_UPDATEREGISTRY makes the mode survive the next start; without it the desktop
    * falls back as soon as anything re-reads the settings. */
   applyResult = ChangeDisplaySettingsA(&displayMode, CDS_UPDATEREGISTRY);
   sprintf(line, "Switch      %d x %d x %d%s  ->  %s (%ld)",
           width, height, bitsPerPixel, rateText, ChangeDisplaySettingsResultText(applyResult), (long)applyResult);
   ReportLine(line);
   return (applyResult == DISP_CHANGE_SUCCESSFUL) ? 0 : 1;
}

/* Whether a mode of this size and depth is among the modes enumerated with these flags (0 or EDS_RAWMODE). */
static BOOL IsModeEnumerated(DWORD width, DWORD height, DWORD bitsPerPixel, DWORD enumerationFlags)
{
   DEVMODEA displayMode;
   DWORD modeIndex;

   for (modeIndex = 0; ; modeIndex++)
   {
      BOOL enumerated;

      ZeroMemory(&displayMode, sizeof(displayMode));
      displayMode.dmSize = sizeof(displayMode);
      enumerated = EnumDisplaySettingsExA(NULL, modeIndex, &displayMode, enumerationFlags);
      if (!enumerated)
      {
         return FALSE;
      }
      if (displayMode.dmPelsWidth == width && displayMode.dmPelsHeight == height && displayMode.dmBitsPerPel == bitsPerPixel)
      {
         return TRUE;
      }
   }
}

/* The refresh rates of the modes of this size and depth enumerated with these flags (0 or EDS_RAWMODE), as " 60 120 144"; " none" without one. */
static void DescribeRates(DWORD width, DWORD height, DWORD bitsPerPixel, DWORD enumerationFlags, char *text, size_t textSize)
{
   DEVMODEA displayMode;
   DWORD modeIndex;
   size_t used = 0;

   text[0] = '\0';
   for (modeIndex = 0; ; modeIndex++)
   {
      BOOL enumerated;
      ZeroMemory(&displayMode, sizeof(displayMode));
      displayMode.dmSize = sizeof(displayMode);
      enumerated = EnumDisplaySettingsExA(NULL, modeIndex, &displayMode, enumerationFlags);
      if (!enumerated)
      {
         break;
      }
      if (displayMode.dmPelsWidth == width && displayMode.dmPelsHeight == height && displayMode.dmBitsPerPel == bitsPerPixel && used + 8 < textSize)
      {
         int written = sprintf(text + used, " %lu", (unsigned long)displayMode.dmDisplayFrequency);
         used += (size_t)written;
      }
   }
   if (used == 0)
   {
      strcpy(text, " none");
   }
}

static void CheckResolutions(void)
{
   const size_t resolutionCount = sizeof(checkedResolutions) / sizeof(checkedResolutions[0]);
   /* Room for both lists of rates beside the rest of the line. */
   char line[512];
   size_t resolutionIndex;

   ReportLine("");
   sprintf(line, "Check at %d bit: listed = EnumDisplaySettings, raw = EDS_RAWMODE (the driver's own list), test = ChangeDisplaySettings(CDS_TEST)", CHECK_BITS_PER_PIXEL);
   ReportLine(line);
   for (resolutionIndex = 0; resolutionIndex < resolutionCount; resolutionIndex++)
   {
      const CheckedResolution *resolution = &checkedResolutions[resolutionIndex];
      BOOL listed = IsModeEnumerated(resolution->width, resolution->height, CHECK_BITS_PER_PIXEL, 0);
      BOOL raw = IsModeEnumerated(resolution->width, resolution->height, CHECK_BITS_PER_PIXEL, EDS_RAWMODE);
      DEVMODEA displayMode;
      LONG testResult;
      const char *testText;
      char listedRates[96];
      char rawRates[96];

      ZeroMemory(&displayMode, sizeof(displayMode));
      displayMode.dmSize = sizeof(displayMode);
      displayMode.dmPelsWidth = resolution->width;
      displayMode.dmPelsHeight = resolution->height;
      displayMode.dmBitsPerPel = CHECK_BITS_PER_PIXEL;
      displayMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
      testResult = ChangeDisplaySettingsA(&displayMode, CDS_TEST);
      testText = ChangeDisplaySettingsResultText(testResult);
      DescribeRates(resolution->width, resolution->height, CHECK_BITS_PER_PIXEL, 0, listedRates, sizeof(listedRates));
      DescribeRates(resolution->width, resolution->height, CHECK_BITS_PER_PIXEL, EDS_RAWMODE, rawRates, sizeof(rawRates));
      sprintf(line, "  %4lu x %4lu   listed %-3s   raw %-3s   test %s (%ld)   rates listed%s, raw%s", (unsigned long)resolution->width, (unsigned long)resolution->height, listed ? "yes" : "no", raw ? "yes" : "no", testText, (long)testResult, listedRates, rawRates);
      ReportLine(line);
   }
}

/* Writes PruningMode into the first display adapter's settings key: 1 hides the modes the monitor supposedly cannot show, 0 lists every mode of the driver. */
static int SetPruningMode(DWORD pruningMode)
{
   const size_t prefixLength = sizeof(REGISTRY_MACHINE_PREFIX) - 1;
   char line[512];
   DISPLAY_DEVICEA displayDevice;
   const char *relativeKey;
   HKEY settingsKey;
   LONG openResult;
   LONG setResult;
   BOOL found;

   ZeroMemory(&displayDevice, sizeof(displayDevice));
   displayDevice.cb = sizeof(displayDevice);
   found = EnumDisplayDevicesA(NULL, 0, &displayDevice, 0);
   if (!found)
   {
      ReportLine("Pruning     no display device");
      return 1;
   }
   relativeKey = displayDevice.DeviceKey;
   if (_strnicmp(relativeKey, REGISTRY_MACHINE_PREFIX, prefixLength) == 0)
   {
      relativeKey += prefixLength;
   }
   openResult = RegOpenKeyExA(HKEY_LOCAL_MACHINE, relativeKey, 0, KEY_SET_VALUE, &settingsKey);
   if (openResult != ERROR_SUCCESS)
   {
      sprintf(line, "Pruning     cannot open HKLM\\%s (%ld)", relativeKey, (long)openResult);
      ReportLine(line);
      return 1;
   }
   setResult = RegSetValueExA(settingsKey, "PruningMode", 0, REG_DWORD, (const BYTE *)&pruningMode, sizeof(pruningMode));
   RegCloseKey(settingsKey);
   sprintf(line, "Pruning     PruningMode = %lu in HKLM\\%s  ->  %s (%ld)", (unsigned long)pruningMode, relativeKey, setResult == ERROR_SUCCESS ? "written" : "failed", (long)setResult);
   ReportLine(line);
   return setResult == ERROR_SUCCESS ? 0 : 1;
}

int main(int argc, char **argv)
{
   const char *outputPath = "modes.txt";
   int argumentIndex = 1;
   int width = 0;
   int height = 0;
   int bitsPerPixel = 32;
   int refreshRate = 0;

   /* modes.exe [ausgabedatei] [breite hoehe [bits [hz]]], modes.exe [ausgabedatei] -check, or modes.exe [ausgabedatei] -pruning 0|1 */
   if (argc > argumentIndex && argv[argumentIndex][0] != '\0' && (argv[argumentIndex][0] < '0' || argv[argumentIndex][0] > '9'))
   {
      outputPath = argv[argumentIndex];
      argumentIndex++;
   }
   if (argc > argumentIndex + 1 && strcmp(argv[argumentIndex], PRUNING_SWITCH) == 0)
   {
      int failed;
      DWORD pruningMode = (DWORD)atoi(argv[argumentIndex + 1]);
      reportFile = fopen(outputPath, "w");
      ReportLine("modes -- display modes of the display driver");
      failed = SetPruningMode(pruningMode);
      ReportLine("");
      ListModes();
      CheckResolutions();
      if (reportFile != NULL)
      {
         fclose(reportFile);
      }
      return failed;
   }

   if (argc > argumentIndex + 1)
   {
      width = atoi(argv[argumentIndex]);
      height = atoi(argv[argumentIndex + 1]);
      argumentIndex += 2;
      if (argc > argumentIndex)
      {
         bitsPerPixel = atoi(argv[argumentIndex]);
      }
      if (argc > argumentIndex + 1)
      {
         refreshRate = atoi(argv[argumentIndex + 1]);
      }
   }

   reportFile = fopen(outputPath, "w");
   ReportLine("modes -- display modes of the display driver");

   if (argc > argumentIndex && strcmp(argv[argumentIndex], CHECK_SWITCH) == 0)
   {
      ListModes();
      CheckResolutions();
      if (reportFile != NULL)
      {
         fclose(reportFile);
      }
      return 0;
   }

   if (width > 0 && height > 0)
   {
      int failed = SwitchMode(width, height, bitsPerPixel, refreshRate);
      ReportLine("");
      ListModes();
      if (reportFile != NULL)
      {
         fclose(reportFile);
      }
      return failed;
   }

   ListModes();
   if (reportFile != NULL)
   {
      fclose(reportFile);
   }
   return 0;
}
