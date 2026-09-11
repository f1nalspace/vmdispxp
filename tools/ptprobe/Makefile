# ptprobe -- asks a vmdispxp built with PASSTHROUGH_PROBE=1 to drive the qemu-3dfx
# passthrough from kernel mode. Plan C spike, phases 0 and 1.
#
#   make        cross for Windows, MinGW -> build/PTPROBE.EXE
#
# The passthrough layout and the FEnum values come from the fork's own header, and the
# commit stamp from the wrapper's stamp.c -- QEMU refuses a session whose stamp differs
# from its own. Both locations can be overridden.

SRC   := src/ptprobe.c
BUILD := build

CROSS := i686-w64-mingw32-

QEMU3DFX              ?= ../../../qemu-3dfx
QEMU3DFX_MESA_INCLUDE ?= $(QEMU3DFX)/qemu-1/hw/mesa
QEMU3DFX_STAMP_SOURCE ?= $(QEMU3DFX)/wrappers/mesa/build/stamp.c
QEMU3DFX_COMMIT_STAMP ?= $(shell sed -n 's/.*rev_\[\] = "\([^"]*\)".*/\1/p' $(QEMU3DFX_STAMP_SOURCE))

CFLAGS := -std=gnu99 -Wall -Wextra -O2 -march=prescott -mtune=core2 -mcrtdll=msvcrt-os -I../../common -I$(QEMU3DFX_MESA_INCLUDE) -DQEMU3DFX_COMMIT_STAMP='"$(QEMU3DFX_COMMIT_STAMP)"'

.PHONY: all clean

all: $(BUILD)/PTPROBE.EXE

$(BUILD)/PTPROBE.EXE: $(SRC) ../../common/ptprobe.h | $(BUILD)
	@echo "--- commit stamp: $(QEMU3DFX_COMMIT_STAMP) ---"
	$(CROSS)gcc $(CFLAGS) -o $@ $(SRC) -lgdi32 -luser32
	@echo "--- dependencies (no api-ms-win-crt may show up) ---"
	@$(CROSS)objdump -p $@ | grep "DLL Name"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
