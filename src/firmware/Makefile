# PocketDoom Firmware Makefile
# Builds bootloader (BRAM) + Doom engine (SDRAM)

# Toolchain
CROSS ?= $(shell which riscv64-unknown-elf-gcc >/dev/null 2>&1 && echo riscv64-unknown-elf- || echo riscv64-elf-)
CC = $(CROSS)gcc
AS = $(CROSS)gcc
LD = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
OBJDUMP = $(CROSS)objdump
SIZE = $(CROSS)size

# Target
TARGET = firmware

# Directories
FPGA_CORE_DIR = ../fpga/core
LIBC_DIR = libc
DOOM_DIR = doom

# Source files - Bootloader (goes in BRAM)
BOOT_SRCS_C = main.c terminal.c dataslot.c misaligned.c

# Source files - libc (linked into SDRAM with Doom)
LIBC_SRCS = $(LIBC_DIR)/memory.c \
            $(LIBC_DIR)/string.c \
            $(LIBC_DIR)/ctype.c \
            $(LIBC_DIR)/stdlib.c \
            $(LIBC_DIR)/qsort.c \
            $(LIBC_DIR)/time.c \
            $(LIBC_DIR)/math.c \
            $(LIBC_DIR)/file.c

LIBC_ASM_SRCS = $(LIBC_DIR)/setjmp.S

# Source files - Doom engine (linked into SDRAM)
# Generic Doom sources (from doom/ directory, excluding d_main.c
# which is replaced by our platform-specific version in doom/riscv/)
DOOM_SRCS = $(DOOM_DIR)/am_map.c \
            $(DOOM_DIR)/d_items.c \
            $(DOOM_DIR)/d_net.c \
            $(DOOM_DIR)/doomdef.c \
            $(DOOM_DIR)/doomstat.c \
            $(DOOM_DIR)/dstrings.c \
            $(DOOM_DIR)/f_finale.c \
            $(DOOM_DIR)/f_wipe.c \
            $(DOOM_DIR)/g_game.c \
            $(DOOM_DIR)/hu_lib.c \
            $(DOOM_DIR)/hu_stuff.c \
            $(DOOM_DIR)/info.c \
            $(DOOM_DIR)/m_argv.c \
            $(DOOM_DIR)/m_bbox.c \
            $(DOOM_DIR)/m_cheat.c \
            $(DOOM_DIR)/m_fixed.c \
            $(DOOM_DIR)/m_menu.c \
            $(DOOM_DIR)/m_misc.c \
            $(DOOM_DIR)/m_random.c \
            $(DOOM_DIR)/m_swap.c \
            $(DOOM_DIR)/p_ceilng.c \
            $(DOOM_DIR)/p_doors.c \
            $(DOOM_DIR)/p_enemy.c \
            $(DOOM_DIR)/p_floor.c \
            $(DOOM_DIR)/p_inter.c \
            $(DOOM_DIR)/p_lights.c \
            $(DOOM_DIR)/p_map.c \
            $(DOOM_DIR)/p_maputl.c \
            $(DOOM_DIR)/p_mobj.c \
            $(DOOM_DIR)/p_plats.c \
            $(DOOM_DIR)/p_pspr.c \
            $(DOOM_DIR)/p_saveg.c \
            $(DOOM_DIR)/p_setup.c \
            $(DOOM_DIR)/p_sight.c \
            $(DOOM_DIR)/p_spec.c \
            $(DOOM_DIR)/p_switch.c \
            $(DOOM_DIR)/p_telept.c \
            $(DOOM_DIR)/p_tick.c \
            $(DOOM_DIR)/p_user.c \
            $(DOOM_DIR)/r_bsp.c \
            $(DOOM_DIR)/r_data.c \
            $(DOOM_DIR)/r_draw.c \
            $(DOOM_DIR)/r_main.c \
            $(DOOM_DIR)/r_plane.c \
            $(DOOM_DIR)/r_segs.c \
            $(DOOM_DIR)/r_sky.c \
            $(DOOM_DIR)/r_things.c \
            $(DOOM_DIR)/sounds.c \
            $(DOOM_DIR)/st_lib.c \
            $(DOOM_DIR)/st_stuff.c \
            $(DOOM_DIR)/tables.c \
            $(DOOM_DIR)/v_video.c \
            $(DOOM_DIR)/wi_stuff.c \
            $(DOOM_DIR)/s_sound.c \
            $(DOOM_DIR)/w_wad.c \
            $(DOOM_DIR)/z_zone.c

# Simplified d_main.c from riscv port (no command-line parsing)
DOOM_SRCS += $(DOOM_DIR)/riscv/d_main.c

# Pocket platform-specific sources (i_video, i_system, sound, net, music)
DOOM_SRCS += doom_pocket.c doom_sound.c doom_music.c doom_net.c

# Assembly sources
SRCS_S = start.S

# All sources
ALL_C_SRCS = $(BOOT_SRCS_C) $(LIBC_SRCS) $(DOOM_SRCS)
OBJS = $(ALL_C_SRCS:.c=.o) $(SRCS_S:.S=.o) $(LIBC_ASM_SRCS:.S=.o)

# Compiler flags for RV32IMAFC (hardware single-precision FPU + compressed ISA)
ARCH = rv32imafc
ABI = ilp32f
CFLAGS = -march=$(ARCH) -mabi=$(ABI) -Os -Wall -Wextra
CFLAGS += -ffreestanding -nostdlib -nostartfiles
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -fno-builtin
CFLAGS += -fsingle-precision-constant
CFLAGS += -I. -I$(LIBC_DIR)

# Doom-specific flags
# Build Doom engine for speed while keeping boot/libc at size-optimized defaults.
DOOM_CFLAGS = $(filter-out -Os,$(CFLAGS)) -O2 -flto -std=gnu11 -fcommon -isystem sysinclude -I$(DOOM_DIR) -Wno-unused-parameter -Wno-unused-variable
DOOM_CFLAGS += -Wno-missing-field-initializers -Wno-sign-compare
DOOM_CFLAGS += -Wno-implicit-function-declaration -Wno-pointer-sign
DOOM_CFLAGS += -Wno-discarded-qualifiers -Wno-parentheses
DOOM_CFLAGS += -DNORMALUNIX -DPOCKET_DOOM

# Assembler flags
ASFLAGS = -march=$(ARCH)_zicsr -mabi=$(ABI)

# Linker flags
LDFLAGS = -march=$(ARCH) -mabi=$(ABI)
LDFLAGS += -T linker.ld -nostdlib -nostartfiles
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-Map=$(TARGET).map
LDFLAGS += -flto

# Standard libgcc for rv32imafc/ilp32f (includes compressed instructions)
LIBGCC = $(shell $(CC) -march=$(ARCH) -mabi=$(ABI) -print-libgcc-file-name)
LIBS = $(LIBGCC)

# RAM size for MIF generation (64KB = 16384 words)
# Only the bootloader goes into MIF (BRAM)
RAM_WORDS = 16384

# Default target
all: $(TARGET).bin $(TARGET).mif $(TARGET).lst doom.bin
	$(SIZE) $(TARGET).elf

# Link everything together
$(TARGET).elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

# Full binary output (bootloader + SDRAM sections)
$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

# Extract BRAM image (bootloader + BRAM hot sections) for MIF
$(TARGET).mif: $(TARGET).elf
	$(OBJCOPY) -O binary \
		--only-section=.boot \
		--only-section=.boot_data \
		--only-section=.boot_bss \
		--only-section=.fasttext \
		--only-section=.fastdata \
		$< boot.bin
	@echo "-- Firmware RAM initialization - $(RAM_WORDS) x 32-bit words" > $@
	@echo "-- Auto-generated from boot sections" >> $@
	@echo "" >> $@
	@echo "WIDTH=32;" >> $@
	@echo "DEPTH=$(RAM_WORDS);" >> $@
	@echo "" >> $@
	@echo "ADDRESS_RADIX=DEC;" >> $@
	@echo "DATA_RADIX=HEX;" >> $@
	@echo "" >> $@
	@echo "CONTENT BEGIN" >> $@
	@hexdump -v -e '1/4 "%08X\n"' boot.bin | awk '{printf "%d : %s;\n", NR-1, $$1}' >> $@
	@LAST=$$(hexdump -v -e '1/4 "%08X\n"' boot.bin | wc -l); \
	if [ $$LAST -lt $(RAM_WORDS) ]; then \
		echo "[$$LAST..$$(($(RAM_WORDS)-1))] : 00000013;" >> $@; \
	fi
	@echo "END;" >> $@
	@echo "Generated $@ with $$(hexdump -v -e '1/4 "%08X\n"' boot.bin | wc -l) words of BRAM image"

# Extract Doom binary (loaded by deferload to SDRAM, executes in place)
doom.bin: $(TARGET).elf
	$(OBJCOPY) -O binary --only-section=.text --only-section=.data $< $@
	@echo "Generated $@ ($$(wc -c < $@) bytes)"

# Disassembly listing
$(TARGET).lst: $(TARGET).elf
	$(OBJDUMP) -d -S $< > $@

# Compile boot C sources
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Compile libc sources
$(LIBC_DIR)/%.o: $(LIBC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Assemble libc assembly
$(LIBC_DIR)/%.o: $(LIBC_DIR)/%.S
	$(AS) $(ASFLAGS) -c -o $@ $<

# Compile Doom sources with relaxed warnings
$(DOOM_DIR)/%.o: $(DOOM_DIR)/%.c
	$(CC) $(DOOM_CFLAGS) -c -o $@ $<

# Compile riscv platform sources (d_main.c)
$(DOOM_DIR)/riscv/%.o: $(DOOM_DIR)/riscv/%.c
	$(CC) $(DOOM_CFLAGS) -c -o $@ $<

# Compile Pocket platform sources with Doom includes
doom_pocket.o: doom_pocket.c
	$(CC) $(DOOM_CFLAGS) -c -o $@ $<

doom_sound.o: doom_sound.c
	$(CC) $(DOOM_CFLAGS) -c -o $@ $<

doom_music.o: doom_music.c
	$(CC) $(DOOM_CFLAGS) -c -o $@ $<

doom_net.o: doom_net.c
	$(CC) $(DOOM_CFLAGS) -c -o $@ $<


# Assemble assembly sources
%.o: %.S
	$(AS) $(ASFLAGS) -c -o $@ $<

# Install MIF to FPGA core directory
install: $(TARGET).mif
	cp $(TARGET).mif $(FPGA_CORE_DIR)/firmware.mif
	@echo "MIF installed to $(FPGA_CORE_DIR)/firmware.mif"

# Copy doom.bin to release assets
release: doom.bin
	mkdir -p ../../release/Assets/pocketdoom/common
	cp doom.bin ../../release/Assets/pocketdoom/common/
	@echo "doom.bin copied to release/Assets/pocketdoom/common/"

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).bin $(TARGET).mif $(TARGET).map $(TARGET).lst
	rm -f boot.bin doom.bin

# Rebuild everything
rebuild: clean all

# Show memory usage
mem: $(TARGET).elf
	$(SIZE) -A -x $(TARGET).elf

.PHONY: all clean rebuild install release mem
