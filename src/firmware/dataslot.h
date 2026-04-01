/*
 * Data Slot interface for Analogue Pocket
 *
 * Provides CPU-controlled data slot operations using APF target commands.
 * The CPU writes to system registers to trigger operations:
 * - Open file into data slot (0x0192)
 * - Read from data slot (0x0180)
 * - Write to data slot (0x0184)
 *
 * Memory map for system registers (base 0x40000000):
 *   0x20: DS_SLOT_ID       - Data slot ID (16-bit)
 *   0x24: DS_SLOT_OFFSET   - Slot offset for read/write
 *   0x28: DS_BRIDGE_ADDR   - Bridge address (maps to SDRAM: bridge 0x00000000 = CPU 0x10000000)
 *   0x2C: DS_LENGTH        - Transfer length in bytes
 *   0x30: DS_PARAM_ADDR    - Address of parameter struct (for openfile)
 *   0x34: DS_RESP_ADDR     - Address of response struct
 *   0x38: DS_COMMAND       - Write to trigger: 1=read, 2=write, 3=openfile
 *   0x3C: DS_STATUS        - Status: bit0=ack, bit1=done, bits[4:2]=err
 */

#ifndef DATASLOT_H
#define DATASLOT_H

#include <stdint.h>
#include <stddef.h>

/* System register addresses */
#define SYS_BASE            0x40000000
#define DS_SLOT_ID          (*(volatile uint32_t*)(SYS_BASE + 0x20))
#define DS_SLOT_OFFSET      (*(volatile uint32_t*)(SYS_BASE + 0x24))
#define DS_BRIDGE_ADDR      (*(volatile uint32_t*)(SYS_BASE + 0x28))
#define DS_LENGTH           (*(volatile uint32_t*)(SYS_BASE + 0x2C))
#define DS_PARAM_ADDR       (*(volatile uint32_t*)(SYS_BASE + 0x30))
#define DS_RESP_ADDR        (*(volatile uint32_t*)(SYS_BASE + 0x34))
#define DS_COMMAND          (*(volatile uint32_t*)(SYS_BASE + 0x38))
#define DS_STATUS           (*(volatile uint32_t*)(SYS_BASE + 0x3C))

/* DS_COMMAND values */
#define DS_CMD_READ         1
#define DS_CMD_WRITE        2

/* SDRAM bridge write drop counter (detects lost writes during DMA).
 * Read: returns drop count since last clear.  Write 1: clears count. */
#define SYS_SDRAM_WR_DROPS  (*(volatile uint32_t*)(SYS_BASE + 0x80))
#define MAX_DMA_RETRIES     3
#define DS_CMD_OPENFILE     3

/* DS_STATUS bits */
#define DS_STATUS_ACK       (1 << 0)
#define DS_STATUS_DONE      (1 << 1)
#define DS_STATUS_ERR_MASK  (7 << 2)
#define DS_STATUS_ERR_SHIFT 2

/* Address conversion: CPU address to bridge address */
/* SDRAM: CPU 0x10000000 = Bridge 0x00000000 */
#define CPU_TO_BRIDGE_ADDR(cpu_addr) ((uint32_t)(cpu_addr) - 0x10000000)
#define BRIDGE_TO_CPU_ADDR(br_addr)  ((uint32_t)(br_addr) + 0x10000000)

/* Uncacheable SDRAM alias: 0x50000000-0x53FFFFFF maps to same physical SDRAM
 * as 0x10000000-0x13FFFFFF but bypasses D-cache.  Use this to read data
 * written by DMA (bridge) without cache coherency issues. */
#define SDRAM_UNCACHED(addr) ((void *)((uint32_t)(addr) + 0x40000000))

/* Shared DMA bounce buffer for dataslot_read callers.
 * After DMA, data must be read through SDRAM_UNCACHED(DMA_BUFFER) to
 * bypass stale D-cache lines, then memcpy'd to the final destination.
 * Placed in the gap between FB1 (ends ~0x10113000) and doom code (0x10200000),
 * safely outside the heap region used by the zone allocator. */
#define DMA_BUFFER       0x10180000          /* Fixed SDRAM address for DMA */
#define DMA_CHUNK_SIZE   (512 * 1024)        /* Max bytes per DMA transfer */


/* Open file parameter structure (256 + 4 + 4 = 264 bytes) */
typedef struct __attribute__((packed)) {
    char     filename[256];   /* Null-terminated path */
    uint32_t flags;           /* bit0: create if missing, bit1: resize/truncate */
    uint32_t size;            /* Desired size if resize flag set */
} dataslot_open_param_t;

/* Open file flags */
#define DS_OPEN_CREATE      (1 << 0)
#define DS_OPEN_RESIZE      (1 << 1)

/*
 * Wait for data slot operation to complete.
 * Returns 0 on success, error code on failure.
 */
int dataslot_wait_complete(void);

/*
 * Open a file into data slot 0.
 * The file path is relative to the Assets directory.
 * Returns 0 on success, negative on error.
 */
int dataslot_open_file(uint16_t slot_id, const char *filename, uint32_t flags, uint32_t size);

/*
 * Read data from a data slot into SDRAM.
 * slot_id: data slot ID (0 for the opened file)
 * offset: byte offset within the slot
 * dest: CPU address in SDRAM to read data into
 * length: number of bytes to read
 * Returns 0 on success, negative on error.
 */
int dataslot_read(uint32_t slot_id, uint32_t offset, void *dest, uint32_t length);

/*
 * Write data from SDRAM to a data slot.
 * slot_id: data slot ID
 * offset: byte offset within the slot
 * src: CPU address in SDRAM to write data from
 * length: number of bytes to write
 * Returns 0 on success, negative on error.
 */
int dataslot_write(uint16_t slot_id, uint32_t offset, const void *src, uint32_t length);

/*
 * Read entire data slot into SDRAM.
 * slot_id: data slot ID
 * dest: CPU address in SDRAM
 * max_length: maximum bytes to read
 * Returns number of bytes read on success, negative on error.
 */
int32_t dataslot_load(uint16_t slot_id, void *dest, uint32_t max_length);

/*
 * Get the size of a data slot.
 * slot_id: data slot ID
 * size_out: pointer to store the size
 * Returns 0 on success, negative on error.
 * Note: Currently returns a fixed max size as slot size query is not implemented.
 */
int dataslot_get_size(uint16_t slot_id, uint32_t *size_out);

#endif /* DATASLOT_H */
