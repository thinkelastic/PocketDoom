# FPGA Makefile for Analogue Pocket Core
# Supports Quartus build and USB Blaster programming

PROJECT = ap_core
TOP_LEVEL = core_top
QUARTUS_DIR = /home/alberto/altera_lite/25.1std/quartus/bin
export PATH := $(QUARTUS_DIR):$(PATH)

# Output files
SOF_FILE = output_files/$(PROJECT).sof
RBF_FILE = output_files/$(PROJECT).rbf
RELEASE_RBF = ../../release/Cores/ThinkElastic.PocketDoom/bitstream.rbf_r

# Default target
all: build

# Full compilation
build:
	quartus_sh --flow compile $(PROJECT)

# Synthesis only (faster for checking errors)
map:
	quartus_map $(PROJECT)

# Place and route only
fit: map
	quartus_fit $(PROJECT)

# Timing analysis
sta: fit
	quartus_sta $(PROJECT)

# Generate programming files
asm: fit
	quartus_asm $(PROJECT)

# Update MIF files without full recompile (fast!)
mif:
	quartus_cdb $(PROJECT) --update_mif
	quartus_asm $(PROJECT)

# Program FPGA via USB Blaster (JTAG)
program: $(SOF_FILE)
	quartus_pgm -m jtag -o "p;$(SOF_FILE)"

# Alternative: program with explicit cable selection
program-usb: $(SOF_FILE)
	quartus_pgm -c "USB-Blaster" -m jtag -o "p;$(SOF_FILE)"

# List available programming cables
list-cables:
	quartus_pgm --list

# Convert SOF to RBF (for Analogue Pocket)
rbf: $(SOF_FILE)
	quartus_cpf -c $(SOF_FILE) $(RBF_FILE)

# Install RBF to release directory
install: $(RBF_FILE)
	cp $(RBF_FILE) $(RELEASE_RBF)
	@echo "Installed to $(RELEASE_RBF)"

# Build firmware, update MIF, and reprogram (fast iteration)
quick:
	$(MAKE) -C ../firmware clean all install
	$(MAKE) mif
	$(MAKE) program

# Full rebuild: firmware + FPGA + program
full:
	$(MAKE) -C ../firmware clean all install
	$(MAKE) build
	$(MAKE) program

# Clean Quartus build files
clean:
	rm -rf db incremental_db greybox_tmp
	rm -rf output_files
	rm -f $(PROJECT).qws $(PROJECT)_assignment_defaults.qdf $(PROJECT).ipregen.rpt
	rm -f c5_pin_model_dump.txt mf_pllbase.xml

# Show timing summary
timing:
	@grep -A 20 "Slow 1100mV 85C Model" output_files/$(PROJECT).sta.summary 2>/dev/null || echo "Run 'make sta' first"

# Show resource usage
resources:
	@grep -E "(Total logic elements|Total registers|Total memory bits|Total PLLs)" output_files/$(PROJECT).fit.summary 2>/dev/null || echo "Run 'make fit' first"

.PHONY: all build map fit sta asm mif program program-usb list-cables rbf install quick full clean timing resources
