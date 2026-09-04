# modes -- list and switch the display modes the driver offers, from inside the guest.
#
#   make        cross for Windows, MinGW -> build/MODES.EXE

SRC   := src/modes.c
BUILD := build

CROSS := i686-w64-mingw32-

CFLAGS := -std=gnu99 -Wall -Wextra -O2 -march=prescott -mtune=core2 -mcrtdll=msvcrt-os

.PHONY: all clean

all: $(BUILD)/MODES.EXE

$(BUILD)/MODES.EXE: $(SRC) | $(BUILD)
	$(CROSS)gcc $(CFLAGS) -o $@ $(SRC) -lgdi32 -luser32
	@echo "--- dependencies (no api-ms-win-crt may show up) ---"
	@$(CROSS)objdump -p $@ | grep "DLL Name"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
