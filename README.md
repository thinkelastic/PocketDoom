# PocketDoom

Doom (1993) running natively on the [Analogue Pocket](https://www.analogue.co/pocket) via a VexiiRiscv RISC-V soft CPU on the Cyclone V FPGA. No emulation — the id Software Doom engine runs as bare-metal firmware on a hardware CPU synthesized in the FPGA fabric.

## Installation

1. Copy the contents of the `release/` directory to your Analogue Pocket SD card root
2. Create a game directory under `Assets/pocketdoom/common/` (e.g. `doom/`, `doom2/`)
3. Copy your IWAD file into that directory
4. Create an instance JSON under `Assets/pocketdoom/ThinkElastic.PocketDoom/` — see [Adding Games & Mods](#adding-games--mods) for step-by-step instructions
5. For link cable multiplayer, you **must** use a **GB/GBC link cable** — GBA cables will NOT work

The game mode is auto-detected from WAD contents. The Pocket menu will list each instance JSON as a selectable entry.

### Controls

| Button | Action |
|--------|--------|
| D-pad | Move / navigate menus |
| A (right face) | Fire |
| B (bottom face) | Use / open doors |
| X (top face) | Weapon up (next) |
| Y (left face) | Weapon down (previous) |
| L1 | Strafe left |
| R1 | Strafe right |
| L2 / Left trigger | Weapon down (previous) |
| R2 / Right trigger | Weapon up (next) |
| Start | Menu (ESC) |
| Select | Automap |

Always-run is enabled by default.

In menus, R1/R2 act as Enter and L1/L2 act as Back.

### Controller Configuration

Shoulder and trigger buttons are configurable via `default.cfg`, placed in `Assets/pocketdoom/common/`. The following keys can be changed:

| Config Key | Default | Description |
|------------|---------|-------------|
| `key_l1` | 44 (`,`) | L1 shoulder — strafe left |
| `key_r1` | 46 (`.`) | R1 shoulder — strafe right |
| `key_l2` | 241 | L2 / left trigger — weapon down |
| `key_r2` | 240 | R2 / right trigger — weapon up |

Special key codes: `240` = weapon up (next), `241` = weapon down (previous), `0` = disabled. Standard Doom key codes (e.g. `157` for fire, `32` for use) can also be assigned.

## Features

- **Full Doom engine** — Software-rendered Doom at 320x200, 8-bit indexed color with hardware palette lookup
- **VexiiRiscv RISC-V CPU** — rv32imafc (integer, multiply/divide, atomics, single-precision FPU, compressed) at 100 MHz with AXI4 bus, 64KB I-cache and 64KB D-cache
- **Execute from SDRAM** — Doom code runs directly from SDRAM with AXI4 burst I-cache fills; hot rendering functions pinned in BRAM for zero-latency access
- **Hardware OPL3 synthesizer** — opl3_fpga (YMF262) core with 18 voices, replacing software OPL emulation for music playback
- **Non-blocking audio** — Variable-length SFX mixing driven by FIFO level feedback, decoupled from frame rate
- **48 kHz stereo audio** — Hardware mixing of OPL3 music + SFX, I2S output
- **Vsync-locked double buffering** — Direct framebuffer rendering with D-cache flush, swap synchronized to 60 Hz vsync
- **CPU-controlled asset loading** — WAD and doom.bin loaded via deferload (dataslot API) with WAD header parsing to determine file size
- **2-player link cable multiplayer** — GB/GBC link cable at 256 kHz, full-duplex serial protocol
- **Dock support** — HDMI output when docked

## Architecture

```
+-----------------------------------------------------------------------+
|                       Analogue Pocket FPGA                            |
|                     (Cyclone V 5CEBA4F23C8)                           |
+-----------------------------------------------------------------------+
|                                                                       |
|  +-----------------------+    +-----------------------------------+   |
|  |   VexiiRiscv CPU      |    |        Memory Subsystem           |   |
|  |  rv32imafc @ 100 MHz  |    |                                   |   |
|  |                       |    |  +--------+  +--------+  +------+ |   |
|  |  AXI4 bus             |    |  | BRAM   |  | SDRAM  |  | PSRAM| |   |
|  |  3-bus architecture   |    |  | 64KB   |  | 64MB   |  | 16MB | |   |
|  +-----------+-----------+    +--+--------+--+--------+--+------+-+   |
|              |                    |                                    |
|  +-----------+--------------------+----------------------------+      |
|  |                     AXI4 Bus Fabric                         |      |
|  |  FetchL1 (I-cache) / LsuL1 (D-cache) / LsuIO (uncached)   |      |
|  +----+--------+---------+---------+---------+--------+-------+      |
|       |        |         |         |         |        |              |
|  +----+--+ +---+----+ +-+------+ ++------+ ++-----+ ++------+       |
|  | Video | | Audio  | | OPL3   | | Link  | | Sys  | | Text  |       |
|  |Scanout| | Output | | Synth  | | MMIO  | | Regs | | Term  |       |
|  +-------+ +---+----+ +---+----+ +-------+ +------+ +-------+       |
|                |           |                                          |
|                +-----+-----+                                          |
|                      |                                                |
|                 +----+----+                                           |
|                 |   I2S   |                                           |
|                 | 48 kHz  |                                           |
|                 +---------+                                           |
+-----------------------------------------------------------------------+
```

### CPU Architecture

The VexiiRiscv CPU uses a 3-bus AXI4 architecture:

- **FetchL1Axi4** — I-cache reads (16-beat bursts from SDRAM/BRAM)
- **LsuL1Axi4** — D-cache reads/writes (64KB, 2-way set-associative, write-back)
- **LsuPlugin IO** — Uncached single-beat access for MMIO registers

Per-bus address decoding routes transactions to SDRAM, PSRAM, or local peripherals. An AXI4 arbiter multiplexes CPU and video scanout SDRAM access.

## Memory Map

| Address Range             | Size   | Description                                          |
|---------------------------|--------|------------------------------------------------------|
| `0x00000000 - 0x0000FFFF` | 64 KB  | BRAM — bootloader + hot rendering functions          |
| `0x10000000 - 0x100FFFFF` | 1 MB   | Framebuffer 0 (320x240, 8-bit indexed)               |
| `0x10100000 - 0x101FFFFF` | 1 MB   | Framebuffer 1 (double buffer)                        |
| `0x10200000 - 0x1024FFFF` | ~289 KB| Doom code + rodata + data (execute in place)         |
| `0x10250000 - 0x10B7FFFF` | ~9 MB  | BSS + heap                                           |
| `0x10B80000 - 0x10BFFFFF` | 512 KB | Runtime stack                                        |
| `0x10C00000 - 0x13FFFFFF` | ~52 MB | WAD data (loaded via deferload)                      |
| `0x20000000`              | 1.2 KB | Terminal VRAM (40x30 characters)                     |
| `0x40000000`              | 256 B  | System registers                                     |
| `0x4C000000`              | 8 B    | Audio FIFO (write samples / read status)             |
| `0x4D000000`              | 256 B  | Link cable MMIO registers                            |
| `0x4E000000`              | 16 B   | OPL3 hardware registers (bank 0 + bank 1 addr/data)  |
| `0x50000000 - 0x53FFFFFF` | 64 MB  | SDRAM uncached alias (bypasses D-cache)              |

## System Registers (0x40000000)

| Offset | Register       | Description                                    |
|--------|----------------|------------------------------------------------|
| 0x00   | SYS_STATUS     | [0] sdram_ready, [1] allcomplete               |
| 0x04   | CYCLE_LO       | Cycle counter (low 32 bits)                    |
| 0x08   | CYCLE_HI       | Cycle counter (high 32 bits)                   |
| 0x0C   | DISPLAY_MODE   | 0 = terminal overlay, 1 = framebuffer          |
| 0x10   | FB_DISPLAY     | Display framebuffer address (25-bit word addr) |
| 0x14   | FB_DRAW        | Draw framebuffer address (25-bit word addr)    |
| 0x18   | FB_SWAP        | Write 1 to request swap; read returns pending  |
| 0x40   | PAL_INDEX      | Palette write index (auto-increment)           |
| 0x44   | PAL_DATA       | Palette entry (RGB888, triggers write)         |

## Hardware OPL3 Synthesizer

Doom's music is MUS format, played through OPL3 (YMF262) FM synthesis. The software Nuked OPL3 emulator was too slow on the 100 MHz VexiiRiscv, so synthesis is offloaded to an FPGA OPL3 core ([gtaylormb/opl3_fpga](https://github.com/gtaylormb/opl3_fpga)).

```
Firmware (RISC-V)              FPGA
+------------------+  MMIO    +------------------+
| MUS parser       |---0x4E-->| opl3_wrapper     |
| opl_write()      | bus stall|   opl3_fpga core |
+------------------+          |   12.288 MHz     |
                              +--------+---------+
                                       | 16-bit signed
+------------------+          +--------v---------+
| SFX mixer        |---0x4C-->| audio_output     |--> I2S 48 kHz
| I_SubmitSound()  | FIFO push|   HW mix + I2S   |
+------------------+          +------------------+
```

- **Core:** opl3_fpga — bit-true YMF262 reverse-engineered design (LGPL-3.0, proven in ao486 MiSTer)
- **Mode:** Full OPL3 with 18 voices (2 banks × 9 channels), OPL2-compatible register writes
- **Clock:** 12.288 MHz / 256 = exactly 48 kHz sample rate (matches I2S, no resampling needed)
- **MMIO:** `0x4E000000/04` = bank 0 addr/data, `0x4E000008/0C` = bank 1 addr/data
- **Bus stalling:** CPU halts automatically during OPL3 write timing (~4 µs addr, ~24 µs data)
- **Volume:** Chocolate Doom-compatible volume formula (velocity × channel volume, additive modulator capped at GENMIDI level)
- **Mixing:** Saturating add of OPL3 mono + SFX stereo with boost, before I2S serialization
- **CDC:** Toggle-handshake for glitch-free multi-bit clock domain crossing

## Audio Pipeline

- **Sample rate:** 48 kHz stereo, 16-bit signed
- **I2S output:** MCLK 12.288 MHz, SCLK 3.072 MHz, LRCK 48 kHz
- **SFX FIFO:** 4096-entry dual-clock FIFO (CPU clock to audio clock)
- **SFX mixing:** Doom mixes at 11,025 Hz with variable batch size driven by FIFO level feedback; firmware upsamples to 48 kHz with linear interpolation
- **Non-blocking submission:** `I_SubmitSound()` pushes as many samples as the FIFO can accept, called from both `I_StartFrame()` and the main loop to avoid audio gaps
- **Music:** Hardware OPL3 generates audio continuously; mixed with SFX in FPGA before I2S output
- **Interface:** CPU writes SFX samples to `0x4C000000`, OPL3 register writes to `0x4E000000-0x4E00000C`

## Video Pipeline

- **Resolution:** 320x240 @ 60 Hz (12.288 MHz pixel clock)
- **Color depth:** 8-bit indexed with 256-entry RGB888 hardware palette
- **Scanout:** AXI4 burst reads from SDRAM
- **Double buffered:** Doom renders directly into the SDRAM draw buffer (no memcpy); `FB_SWAP` requests vsync-synchronized swap. Software waits for pending swaps to prevent desynchronization.
- **D-cache coherency:** After rendering, firmware reads 128 KB of unrelated SDRAM to force the 64 KB 2-way D-cache to evict all dirty framebuffer lines to physical SDRAM for video scanout
- **Clock domain crossing:** Dual-clock FIFO between pixel clock (12.288 MHz) and SDRAM clock (100 MHz)

## BRAM Hot Path

Performance-critical rendering inner loops are pinned in the 64 KB BRAM for single-cycle instruction fetch, avoiding I-cache misses during the most executed code paths:

| Function | File | Description |
|----------|------|-------------|
| `R_DrawColumn` | r_draw.c | Wall column rasterizer (innermost loop) |
| `R_DrawSpan` | r_draw.c | Floor/ceiling span rasterizer |
| `FixedMul` | m_fixed.c | 16.16 fixed-point multiply |
| `FixedDiv2` | m_fixed.c | 16.16 fixed-point divide |
| `I_FinishUpdate` | doom_pocket.c | Cache flush + buffer swap |
| `I_SubmitSound` | doom_sound.c | Audio FIFO drain |

Functions are annotated with `PD_FASTTEXT` (defined in `doomdef.h`), placed in the `.fasttext` linker section. Current usage: ~700 bytes of 64 KB BRAM.

## Link Cable Multiplayer

2-player multiplayer over the Analogue Pocket's GB/GBC link cable.

- **Physical:** GB/GBC cable (crosses SO/SI), GBA cables do NOT work
- **Protocol:** Full-duplex serial, 33-bit transfers `{valid, data[31:0]}`, MSB-first
- **Speed:** 256 kHz SCK (GB/GBC mode)
- **Hardware:** TX/RX FIFOs (256 entries each), 3-stage SCK synchronizer for slave mode

## Boot Flow

1. FPGA configures, BRAM bootloader runs from address `0x00000000`
2. Bootloader waits for APF bridge `allcomplete` signal
3. Loads `doom.bin` from data slot 1 into SDRAM (`0x10200000`) via chunked deferload
4. Loads WAD header from data slot 0, parses it to determine full WAD size
5. Loads remaining WAD body into SDRAM (`0x10C00000`)
6. `fence` + `fence.i` (flush D-cache, invalidate I-cache)
7. Clears BSS, switches to runtime stack, jumps to `doom_main()` in SDRAM

## Building

### Prerequisites

- **RISC-V toolchain:** `riscv64-elf-gcc` with rv32imafc support
- **Intel Quartus Prime:** 25.1 or later (Lite edition sufficient)
- **Analogue Pocket:** Firmware 2.2 or later

```bash
# Arch Linux
sudo pacman -S riscv64-elf-gcc riscv64-elf-newlib

# The firmware uses -march=rv32imafc -mabi=ilp32f
```

### Build Firmware

```bash
cd src/firmware
make                  # Builds doom.bin + firmware.mif
make install          # Copies MIF to FPGA directory
```

### Build FPGA

```bash
cd src/fpga
make                  # Full Quartus synthesis
make mif              # Update MIF only, no resynthesis (~1 min)
make program          # Program via JTAG (USB Blaster)
```

### Package Release

```bash
make                  # From project root — packages release/ directory
```

### Quick Development Cycle

```bash
cd src/fpga
make quick            # Build firmware + update MIF + program via JTAG
```

### Deploy to SD Card

```bash
./deploy.sh           # Auto-detects SD card, mounts, syncs release/, unmounts
```

## Installation Layout

```
SD Card Root/
+-- Assets/
|   +-- pocketdoom/
|       +-- common/
|       |   +-- doom.bin                 # Doom firmware (always required)
|       |   +-- default.cfg              # Controller/settings configuration
|       |   +-- doom/
|       |   |   +-- DOOM.WAD            # IWAD
|       |   +-- doom2/
|       |   |   +-- DOOM2.WAD           # IWAD
|       |   |   +-- mymod.wad           # Optional PWAD (mod)
|       |   +-- plutonia/
|       |       +-- PLUTONIA.wad        # IWAD
|       +-- ThinkElastic.PocketDoom/
|           +-- Doom.json               # Instance: selectable from Pocket menu
|           +-- Doom2.json
|           +-- Doom2Modded.json        # Instance with PWAD
|           +-- Plutonia.json
+-- Cores/
|   +-- ThinkElastic.PocketDoom/
|       +-- bitstream.rbf_r
|       +-- core.json
|       +-- data.json
|       +-- (other .json files)
+-- Platforms/
    +-- _images/
    |   +-- pocketdoom.bin
    +-- pocketdoom.json
```

## Adding Games & Mods

Each game or mod combination is an **instance JSON** file placed under `Assets/pocketdoom/ThinkElastic.PocketDoom/`. The filename (without `.json`) is what appears in the Pocket menu. WAD files are stored under `Assets/pocketdoom/common/` in subdirectories.

### Adding an IWAD (base game)

1. Create a directory: `Assets/pocketdoom/common/doom2/`
2. Copy your IWAD into it: `Assets/pocketdoom/common/doom2/DOOM2.WAD`
3. Create `Assets/pocketdoom/ThinkElastic.PocketDoom/Doom2.json`:

```json
{
    "instance": {
        "magic": "APF_VER_1",
        "variant_select": {
            "id": 666,
            "select": false
        },
        "data_slots": [
            {
                "id": 0,
                "filename": "doom2/DOOM2.WAD"
            },
            {
                "id": 1,
                "filename": "doom.bin"
            },
            {
                "id": 3,
                "filename": "default.cfg"
            }
        ]
    }
}
```

The `filename` path is relative to `Assets/pocketdoom/common/`. Data slot `0` is the IWAD, slot `1` is the Doom firmware binary, slot `3` is the configuration file.

### Adding a PWAD (mod)

To play a mod, copy the PWAD into the same directory as the IWAD it requires, then create an instance JSON that loads both:

1. Copy the PWAD: `Assets/pocketdoom/common/doom2/btsx_e1.wad`
2. Create `Assets/pocketdoom/ThinkElastic.PocketDoom/BTSXEpisode1.json`:

```json
{
    "instance": {
        "magic": "APF_VER_1",
        "variant_select": {
            "id": 666,
            "select": false
        },
        "data_slots": [
            {
                "id": 0,
                "filename": "doom2/DOOM2.WAD"
            },
            {
                "id": 1,
                "filename": "doom.bin"
            },
            {
                "id": 2,
                "filename": "doom2/btsx_e1.wad"
            },
            {
                "id": 3,
                "filename": "default.cfg"
            }
        ]
    }
}
```

Data slot `2` is the PWAD (optional). Slot `3` is the configuration. You can create multiple instance JSONs that share the same IWAD but load different PWADs.

### Data Slot Reference

| Slot | Purpose | Required |
|------|---------|----------|
| `0`  | IWAD (base game WAD) | Yes |
| `1`  | Doom firmware (`doom.bin`) | Yes |
| `2`  | PWAD (mod WAD) | No |
| `3`  | Configuration (`default.cfg`) | No |

## Project Structure

```
.
+-- src/
|   +-- firmware/                  # Doom firmware (C, bare-metal)
|   |   +-- main.c                 # Bootloader (deferload, BSS clear, jump)
|   |   +-- doom/                  # Doom engine source
|   |   |   +-- riscv/d_main.c    # Platform-specific main
|   |   |   +-- doomdef.h         # PD_FASTTEXT/PD_FASTDATA macros
|   |   +-- doom_pocket.c         # Platform layer (video, input, timing)
|   |   +-- doom_sound.c          # SFX audio driver (async FIFO submission)
|   |   +-- doom_music.c          # MUS parser + hardware OPL3 driver (18 voices)
|   |   +-- doom_net.c            # Link cable network driver
|   |   +-- libc/                 # Minimal C library
|   |   +-- linker.ld             # Linker script (BRAM/SDRAM layout)
|   |   +-- Makefile
|   |
|   +-- fpga/                     # FPGA design (Verilog)
|   |   +-- core/
|   |   |   +-- core_top.v        # Top-level: CPU + bus + peripherals
|   |   |   +-- cpu_system.v      # VexiiRiscv + AXI4 bus routing
|   |   |   +-- axi_periph_slave.v # BRAM, system regs, palette, controllers
|   |   |   +-- axi_sdram_slave.v  # SDRAM AXI4 slave with burst support
|   |   |   +-- axi_sdram_arbiter.v # CPU + video scanout SDRAM arbitration
|   |   |   |   +-- opl2_wrapper.v    # Legacy OPL2 wrapper (unused)
|   |   |   +-- audio_output.v    # I2S output with OPL3+SFX hardware mixing
|   |   |   +-- io_sdram.v        # SDRAM controller
|   |   |   +-- psram_controller.v # PSRAM controller
|   |   |   +-- video_scanout_indexed.v  # 8-bit indexed video scanout
|   |   |   +-- link_mmio.v       # Link cable serial transceiver
|   |   |   +-- text_terminal.v   # Debug text overlay
|   |   +-- opl3/                 # opl3_fpga YMF262 core (Greg Taylor, LGPL-3.0)
|   |   +-- vexriscv/
|   |   |   +-- VexiiRiscv_Full.v # Generated RISC-V CPU core
|   |   +-- apf/                  # Analogue Pocket framework (bridge, I/O)
|   |   +-- Makefile
|   |
|   +-- firmware_test/            # Hardware test firmware (SDRAM/PSRAM/CPU tests)
|
+-- dist/                         # Platform images and icons
+-- release/                      # Packaged release for SD card
+-- tools/
|   +-- capture_ocr.sh            # HDMI capture + OCR testing tool
|   +-- mus2vgm.py                # MUS→VGM converter for offline debugging
+-- Makefile                      # Top-level build/package
+-- deploy.sh                     # Quick deploy to SD card (auto-detect + mount)
+-- *.json                        # APF configuration files
```

## Important Notes

- **JTAG programming loses SDRAM data.** After JTAG programming, the Pocket must reload `doom.bin` and the WAD from the SD card. Always deploy both firmware and bitstream to the SD card for testing.
- **Firmware and FPGA must match.** The BRAM initialization (MIF) is compiled into the bitstream. If `doom.bin` on the SD card doesn't match the MIF in the FPGA, `.fasttext` function calls will jump to wrong addresses and crash.

## Changelog

### v1.0.9

**Save System**
- PSRAM-based saves — eliminates D-cache coherency corruption that could silently corrupt save files
- Per-game save filenames derived from WAD name (DOOM.sav, DOOM2.sav, etc.)

**Video**
- Full 320x240 rendering — no more black bars
- Centered title screens, menus, intermission, and finale on 240-line display
- Fixed status bar widget and weapon hand positions for taller screen
- Smoother lighting — reduced visible light banding
- Fixed uninitialized status bar buffer that could corrupt BRAM rendering functions

**Audio**
- Increased OPL2 music volume — removed excessive attenuation for better music/SFX balance
- Fixed I2S clicking — registered mixer output eliminates race condition between FIFO read and I2S sample load

**Other**
- Initial Analogizer support with HV offset controls (contributed by [@RndMnkIII](https://github.com/RndMnkIII))
- Auto-detect RISC-V cross-compiler prefix in Makefile

## License

- **Doom engine:** GPL-2.0 (id Software)
- **opl3_fpga OPL3 core:** LGPL-3.0 (Greg Taylor)
- **VexiiRiscv:** MIT (SpinalHDL)
- **PocketDoom (FPGA/firmware):** MIT

## Acknowledgments

- [boogermann](https://github.com/boogermann/) — First implementation of Doom on RISC-V, which served as the foundation for this project
- [id Software](https://github.com/id-Software/DOOM) — Original Doom source release
- [gtaylormb/opl3_fpga](https://github.com/gtaylormb/opl3_fpga) — Hardware OPL3 (YMF262) synthesizer core, bit-true reverse-engineered design

- [SpinalHDL/VexiiRiscv](https://github.com/SpinalHDL/VexiiRiscv) — RISC-V CPU core
- [agg23](https://github.com/agg23) — openFPGA core architecture patterns and reference implementations (audio, video scanout, APF bridge integration)
- [Analogue](https://www.analogue.co/developer) — Pocket openFPGA development framework
- [dyreschlock](https://github.com/dyreschlock) — Platform image
