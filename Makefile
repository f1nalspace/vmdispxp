# XPDM display driver pair for QEMU's standard VGA (PCI 1234:1111).
#
# Sources come from ReactOS (GPL-2.0-or-later); see README.md for the exact
# revision and for what we changed. Built with the same MinGW cross toolchain
# that builds the qemu-3dfx wrappers -- no DDK and no Open Watcom needed.

CROSS_PREFIX ?= i686-w64-mingw32-
CC            = $(CROSS_PREFIX)gcc
STRIP         = $(CROSS_PREFIX)strip

BUILD ?= build

# The DDK headers ship inside the MinGW tree but in a subdirectory of their own,
# and bochsmp.c includes them unqualified. Derived from the compiler so that a
# different prefix keeps working; override if your toolchain puts them elsewhere.
MINGW_LIBDIR := $(dir $(shell $(CC) -print-file-name=libwin32k.a))
DDK_INCLUDE  ?= $(MINGW_LIBDIR)../include/ddk

# make DEBUGCON=1 turns on the driver's own diagnostics over QEMU's port 0xE9;
# start the guest with -debugcon file:<path> to catch them.
DEBUGCON ?= 0
ifeq ($(DEBUGCON),1)
DEBUGCON_FLAGS = -DQEMU_DEBUGCON
endif

MINIPORT_NAME ?= qemump
DISPLAY_NAME  ?= qemudisp

# Kernel mode code must not touch SSE/MMX: the FPU state is not saved for us.
# This deliberately differs from the wrappers, which target SSE3.
ARCH_FLAGS = -march=i686 -mno-sse -mno-mmx -mno-3dnow -mfpmath=387

COMMON_CFLAGS = -Wall -O2 $(ARCH_FLAGS) -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -fno-ident -mno-stack-arg-probe -D_X86_ -DWIN32 -Icompat -Icommon $(DEBUGCON_FLAGS) $(PIXELFORMAT_FLAGS) -isystem $(DDK_INCLUDE)

# Subsystem 1 is "native"; the version stamp keeps XP happy.
COMMON_LDFLAGS = -nostdlib -shared -Wl,--subsystem,native -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1 -Wl,--image-base,0x10000

MINIPORT_SRC = miniport/bochsmp.c
# make PIXELFORMATS=1 puts the pixel format DDI back into the driver. It is off because
# it measurably breaks the ICD path, see display/enable.c -- kept for the next attempt.
PIXELFORMATS ?= 0
ifeq ($(PIXELFORMATS),1)
PIXELFORMAT_FLAGS = -DQEMUDISP_OWN_PIXEL_FORMATS
PIXELFORMAT_SRC   = display/pixelformat.c
endif

DISPLAY_SRC  = display/enable.c display/icd.c $(PIXELFORMAT_SRC) common/kmem.c display/palette.c display/pointer.c display/screen.c display/surface.c

MINIPORT_OBJ = $(MINIPORT_SRC:%.c=$(BUILD)/%.o)
DISPLAY_OBJ  = $(DISPLAY_SRC:%.c=$(BUILD)/%.o)

MINIPORT_OUT = $(BUILD)/$(MINIPORT_NAME).sys
DISPLAY_OUT  = $(BUILD)/$(DISPLAY_NAME).dll

all: $(MINIPORT_OUT) $(DISPLAY_OUT)

$(MINIPORT_OBJ): CFLAGS_EXTRA = -Iminiport
$(DISPLAY_OBJ):  CFLAGS_EXTRA = -Idisplay

# The replacement memcpy/memset must not be turned into calls to themselves.
$(BUILD)/common/kmem.o: CFLAGS_EXTRA = -fno-tree-loop-distribute-patterns

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) $(CFLAGS_EXTRA) -c $< -o $@

$(MINIPORT_OUT): $(MINIPORT_OBJ)
	$(CC) $(COMMON_LDFLAGS) -Wl,--exclude-all-symbols -Wl,--entry,_DriverEntry@8 -o $@ $^ -lvideoprt -lntoskrnl

# No -lntoskrnl here on purpose: the display driver must import from win32k.sys alone,
# so memcpy and memset come from common/kmem.c instead.
$(DISPLAY_OUT): $(DISPLAY_OBJ) display/qemudisp.def
	$(CC) $(COMMON_LDFLAGS) -Wl,--exclude-all-symbols -Wl,--entry,_DrvEnableDriver@12 -o $@ $(DISPLAY_OBJ) display/qemudisp.def -lwin32k

clean:
	rm -rf $(BUILD)

.PHONY: all clean
