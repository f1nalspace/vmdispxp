# ddbench -- DirectDraw 2D throughput, measured inside the guest against the
# system's own ddraw.dll. Not the wine9x route; see the file header.
#
#   make        cross for Windows, MinGW -> build/DDBENCH.EXE

SRC   := src/ddbench.c
BUILD := build

CROSS := i686-w64-mingw32-

CFLAGS := -std=gnu99 -Wall -Wextra -O2 -march=prescott -mtune=core2 -mcrtdll=msvcrt-os

.PHONY: all clean

all: $(BUILD)/DDBENCH.EXE

$(BUILD)/DDBENCH.EXE: $(SRC) | $(BUILD)
	$(CROSS)gcc $(CFLAGS) -o $@ $(SRC) -lddraw -ldxguid -lgdi32 -luser32
	@echo "--- dependencies (no api-ms-win-crt may show up) ---"
	@$(CROSS)objdump -p $@ | grep "DLL Name"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
