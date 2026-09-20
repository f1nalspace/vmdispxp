# vmdispxp — XPDM display driver for QEMU, with the qemu-3dfx OpenGL ICD

An XPDM display driver pair for Windows 2000/XP guests on **QEMU's standard VGA** (`-device VGA`, `PCI\VEN_1234&DEV_1111`). It offers free resolutions at 16 and 32 bpp, accelerates 2D through a shadow buffer, and announces the qemu-3dfx OpenGL ICD `fvm3dx32.dll` to Windows.

This is the NT counterpart to `vmdisp9x`, which does the same job for Windows 9x.

**XPDM, not WDDM.** WDDM arrived with Vista; XP only knows the older model.

## Origin

This driver is built on **ReactOS** code and stays under its licence, **GPL-2.0-or-later**. Taken from ReactOS commit `b256209ed4f6275e89a96991b5917e8110023dce` of 25 July 2023:

| Taken over | From | Copyright |
|---|---|---|
| `miniport/bochsmp.{c,h}` | `win32ss/drivers/miniport/bochs/` | Hervé Poussineau, 2022 |
| `display/*.{c,h}` | `win32ss/drivers/displays/framebuf/` | Filip Navara, 2004, and the ReactOS authors |
| `compat/*.h` | ReactOS SDK headers the MinGW toolchain does not ship | |

`dd.c` and `ddenable.c` were not taken over — ReactOS does not build them either, and `enable.c` carries its own stubs for `DrvEnableDirectDraw`.

Added here are `display/icd.c` (the ICD escape), `display/accel.c` (the shadow buffer), `common/kmem.c` and `common/dbgport.h`. Changed are `display/enable.c`, `display/framebuf.h`, `display/surface.c` and `miniport/bochsmp.c`.

Two `compat/` headers needed a fix for the MinGW toolchain, both marked with a `qemu-3dfx:` comment: `d3dkmdt.h` redeclares `POWER_ACTION` and `DEVICE_POWER_STATE`, which MinGW's `winnt.h` already has, and `d3dkmthk.h` uses a type that its own copy never declares.

## What it is for

Under Windows XP, `opengl32.dll` does not find an OpenGL ICD through the registry alone — it asks the **display driver**, through `ExtEscape`. Microsoft's Cirrus driver does not answer that, and VirtualBox's XPDM driver does not implement the escape either. Without a driver that answers it, every OpenGL application falls back to software rendering.

Cirrus under XP also offers neither free resolutions nor 16 bpp, and leaves 2D unaccelerated.

## Building

    make

Only the **i686 MinGW cross toolchain** is needed — no DDK, no Open Watcom. The import libraries `libvideoprt.a` and `libwin32k.a` ship with it.

**No SSE, no MMX.** Kernel code must not touch those registers, because the FPU state is not saved for it. Hence `-march=i686 -mno-sse -mno-mmx`.

**A display driver may import from `win32k.sys` and from nothing else.** Linking `-lntoskrnl` — for `memcpy`, say — creates a second import descriptor, and `EngLoadImage` then rejects the driver **silently**: no error, no event log entry, no bluescreen, just a black screen while the miniport keeps running. That is why `common/kmem.c` provides `memcpy` and `memset`, compiled with `-fno-tree-loop-distribute-patterns` so GCC does not turn the two loops into calls to themselves. Check the result:

    i686-w64-mingw32-objdump -p build/qemump.sys   | grep -E "Subsystem|DLL Name"
    i686-w64-mingw32-objdump -p build/qemudisp.dll | grep -E "Subsystem|DLL Name"

`qemump.sys` must import `ntoskrnl.exe` and `videoprt.sys`, `qemudisp.dll` **only** `win32k.sys`, both with subsystem 1 (NT native).

## The OpenGL ICD

`opengl32.dll` asks the driver twice: `QUERYESCSUPPORT` for `OPENGL_GETINFO`, then `OPENGL_GETINFO` itself. What comes back is not the name of the library but the name of a registry subkey. **On NT that subkey carries four values**, where Windows 9x has a plain string value instead:

    HKLM\Software\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\FVM3DX
        DLL           REG_SZ     fvm3dx32.dll
        Flags         REG_DWORD  3
        Version       REG_DWORD  2
        DriverVersion REG_DWORD  1

**Both bits of `Flags` matter.** Without **bit 0**, Windows never asks the ICD for a pixel format, hands the application its own generic formats, and OpenGL ends up in software. Without **bit 1**, `__DrvSwapBuffers` calls `glFinish()` before every `SwapBuffers` — a full round trip across the device boundary, costing about a third of the frame rate.

`Version` and `DriverVersion` must repeat what the escape returns, and `Version` must be 2. The INF sets all of it. To turn the ICD off for a counter-test, delete the `FVM3DX` subkey or set its `Flags` to 0.

## Status

The driver runs under Windows XP with the desktop at 1280×768 in 32 bpp; XP accepts the INF through its own hardware search.

**2D is fast**, through two changes that accelerate nothing but move the memory GDI works on. The frame buffer is mapped write-combined (`VIDEO_MEMORY_SPACE_P6CACHE`) instead of uncached, and GDI draws into a shadow buffer in system memory, from which the driver copies only the touched rectangle after each operation. Dragging a window went from 17.02 ms to 0.07 ms, an `AlphaBlend` fade from 261.37 ms to 2.61 ms.

**The hook list has to stay complete.** All fourteen drawing functions of the DDI are hooked. A missing one is a path on which GDI changes the shadow without the driver noticing, and those pixels would never reach the screen. Anything added goes into `QEMUDISP_SHADOW_HOOKS` **and** into the function table in `display/enable.c`.

**DirectDraw needs nothing of its own.** The driver reports no DirectDraw callbacks, so DirectDraw puts its surfaces in system memory and reaches the screen through the same hooks — measured at 1565 full frames per second at 1280×720×32.

**16 and 32 bpp are offered.** 8 bpp is missing and would need palette handling in the miniport. The resolution list is cut short by the monitor EDID rather than by the driver.

**Tried and rejected: the pixel format DDI in the display driver.** With `DrvDescribePixelFormat` and `DrvSetPixelFormat` present, `opengl32.dll` stops asking for the ICD escape altogether and OpenGL fails outright instead of falling back to software. The code stays in `display/pixelformat.c` behind `make PIXELFORMATS=1`.

## Diagnostics

    make DEBUGCON=1        the drivers write to QEMU's debug console on port 0xE9

The frame buffer can be read from outside, which is the decisive measurement when the screen stays black (`info pci` gives the address):

    pmemsave 0xfd000000 262144 /tmp/vram.bin

## Content

    miniport/        qemump.sys   — miniport, drives the Bochs VBE registers 0x1CE/0x1CF
    display/         qemudisp.dll — XPDM display driver, shadow buffer, ICD escape
    common/          memcpy and memset, and the debug console
    compat/          DDK headers the MinGW toolchain does not ship
    inf/fvm3dx.inf   installs both and registers the ICD
    tools/gdibench   GDI throughput, measured inside the guest
    tools/ddbench    the same for DirectDraw
    tools/modes      list the offered display modes and set one
