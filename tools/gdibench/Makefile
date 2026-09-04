# gdibench -- 2D throughput of the display driver, measured inside the guest.
#
# Guest only: there is nothing to compare against on the host, the point is the
# difference between two display drivers under the same guest.
#
#   make        cross for Windows, MinGW -> build/GDIBENCH.EXE

SRC   := src/gdibench.c
BUILD := build

CROSS := i686-w64-mingw32-

# -mcrtdll=msvcrt-os keeps the toolchain off the UCRT, which no Windows before Vista has.
# -march=prescott is the same floor as the wrappers, so the binary runs in the 9x guest too.
CFLAGS := -std=gnu99 -Wall -Wextra -O2 -march=prescott -mtune=core2 -mcrtdll=msvcrt-os

.PHONY: all clean

all: $(BUILD)/GDIBENCH.EXE

$(BUILD)/GDIBENCH.EXE: $(SRC) | $(BUILD)
	$(CROSS)gcc $(CFLAGS) -o $@ $(SRC) -lgdi32 -luser32
	@echo "--- dependencies (no api-ms-win-crt may show up) ---"
	@$(CROSS)objdump -p $@ | grep "DLL Name"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
