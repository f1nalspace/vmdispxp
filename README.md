# vmdispxp — XPDM-Anzeigetreiber für QEMU mit qemu-3dfx-ICD

Das NT-Gegenstück zu `vmdisp9x`: ein Anzeigetreiberpaar für Windows 2000/XP auf
**QEMUs Standard-VGA** (`-device VGA`, `PCI\VEN_1234&DEV_1111`), das den
qemu-3dfx-ICD `qmfxgl32.dll` bekanntgibt.

    qemump.sys     Miniport, spricht die Bochs-VBE-Register 0x1CE/0x1CF
    qemudisp.dll   Anzeigetreiber (XPDM), Schattenpuffer fuer 2D, beantwortet OPENGL_GETINFO

    tools/gdibench  2D-Durchsatz von GDI, im Gast gemessen
    tools/ddbench   dasselbe fuer DirectDraw, gegen das eigene ddraw.dll von Windows
    tools/modes     angebotene Bildschirmmodi auflisten und einen davon setzen

**⚠ XPDM, nicht WDDM.** WDDM kam erst mit Vista; XP kennt nur das ältere Modell.

## Wozu

Unter Windows XP fand `opengl32.dll` unseren ICD über keinen der drei
Registrierungswege [325]. Der Weg, der bleibt, ist derselbe wie unter 9x: der
**Anzeigetreiber** wird per `ExtEscape` gefragt. Microsofts Cirrus-Treiber
beantwortet das nicht, und ein quelloffener Treiber, der es könnte, war bis jetzt
nicht in Sicht — VirtualBox' XPDM-Treiber hat den Escape **nicht** [339].

Nebenbei gibt der Treiber freie Auflösungen und 16/32 Bit, was Cirrus unter XP nicht
kann — und seit dem 04.09.2026 ist auch **2D schnell**, siehe den nächsten Abschnitt.

## Herkunft und Lizenz

Der Unterbau stammt aus **ReactOS** (GPL-2.0-or-later), Stand
`b256209ed4f6275e89a96991b5917e8110023dce` vom 25.07.2023:

    miniport/bochsmp.{c,h}   aus win32ss/drivers/miniport/bochs/
    display/*.{c,h}          aus win32ss/drivers/displays/framebuf/

`dd.c` und `ddenable.c` sind nicht übernommen — ReactOS baut sie selbst nicht mit,
`enable.c` hat eigene Stümpfe für `DrvEnableDirectDraw`.

Unter `compat/` liegen Header, die die MinGW-Kette nicht mitbringt. `ddrawint.h`,
`dvp.h`, `d3dnthal.h`, `d3dkmdt.h`, `d3dkmthk.h` und `d3dukmdt.h` kommen aus ReactOS'
SDK (public domain bzw. GPL), `section_attribs.h` ist neu geschrieben. Zwei davon
sind angepasst, beide Stellen tragen einen `qemu-3dfx:`-Kommentar:

- `d3dkmdt.h` — der `#ifndef __REACTOS__`-Block deklariert `POWER_ACTION` und
  `DEVICE_POWER_STATE` erneut, die MinGWs `winnt.h` schon hat. Auf `#if 0` gesetzt.
- `d3dkmthk.h` — benutzt `D3DKMT_DISPLAYPORT_OPERATION_HEADER`, das in ReactOS'
  Kopie nirgends steht. Vorwärtsdeklaration ergänzt.

Von uns selbst sind:

    display/icd.c       der DrvEscape mit QUERYESCSUPPORT und OPENGL_GETINFO
    display/accel.c     der Schattenpuffer und die vierzehn eingehakten Zeichenfunktionen
    common/kmem.c       memcpy und memset, siehe unten -- der Grund, warum es sie gibt
    common/dbgport.h    Diagnose ueber QEMUs Debug-Konsole auf Port 0xE9

Dazu Änderungen in `display/enable.c` (Funktionstabelle), `display/framebuf.h` (PDEV und
Deklarationen), `display/surface.c` (die Fläche ist jetzt der Schatten) und
`miniport/bochsmp.c` (write-combined Abbildung, 16 bpp).

## Bauen

    make

Es braucht **nur die MinGW-Kette**, die auch die qemu-3dfx-Wrapper baut — kein DDK,
kein Open Watcom. Die beiden Importbibliotheken `libvideoprt.a` (Miniport) und
`libwin32k.a` (Anzeigetreiber) liegen samt DDK-Headern in
`/usr/i686-w64-mingw32/`; das Makefile findet sie über den Compiler selbst.

**⚠ Kein SSE, kein MMX.** Kernelcode darf die Register nicht anfassen, der
FPU-Zustand wird für ihn nicht gesichert. Deshalb `-march=i686 -mno-sse -mno-mmx`,
abweichend von den Wrappern, die auf SSE3 zielen.

Gegenprobe am Ergebnis:

    i686-w64-mingw32-objdump -p build/qemump.sys   | grep -E "Subsystem|DLL Name"
    i686-w64-mingw32-objdump -p build/qemudisp.dll | grep -E "Subsystem|DLL Name"

`qemump.sys` muss `ntoskrnl.exe` und `videoprt.sys` importieren, `qemudisp.dll`
**ausschliesslich `win32k.sys`**, beide mit Subsystem **1 (NT native)**.

**⚠ Das ist keine Kosmetik, sondern die Bedingung, unter der der Treiber überhaupt
lädt** [348]. Ein zweiter Importdeskriptor neben `win32k.sys` — und `-lntoskrnl` für
`memcpy` erzeugt genau den — lässt `EngLoadImage` den Anzeigetreiber ablehnen, und
zwar **wortlos**: keine Fehlermeldung, kein Ereignisprotokolleintrag, kein Bluescreen.
Sichtbar wird es nur daran, dass der Bildschirm schwarz bleibt, während der Miniport
weiterläuft. Deshalb `common/kmem.c` statt `-lntoskrnl`, übersetzt mit
`-fno-tree-loop-distribute-patterns`, damit GCC die beiden Schleifen nicht durch
Aufrufe von sich selbst ersetzt.

## Der ICD-Weg

`opengl32.dll` fragt zweimal:

1. `ExtEscape(hdc, QUERYESCSUPPORT, …, OPENGL_GETINFO)` — beantwortet der Treiber das?
2. `ExtEscape(hdc, OPENGL_GETINFO, …)` — 532 Bytes zurück.

Zurück kommt **nicht** der Name der Bibliothek, sondern der Name des
Registrierungsschlüssels darunter. **Unter NT ist das ein Unterschlüssel mit vier
Werten**, nicht ein schlichter Wert wie unter Windows 9x:

    HKLM\Software\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\QEMUFX
        DLL           REG_SZ     qmfxgl32.dll
        Flags         REG_DWORD  3
        Version       REG_DWORD  2
        DriverVersion REG_DWORD  1

Die 532 Bytes sind `4 + 4 + 262 * sizeof(WCHAR)`. Unter 9x steht der Name als CHAR,
unter NT als WCHAR — der einzige Unterschied zwischen den beiden Fassungen. Die
Konstanten `QUERYESCSUPPORT` (8) und `OPENGL_GETINFO` (4353 = 0x1101) kommen aus
`wingdi.h` und `winddi.h`, sind also nicht geraten.

**⚠ `Flags` hat zwei Bits, und beide zählen.**

**Bit 0 ist der Schalter, an dem die ganze Strecke hängt.** Ohne ihn lädt
`opengl32.dll` den ICD, nimmt ihn an — und fragt ihn danach **nie** nach einem
Pixelformat: `__DrvDescribePixelFormat` und `__DrvSetPixelFormat` prüfen das Bit und
fallen sonst auf Windows' eigene, generische Formate zurück. Die tragen
`PFD_GENERIC_FORMAT`, und damit geht `wglCreateContext` zwingend in die
Software-Umsetzung. Genau das war der offene Punkt aus `docs/LOG.md` [350]/[352];
gelesen wurde er aus XPs eigenem `opengl32.dll`, siehe [427].

**Bit 1 kostet ein Drittel der Bildrate, wenn es fehlt.** `__DrvSwapBuffers` ruft sonst vor
**jedem** `SwapBuffers` ein `glFinish()`:

    5f0ec4f7:  test al,0x2       ; Flags & 2
    5f0ec4f9:  jne  0x5f0ec500   ; gesetzt: ueberspringen
    5f0ec4fb:  call 0x5f0d3fa8   ; _glFinish@0
    5f0ec503:  call DWORD PTR [esi+0x54]   ; ICD->DrvSwapBuffers

Über die Gerätegrenze ist das ein voller Rundlauf, der auf die GPU wartet. Gemessen mit
`glcube`: **7497,4 FPS mit `Flags=1` gegen 11414,8 FPS mit `Flags=3`**, bei unverändert
bestandenem Transparenztest. Siehe [432].

**⚠ `Version` und `DriverVersion` müssen wiederholen, was der Escape ausgibt** — die
beiden ersten DWORDs aus `display/icd.c`. `opengl32.dll` vergleicht sie und verwirft den
Treiber bei Abweichung. `Version` **muss** dabei 2 sein, ein anderer Wert wird gar nicht
erst geprüft.

Fehlt der Unterschlüssel, fällt `opengl32.dll` auf die 9x-Schreibweise zurück — denselben
Namen als `REG_SZ`-Wert im Schlüssel darüber — und setzt `Flags` dann fest auf 0. Der Weg
trägt also, führt aber in die Software.

Die INF setzt beides selbst. **Zum Abschalten für eine Gegenprobe genügt es, den
Unterschlüssel `QEMUFX` zu löschen oder seinen `Flags`-Wert auf 0 zu setzen.**

## Stand

**Der Treiber läuft unter Windows XP** [345], [349], und ist seit [355] die **Vorgabe**
in `vm/winxp.sh` — `--vga cirrus` ist der Rückweg. XP nimmt die INF über die automatische
Suche an, der Desktop steht bei **1280×768 in 32 Bit**, Fenster und Menüs zeichnen sauber.

**⭐ 2D ist seit dem 04.09.2026 schnell** — und zwar durch zwei Änderungen, die beide
nichts beschleunigen, sondern nur den Speicher wechseln, auf dem GDI arbeitet. Gemessen
mit `tools/gdibench`, Einzelheiten in `docs/LOG.md` [381]–[386].

1. **Der Bildspeicher wird write-combined abgebildet** (`VIDEO_MEMORY_SPACE_P6CACHE` in
   `miniport/bochsmp.c`) statt ungecacht. Auf einem AMD-Host mit NPT gilt der Speichertyp
   des Gastes wirklich — ungecacht heißt dort ungecacht, und das waren **41 MB/s**.
2. **GDI zeichnet in einen Schattenpuffer im Systemspeicher** (`display/accel.c`), und der
   Treiber kopiert nach jeder Zeichenoperation nur das berührte Rechteck in den
   Bildspeicher. Das erledigt die Lesezugriffe, die write-combined nicht billiger macht.

| 1280×768×32 | vorher | nur write-combined | + Schattenpuffer |
|---|---|---|---|
| Vollflächig füllen | 90,18 ms | 0,20 ms | 0,43 ms |
| Bildschirm → Speicher | 81,70 ms | 9,71 ms | 0,09 ms |
| Fenster schieben | 17,02 ms | 3,23 ms | **0,07 ms** |
| Überblenden (`AlphaBlend`) | 261,37 ms | 253,78 ms | **2,61 ms** |

**⚠ Die Hakenliste muss vollständig bleiben.** Eingehakt sind alle vierzehn
Zeichenfunktionen des DDI. Eine, die fehlt, ist ein Weg, auf dem GDI den Schatten ändert,
ohne dass der Treiber davon erfährt — diese Pixel kämen nie auf den Bildschirm. Wer eine
Funktion hinzufügt, trägt sie in `QEMUDISP_SHADOW_HOOKS` **und** in die Funktionstabelle
in `display/enable.c` ein.

**DirectDraw braucht dafür nichts eigenes.** Der Treiber meldet keine DirectDraw-Rückrufe,
DirectDraw legt seine Flächen deshalb in den Systemspeicher und bringt sie über GDI auf
den Schirm — also über dieselben Haken, mit einem Kopiervorgang je Bild. Gemessen mit
`tools/ddbench`: **1565 volle Bilder je Sekunde** bei 1280×720×32, 7790 bei 640×480×16.
Ein eigener HAL wurde bewusst nicht gebaut, die Begründung steht in `docs/LOG.md` [385].

**Alle vier Passthrough-Strecken sind darauf gemessen** [355]: OpenGL meldet dieselbe
RTX 3090, Direct3D 8 dieselben 64,1 FPS, DirectDraw 9.576,7 gegen 9.739,1 FPS. Glide
liegt mit 16.647,9 gegen 18.612,0 FPS **10,5 % niedriger, ungeklärt und nur einmal
gemessen.**

**Angeboten werden 16 und 32 bpp.** Die 16 Bit kamen erst mit [355] dazu — ohne sie
scheitert jedes Spiel, das `SetDisplayMode` auf 640×480×16 ruft, mit `E_NOTIMPL`.
8 bpp fehlt weiterhin; das bräuchte Palettenbehandlung im Miniport.

**⚠ Die Auflösungsliste kürzt Windows am Monitor-EDID, nicht der Treiber** [386]. Ohne
Zutun endet sie bei 1920×1080 — 1600×1200 fehlt, obwohl es **weniger** Bildspeicher
braucht. `sh vm/winxp.sh --vga-res 2560x1440` gibt dem Gerät ein anderes EDID mit, und
1600×1200 sowie 2560×1440 erscheinen. `--no-edid` zeigt alles, was der Treiber anbietet.

**Der ICD-Escape wird bedient**, gemessen über die Debug-Konsole:

    qemudisp: DrvEscape 0x00000008
    qemudisp: QUERYESCSUPPORT for 0x00001101
    qemudisp: OPENGL_GETINFO is supported
    qemudisp: DrvEscape 0x00001101
    qemudisp: handed out the ICD name, bytes 0x00000214

Und `qmfxgl32.dll` **wird daraufhin geladen** — belegt an Dr. Watsons Modulliste, in
der sie bei den Versuchen vom 01.09. [325] nie auftauchte.

**⚠ Geprüft und verworfen: die Pixelformat-DDI im Anzeigetreiber** [352]. `framebuf`
hat `DrvDescribePixelFormat`, `DrvSetPixelFormat` und `DrvSwapBuffers` nicht; nachgerüstet
werden sie zwar gerufen, aber `opengl32.dll` fragt dann den ICD-Escape **gar nicht mehr**
(39 Aufrufe → 0) und OpenGL scheitert ganz, statt in Software zu laufen. Offenbar hält
opengl32 einen Treiber mit eigenen `PFD_SUPPORT_OPENGL`-Formaten für einen, der OpenGL
selbst umsetzt. Der Code bleibt in `display/pixelformat.c` und ist mit
`make PIXELFORMATS=1` wieder einschaltbar.

**⭐ Und seit dem 09.09.2026 rendert `opengl32.dll` darüber auch wirklich** [427]–[429].
Der frühere offene Punkt — „lädt den ICD, benutzt ihn aber nicht" — waren die vier
Registrierungswerte oben, nicht fehlender Code. Belegt an zwei Stellen:

    glcube ohne eine Datei von uns im Ordner   NVIDIA GeForce RTX 3090/PCIe/SSE2, 7497,4 FPS
    Quake III Arena, opengl32.dll umbenannt    "255 PFDs found", "hardware acceleration found"

Quake III sagt den Pfad selbst: `LoadLibrary( 'C:\WINDOWS\system32\opengl32.dll' )`.

**Der ICD-Weg kostet nichts mehr** [432]. Er lag anfangs ein Drittel zurück; die Ursache war
das erzwungene `glFinish` vor jedem `SwapBuffers`, das `Flags` Bit 1 abschaltet. Mit
`Flags=3` liefert `glcube` über den ICD **11414,8 FPS** gegen 10856,5 FPS mit unserer
`opengl32.dll` im Ordner, im selben Lauf gemessen — die beiden Wege sind gleichauf.

### Diagnose

    make DEBUGCON=1                       Treiber mit Ausgabe auf Port 0xE9
    sh vm/winxp.sh --debugcon <datei>     die Ausgabe landet in dieser Datei

Gegenprobe, dass die Kette selbst steht — über QEMUs Monitor in den Port schreiben:

    o /b 0xe9 0x41

**Der Bildspeicher lässt sich von außen lesen**, was beim schwarzen Bild die
entscheidende Messung war (`info pci` liefert die Adresse):

    pmemsave 0xfd000000 262144 /tmp/vram.bin

**⚠ Ungeprüft:** ob die GL-Fensterübergabe des Hosts (`mesa_gl_takeover`, Fork-Commit
`7fd9122`) den Wechsel von `cirrus-vga` auf `-device VGA` unbeschadet übersteht — die
vier belegten Strecken sind unter diesem Anzeigegerät noch nicht nachgefahren.
