/*
 * Data Slot interface for Analogue Pocket
 *
 * Implements CPU-controlled data slot operations using APF target commands.
 */

#include "dataslot.h"
#include "libc/libc.h"
#include "terminal.h"

/* Set to 1 for verbose dataslot debug output (slow — prints on every read) */
#define DS_DEBUG 0
#if DS_DEBUG
#define DS_LOG(...) term_printf(__VA_ARGS__)
#else
#define DS_LOG(...) do {} while(0)
#endif

/* Parameter buffer in SDRAM — placed at the end of the DMA bounce buffer
 * area (0x10180000 + 512KB) so it stays outside the zone heap. */
#define PARAM_BUFFER_ADDR   0x101F0000  /* CPU address for param struct */
#define RESP_BUFFER_ADDR    0x101F1000  /* CPU address for response struct */

/* Timeout for operations (in loop iterations) */
/* 15 seconds at 133MHz with ~10 cycles/loop = 200M iterations */
#define TIMEOUT_LOOPS       200000000

__attribute__((section(".text.boot")))
int dataslot_wait_complete(void) {
    volatile int timeout = TIMEOUT_LOOPS;
    uint32_t status;

    DS_LOG("wait: initial status=%x\n", DS_STATUS);

    /* First, if ACK is already high from previous command, wait for it to clear.
     * This proves the bridge received our new command and cleared old status. */
    status = DS_STATUS;
    if (status & DS_STATUS_ACK) {
        DS_LOG("wait: ACK high, waiting to clear\n");
        while (DS_STATUS & DS_STATUS_ACK) {
            if (--timeout <= 0) {
                DS_LOG("wait: timeout at clear, t=%d\n", timeout);
                return -3;
            }
        }
        DS_LOG("wait: ACK cleared\n");
    }

    /* Also wait for DONE to clear from previous command.  If we don't,
     * the DONE check below will return immediately with stale status. */
    if (DS_STATUS & DS_STATUS_DONE) {
        DS_LOG("wait: DONE still high, waiting to clear\n");
        while (DS_STATUS & DS_STATUS_DONE) {
            if (--timeout <= 0) {
                DS_LOG("wait: timeout at DONE clear, t=%d\n", timeout);
                return -3;
            }
        }
        DS_LOG("wait: DONE cleared\n");
    }

    /* Wait for this command's ack */
    timeout = TIMEOUT_LOOPS;
    while (!(DS_STATUS & DS_STATUS_ACK)) {
        if (--timeout <= 0) {
            DS_LOG("wait: timeout at ack, t=%d\n", timeout);
            return -1;
        }
    }
    DS_LOG("wait: got ACK\n");

    /* Wait for done */
    timeout = TIMEOUT_LOOPS;
    while (!(DS_STATUS & DS_STATUS_DONE)) {
        if (--timeout <= 0) {
            DS_LOG("wait: timeout at done, t=%d s=%x\n", timeout, DS_STATUS);
            return -2;
        }
    }
    DS_LOG("wait: got DONE\n");

    /* Check error code */
    uint32_t final_status = DS_STATUS;
    int err = (final_status & DS_STATUS_ERR_MASK) >> DS_STATUS_ERR_SHIFT;
    DS_LOG("wait: final status=%x err=%d\n", final_status, err);
    return err ? -err : 0;
}

__attribute__((section(".text.boot")))
int dataslot_open_file(uint16_t slot_id, const char *filename, uint32_t flags, uint32_t size) {
    /* Build parameter struct in SDRAM */
    dataslot_open_param_t *param = (dataslot_open_param_t *)PARAM_BUFFER_ADDR;

    /* Clear and fill the struct */
    memset(param, 0, sizeof(*param));
    strncpy(param->filename, filename, 255);
    param->filename[255] = '\0';
    param->flags = flags;
    param->size = size;

    /* Set up registers */
    DS_SLOT_ID = slot_id;
    DS_PARAM_ADDR = CPU_TO_BRIDGE_ADDR(PARAM_BUFFER_ADDR);
    DS_RESP_ADDR = CPU_TO_BRIDGE_ADDR(RESP_BUFFER_ADDR);

    /* Trigger openfile command */
    DS_COMMAND = DS_CMD_OPENFILE;

    /* Wait for completion */
    return dataslot_wait_complete();
}

__attribute__((section(".text.boot")))
int dataslot_read(uint32_t slot_id, uint32_t offset, void *dest, uint32_t length) {
    /* Validate destination is in SDRAM */
    uint32_t dest_addr = (uint32_t)dest;
    if (dest_addr < 0x10000000 || dest_addr >= 0x14000000) {
        return -10;  /* Invalid destination address */
    }

    uint32_t bridge_addr = CPU_TO_BRIDGE_ADDR(dest_addr);

    DS_LOG("DS: slot=%d off=%x br=%x len=%x\n",
                slot_id, offset, bridge_addr, length);

    __asm__ volatile("fence");

    int result;
    for (int attempt = 0; attempt <= MAX_DMA_RETRIES; attempt++) {
        /* Clear the SDRAM write drop counter before each attempt */
        SYS_SDRAM_WR_DROPS = 1;
        for (volatile int i = 0; i < 16; i++) {}  /* CDC settle */

        /* Set up registers and trigger */
        DS_SLOT_ID = slot_id;
        DS_SLOT_OFFSET = offset;
        DS_BRIDGE_ADDR = bridge_addr;
        DS_LENGTH = length;
        DS_COMMAND = DS_CMD_READ;

        result = dataslot_wait_complete();
        if (result < 0)
            return result;

        /* Wait for bridge write pipeline to settle (skid → dcfifo → SDRAM) */
        for (volatile int i = 0; i < 32; i++) {}

        /* Check if any bridge writes were dropped during the transfer */
        uint32_t drops = SYS_SDRAM_WR_DROPS & 0xFFFF;
        if (drops == 0)
            break;  /* Clean transfer — no retry needed */

        /* Drops detected — retry (data may be corrupted) */
        DS_LOG("DS: %d drops on attempt %d, retrying\n", drops, attempt);
    }

    return result;
}

__attribute__((section(".text.boot")))
int dataslot_write(uint16_t slot_id, uint32_t offset, const void *src, uint32_t length) {
    /* Validate source is in SDRAM */
    uint32_t src_addr = (uint32_t)src;
    if (src_addr < 0x10000000 || src_addr >= 0x14000000) {
        return -10;  /* Invalid source address */
    }

    /* Set up registers */
    DS_SLOT_ID = slot_id;
    DS_SLOT_OFFSET = offset;
    DS_BRIDGE_ADDR = CPU_TO_BRIDGE_ADDR(src_addr);
    DS_LENGTH = length;

    /* Trigger write command */
    DS_COMMAND = DS_CMD_WRITE;

    /* Wait for completion */
    return dataslot_wait_complete();
}

__attribute__((section(".text.boot")))
int32_t dataslot_load(uint16_t slot_id, void *dest, uint32_t max_length) {
    /* For now, just read the requested amount */
    /* TODO: Could query slot size first if needed */
    int result = dataslot_read(slot_id, 0, dest, max_length);
    if (result < 0) return result;
    return (int32_t)max_length;
}

__attribute__((section(".text.boot")))
int dataslot_get_size(uint16_t slot_id, uint32_t *size_out) {
    /* TODO: Implement proper slot size query via APF protocol */
    /* For now, return a large fixed size based on slot ID */
    if (size_out == NULL) return -1;

    switch (slot_id) {
        case 0:  /* WAD data */
            *size_out = 20 * 1024 * 1024;  /* 20 MB */
            break;
        case 1:  /* Doom binary */
            *size_out = 4 * 1024 * 1024;   /* 4 MB */
            break;
        default:
            *size_out = 1 * 1024 * 1024;   /* 1 MB default */
            break;
    }
    return 0;
}
