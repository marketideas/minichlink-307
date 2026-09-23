TOOLS:=minichlink-307 minichlink-307.so

CFLAGS:=-O0 -g3 -Wall -DCH32V003 -I.
C_S:=minichlink.c pgm-wch-linke.c pgm-esp32s2-ch32xx.c nhc-link042.c ardulink.c serial_dev.c pgm-b003fun.c minichgdb.c

# General Note: To use with GDB, gdb-multiarch
# gdb-multilib {file}
# target remote :2345

ifeq ($(OS),Windows_NT)
	LDFLAGS:=-L. -lpthread -lusb-1.0 -lsetupapi -lws2_32
	CFLAGS:=-Os -s -Wall -D_WIN32_WINNT=0x0600 -DCH32V003 -I.
	TOOLS:=minichlink-307.exe
else
	OS_NAME := $(shell uname -s | tr A-Z a-z)
	ifeq ($(OS_NAME),linux)
		LDFLAGS:=-lpthread -lusb-1.0 -ludev
	endif
	ifeq ($(OS_NAME),darwin)
		LDFLAGS:=-lpthread -lusb-1.0 -framework CoreFoundation -framework IOKit
		CFLAGS:=-O0 -Wall -Wno-asm-operand-widths -Wno-deprecated-declarations -Wno-deprecated-non-prototype -D__MACOSX__ -DCH32V003 -I.
		INCLUDES:=$(shell pkg-config --cflags-only-I libusb-1.0)
		LIBINCLUDES:=$(shell pkg-config --libs-only-L libusb-1.0)
		INCS:=$(INCLUDES) $(LIBINCLUDES)
	endif
endif

# koin firmware/CH32 runs this binary from tools/ (PROGRAM := minichlink-307).
KOIN_CH32 := ../koin/firmware/CH32
KOIN_CH32_TOOLS := $(KOIN_CH32)/tools
KOIN_BUILD_DIR := $(KOIN_CH32)/build

# Defaults matched to firmware/CH32/Makefile. Override on the command line
# (make flash CH32V_SRAM_SIZE=128 APP_NAME=assassin).
CH32V_SRAM_SIZE ?= 96
APP_NAME ?= assassin
BUILD_TYPE ?= Debug
PROGRAM := ./minichlink-307

all : $(TOOLS)
ifneq ($(OS),Windows_NT)
	cp -f minichlink-307 $(KOIN_CH32_TOOLS)/
endif

# will need mingw-w64-x86-64-dev gcc-mingw-w64-x86-64
minichlink-307.exe : $(C_S)
	x86_64-w64-mingw32-gcc -o $@ $^ $(LDFLAGS) $(CFLAGS)

minichlink-307 : $(C_S)
	gcc -o $@ $^ $(LDFLAGS) $(CFLAGS) $(INCS)

minichlink-307.so : $(C_S)
	gcc -o $@ $^ $(LDFLAGS) $(CFLAGS) $(INCS) -shared -fPIC

minichlink-307.dll : $(C_S)
	x86_64-w64-mingw32-gcc -o $@ $^ $(LDFLAGS) $(CFLAGS) $(INCS) -shared -DMINICHLINK_AS_LIBRARY

install_udev_rules :
	cp 99-WCH-LinkE.rules /etc/udev/rules.d/
	service udev restart

inspect_bootloader : minichlink-307
	./minichlink-307 -r test.bin launcher 0x780
	riscv64-unknown-elf-objdump -S -D test.bin -b binary -m riscv:rv32 | less

clean :
	rm -rf $(TOOLS)

# ── Same programmer targets as koin firmware/CH32 ─────────────────────────
# Invokes this tree's ./minichlink-307 against the koin build products.
# Does not configure or build the firmware.
.PHONY: flash release_flash flash305 protect unprotect reset status swd readflash kill killocd killall help

help:
	@echo ""
	@echo "minichlink-307 — make targets"
	@echo ""
	@echo "  all                  Build minichlink-307 and minichlink-307.so, then copy minichlink-307 to koin firmware/CH32/tools (default)"
	@echo "  minichlink-307       Build the programmer"
	@echo "  minichlink-307.so    Build the shared library"
	@echo "  minichlink-307.exe   Cross-build the Windows programmer"
	@echo "  minichlink-307.dll   Cross-build the Windows shared library"
	@echo "  clean                Remove built binaries"
	@echo "  install_udev_rules   Install 99-WCH-LinkE.rules and restart udev"
	@echo "  inspect_bootloader   Read the launcher and disassemble it"
	@echo ""
	@echo "Programmer targets use ./minichlink-307 and the koin CH32 tree."
	@echo "They do not configure or build the firmware."
	@echo "CH32V_SRAM_SIZE=$(CH32V_SRAM_SIZE)  APP_NAME=$(APP_NAME)  image=$(KOIN_BUILD_DIR)/$(APP_NAME).bin"
	@echo ""
	@echo "  flash                Attach, write the koin image, enable RDP if that build is Release, reboot"
	@echo "  release_flash        Same as flash, and force readout protection on"
	@echo "  flash305             Write tools/WCH-LinkE-APP-IAP.bin"
	@echo "  protect              Enable readout protection and let the part run"
	@echo "  unprotect            Disable readout protection (mass-erases application flash)"
	@echo "  reset                Pulse NRST only; do not attach"
	@echo "  status               Read chip status; do not reset if SWD is off"
	@echo "  swd                  Read over live SWD only; fail if the pins do not answer"
	@echo "  readflash            Read the first 256 bytes of application flash"
	@echo "  kill                 Kill riscv-none-embed-gdb"
	@echo "  killocd              Kill openocd"
	@echo "  killall              Kill gdb and openocd"
	@echo "  help                 Print this list"
	@echo ""

flash: minichlink-307 killall
	@$(PROGRAM) -A
	# -K programs FLASH_OBR.RAM_CODE_MOD to the SRAM/FLASH split the image
	# was linked for (CH32V_SRAM_SIZE in the koin Makefile).
	@$(PROGRAM) -K $(CH32V_SRAM_SIZE) -w $(KOIN_BUILD_DIR)/$(APP_NAME).bin flash
	@bt="$(BUILD_TYPE)"; \
	 cached=$$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' $(KOIN_BUILD_DIR)/CMakeCache.txt 2>/dev/null); \
	 if [ "$$bt" = Release ] || [ "$$cached" = Release ]; then \
	   echo "Release image: enabling flash readout protection"; \
	   $(PROGRAM) -P; \
	 else \
	   echo "------------------"; \
	   $(PROGRAM) -i; \
	   $(PROGRAM) -b; \
	 fi

# Sets BUILD_TYPE=Release so the flash recipe enables readout protection.
# Does not rebuild the koin firmware.
release_flash: BUILD_TYPE = Release
release_flash: flash
	@echo "RELEASE flash complete (readout protection enabled)"

# Enable readout protection and let the part run. One invocation: a later
# debug attach (the old trailing -b) asserts a hold that readout protection
# will not let us clear.
protect: minichlink-307 killall
	@echo "Enabling flash readout protection"
	@$(PROGRAM) -P

# Disable RDP. On CH32 this mass-erases application flash.
unprotect: minichlink-307 killall
	@echo "WARNING: disabling readout protection mass-erases application flash"
	@$(PROGRAM) -A
	@$(PROGRAM) -K $(CH32V_SRAM_SIZE) -p
	@$(PROGRAM) -b

flash305: minichlink-307 killall
	@$(PROGRAM) -A
	@$(PROGRAM) -w $(KOIN_CH32_TOOLS)/WCH-LinkE-APP-IAP.bin flash
	@$(PROGRAM) -i
	@$(PROGRAM) -b

# Pin reset only. Does not attach, so a protected running image is not
# left in the debug hold.
reset: minichlink-307 killocd
	@$(PROGRAM) -R

# Read-only. If SWD is already off (readout protection), do not pulse NRST
# to break in. Flash and unprotect still use that retry.
status: minichlink-307 killall
	@$(PROGRAM) -i

# Test that firmware turned SWD off. A running protected image should make
# this fail. Flash and unprotect still pulse NRST and attach under reset.
swd: minichlink-307 killall
	@$(PROGRAM) -S

# Read the start of application flash (vector table) over the debugger.
# This uses the normal attach, including the reset retry. Hex goes to the console.
readflash: minichlink-307 killall
	@$(PROGRAM) -r + flash 256

kill:
	-@killall -9 riscv-none-embed-gdb 2>/dev/null

killocd:
	-@killall -9 openocd 2>/dev/null

killall: kill killocd
