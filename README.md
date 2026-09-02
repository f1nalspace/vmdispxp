# vmdispxp — XPDM-Anzeigetreiber für QEMU mit qemu-3dfx-ICD

Das NT-Gegenstück zu `vmdisp9x`: ein Anzeigetreiberpaar für Windows 2000/XP auf
**QEMUs Standard-VGA** (`-device VGA`, `PCI\VEN_1234&DEV_1111`), das den
qemu-3dfx-ICD `qmfxgl32.dll` bekanntgibt.

    qemump.sys     Miniport, spricht die Bochs-VBE-Register 0x1CE/0x1CF
    qemudisp.dll   Anzeigetreiber (XPDM), beantwortet OPENGL_GETINFO

**⚠ XPDM, nicht WDDM.** WDDM kam erst mit Vista; XP kennt nur das ältere Modell.

## Wozu

Unter Windows XP fand `opengl32.dll` unseren ICD über keinen der drei
Registrierungswege [325]. Der Weg, der bleibt, ist derselbe wie unter 9x: der
**Anzeigetreiber** wird per `ExtEscape` gefragt. Microsofts Cirrus-Treiber
beantwortet das nicht, und ein quelloffener Treiber, der es könnte, war bis jetzt
nicht in Sicht — VirtualBox' XPDM-Treiber hat den Escape **nicht** [339].

Nebenbei löst der Treiber das zweite Problem aus [336]: QEMUs Cirrus-Emulation ist
zäh. Hier liegt der Bildspeicher linear, ohne Bankumschaltung.

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
    common/kmem.c       memcpy und memset, siehe unten -- der Grund, warum es sie gibt
    common/dbgport.h    Diagnose ueber QEMUs Debug-Konsole auf Port 0xE9

Dazu je eine Zeile in `display/enable.c` (Funktionstabelle) und `display/framebuf.h`
(Deklaration).

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
Registrierungsschlüssels darunter:

    HKLM\Software\Microsoft\Windows\CurrentVersion\OpenGLdrivers\QEMUFX = "qmfxgl32.dll"

Die 532 Bytes sind `4 + 4 + 262 * sizeof(WCHAR)`. Unter 9x steht der Name als CHAR,
unter NT als WCHAR — der einzige Unterschied zwischen den beiden Fassungen. Die
Konstanten `QUERYESCSUPPORT` (8) und `OPENGL_GETINFO` (4353 = 0x1101) kommen aus
`wingdi.h` und `winddi.h`, sind also nicht geraten.

Die INF setzt den Schlüssel selbst. **Zum Abschalten für eine Gegenprobe genügt es,
den Wert `QEMUFX` zu löschen** — dann fällt Windows auf seine Software-Umsetzung
zurück.

## Stand

**Der Treiber läuft unter Windows XP** [345], [349]. XP nimmt die INF über die
automatische Suche an, der Desktop steht bei **1280×768 in 32 Bit**, Fenster und Menüs
zeichnen sauber — statt der zähen Cirrus-Emulation aus [336].

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

**⚠ Offen: `opengl32.dll` rendert trotzdem in Software.** `wglgears` liefert 10–15 FPS
und erscheint im QMP-Abzug des Gastes, also aus dem Gastspeicher statt vom Host.
`DrvValidateVersion` im ICD wird **nie gerufen** — nachgewiesen mit einer MessageBox,
nicht nur mit einem Log. Was `opengl32.dll` zwischen `LoadLibrary` und der ersten
Treiberfunktion sonst noch prüft, ist von hier aus nicht einsehbar. Einzelheiten in
`docs/LOG.md` [350], [351].

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
