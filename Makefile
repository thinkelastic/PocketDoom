# Analogue Pocket Core Makefile
# Replaces build.sh and build-quick.sh

# Configuration
CORE_NAME = ThinkElastic.PocketDoom
QUARTUS_PROJECT = ap_core

# Directories
FPGA_DIR = src/fpga
FIRMWARE_DIR = src/firmware
OUTPUT_DIR = release
RELEASE_CORE_DIR = $(OUTPUT_DIR)/Cores/$(CORE_NAME)
RELEASE_PLATFORMS_DIR = $(OUTPUT_DIR)/Platforms
RELEASE_ASSETS_DIR = $(OUTPUT_DIR)/Assets/pocketdoom/common
RELEASE_INSTANCES_DIR = $(OUTPUT_DIR)/Assets/pocketdoom/$(CORE_NAME)

# Files
BITSTREAM_SOURCE = $(FPGA_DIR)/output_files/$(QUARTUS_PROJECT).rbf
BITSTREAM_TARGET = $(RELEASE_CORE_DIR)/bitstream.rbf_r
FIRMWARE_SOURCE = $(FIRMWARE_DIR)/doom.bin
FIRMWARE_TARGET = $(RELEASE_ASSETS_DIR)/doom.bin

# JSON configuration files
JSON_FILES = core.json video.json audio.json input.json data.json variants.json interact.json

# Tools
REVERSE_BITS = ./reverse_bits

# Default target - package without recompiling FPGA
all: package

# Full build - compile FPGA, firmware, and package
full: fpga firmware package

# Compile FPGA with Quartus (builds firmware first to generate MIF)
# Clears Quartus cache to ensure MIF changes are picked up
fpga: firmware-mif clean-fpga-cache
	@echo "Compiling FPGA design..."
	@if ! command -v quartus_sh >/dev/null 2>&1; then \
		echo "Error: quartus_sh not found in PATH"; \
		exit 1; \
	fi
	cd $(FPGA_DIR) && quartus_sh --flow compile $(QUARTUS_PROJECT)
	@echo "FPGA compilation complete"

# Build firmware and install MIF to FPGA core directory
firmware-mif:
	@echo "Building firmware and generating MIF..."
	$(MAKE) -C $(FIRMWARE_DIR) install
	@echo "Firmware MIF ready for FPGA build"

# Build firmware
firmware:
	@echo "Building firmware..."
	$(MAKE) -C $(FIRMWARE_DIR)
	@echo "Firmware build complete"

# Package release (uses existing bitstream)
package: $(REVERSE_BITS) check-bitstream release-dirs copy-bitstream copy-firmware copy-config copy-json copy-platform copy-instances copy-icon install-txt
	@echo ""
	@echo "Build complete!"
	@echo "Release package: $(OUTPUT_DIR)/"
	@echo ""
	@tree -L 4 $(OUTPUT_DIR) 2>/dev/null || find $(OUTPUT_DIR) -type f | sort

# Check that bitstream exists
check-bitstream:
	@if [ ! -f "$(BITSTREAM_SOURCE)" ]; then \
		echo "Error: Bitstream not found at $(BITSTREAM_SOURCE)"; \
		echo "Run 'make fpga' first or compile with Quartus"; \
		exit 1; \
	fi

# Create release directory structure
release-dirs:
	@echo "Creating release directories..."
	@rm -rf $(OUTPUT_DIR)
	@mkdir -p $(RELEASE_CORE_DIR)
	@mkdir -p $(RELEASE_PLATFORMS_DIR)/_images
	@mkdir -p $(RELEASE_ASSETS_DIR)
	@mkdir -p $(RELEASE_INSTANCES_DIR)

# Build bit reversal tool
$(REVERSE_BITS): reverse_bits.c
	@echo "Compiling bit reversal tool..."
	gcc -O2 -o $@ $<

# Convert and copy bitstream
copy-bitstream: $(REVERSE_BITS)
	@echo "Converting bitstream to RBF_R format..."
	$(REVERSE_BITS) $(BITSTREAM_SOURCE) $(BITSTREAM_TARGET)

# Copy firmware (doom.bin)
copy-firmware:
	@echo "Copying doom.bin..."
	cp $(FIRMWARE_SOURCE) $(FIRMWARE_TARGET)

# Copy default config
copy-config:
	@echo "Copying default.cfg..."
	cp dist/common/default.cfg $(RELEASE_ASSETS_DIR)/default.cfg

# Copy JSON configuration files
copy-json:
	@echo "Copying configuration files..."
	@for f in $(JSON_FILES); do \
		cp $$f $(RELEASE_CORE_DIR)/; \
	done

# Copy platform definition and images
copy-platform:
	@echo "Copying platform files..."
	@cp dist/platforms/*.json $(RELEASE_PLATFORMS_DIR)/
	@cp dist/platforms/_images/*.bin $(RELEASE_PLATFORMS_DIR)/_images/

# Copy instance JSON files (game selector)
copy-instances:
	@echo "Copying instance files..."
	@cp dist/instances/*.json $(RELEASE_INSTANCES_DIR)/

# Copy core icon if it exists
copy-icon:
	@if [ -f "dist/icon.bin" ]; then \
		echo "Copying core icon..."; \
		cp dist/icon.bin $(RELEASE_CORE_DIR)/; \
	fi

# Generate installation instructions
define INSTALL_TEXT
Analogue Pocket Core Installation Instructions
================================================

Core: $(CORE_NAME)

Installation Steps:
-------------------

1. Insert your Analogue Pocket's SD card into your computer

2. Copy the entire contents of this release folder to your SD card root
   - Merge with existing folders if they exist

3. Safely eject the SD card and insert it back into your Analogue Pocket

4. Power on your Analogue Pocket

5. Navigate to the "Cores" menu and select "$(CORE_NAME)"

Directory Structure:
--------------------
Your SD card should have this structure:

SD Card Root/
+-- Assets/
|   +-- pocketdoom/
|       +-- common/
|           +-- doom.bin
+-- Cores/
|   +-- $(CORE_NAME)/
|       +-- bitstream.rbf_r
|       +-- core.json
|       +-- video.json
|       +-- audio.json
|       +-- input.json
|       +-- data.json
|       +-- variants.json
|       +-- interact.json
|       +-- icon.bin
+-- Platforms/
    +-- _images/
    |   +-- pocketdoom.bin
    +-- pocketdoom.json

Troubleshooting:
----------------
- Make sure the SD card is formatted as exFAT
- Ensure your Analogue Pocket firmware is version 1.1 or later
- Check that all files copied correctly without errors

For more information, visit:
https://www.analogue.co/developer
endef
export INSTALL_TEXT

install-txt:
	@echo "Generating installation instructions..."
	@echo "$$INSTALL_TEXT" > $(OUTPUT_DIR)/INSTALL.txt

# Clean all build artifacts
clean:
	@echo "Cleaning..."
	rm -rf $(OUTPUT_DIR)
	rm -f $(REVERSE_BITS)
	$(MAKE) -C $(FIRMWARE_DIR) clean

# Fast firmware update - only updates MIF in existing bitstream (no full recompile)
# Use this when you only changed firmware and want a quick rebuild (~1 min vs ~15 min)
firmware-update: firmware-mif
	@echo "Updating MIF in existing bitstream..."
	@if [ ! -f "$(FPGA_DIR)/output_files/$(QUARTUS_PROJECT).sof" ]; then \
		echo "Error: No existing compile found. Run 'make fpga' first."; \
		exit 1; \
	fi
	cd $(FPGA_DIR) && quartus_cdb --update_mif $(QUARTUS_PROJECT)
	cd $(FPGA_DIR) && quartus_asm $(QUARTUS_PROJECT)
	@echo "Firmware updated in bitstream"

# Alias for firmware-update
fw: firmware-update package

# Clean Quartus cache (forces MIF files to be re-read)
clean-fpga-cache:
	@echo "Clearing Quartus cache to pick up MIF changes..."
	rm -rf $(FPGA_DIR)/db $(FPGA_DIR)/incremental_db

# Clean FPGA build artifacts
clean-fpga: clean-fpga-cache
	@echo "Cleaning FPGA build artifacts..."
	rm -f $(FPGA_DIR)/output_files/*

# Quick target (alias for package)
quick: package

# Program FPGA via JTAG (for development)
program: firmware-mif
	@echo "Programming FPGA via JTAG..."
	$(MAKE) -C $(FPGA_DIR) program

.PHONY: all full fpga firmware-mif firmware firmware-update fw package check-bitstream release-dirs copy-bitstream copy-firmware copy-config copy-json copy-platform copy-icon install-txt clean clean-fpga-cache clean-fpga quick program
