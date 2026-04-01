/*
 * File I/O emulation for VexRiscv
 * Uses data slots for loading files into SDRAM
 */

#include "libc.h"
#include "../dataslot.h"

/* Standard file descriptors (unused but defined for compatibility) */
static FILE stdin_file = {0, 0, 0, 0, NULL};
static FILE stdout_file = {0, 0, 0, 0, NULL};
static FILE stderr_file = {0, 0, 0, 0, NULL};

FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

/* File table for open files */
#define MAX_OPEN_FILES 4
static FILE file_table[MAX_OPEN_FILES];
static int file_table_used[MAX_OPEN_FILES] = {0};

/* Find a free file slot */
static FILE *alloc_file(void) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!file_table_used[i]) {
            file_table_used[i] = 1;
            memset(&file_table[i], 0, sizeof(FILE));
            return &file_table[i];
        }
    }
    return NULL;
}

/* Free a file slot */
static void free_file(FILE *f) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (&file_table[i] == f) {
            file_table_used[i] = 0;
            return;
        }
    }
}

/* Data slot IDs (match data.json) */
#define WAD_SLOT_ID      0    /* IWAD (required) */
#define PWAD_SLOT_ID     2    /* PWAD mod (optional) */
#define CFG_SLOT_ID      3    /* default.cfg */
#define CFG_MAX_SIZE     2048 /* Max config file size to load */

/* Check if string ends with suffix (case-insensitive) */
static int str_ends_with_ci(const char *str, const char *suffix) {
    int str_len = strlen(str);
    int suf_len = strlen(suffix);
    if (suf_len > str_len) return 0;
    return strncasecmp(str + str_len - suf_len, suffix, suf_len) == 0;
}

/* Track how many WAD files have been opened (IWAD first, then PWAD) */
static int wad_open_count = 0;

static const char *path_basename(const char *path);
static int is_cfg_file(const char *pathname);

/* Map filename to data slot ID */
static int filename_to_slot(const char *pathname) {
    if (str_ends_with_ci(pathname, ".wad")) {
        /* First .wad open = IWAD (slot 0), second = PWAD (slot 2) */
        return (wad_open_count == 0) ? WAD_SLOT_ID : PWAD_SLOT_ID;
    }
    if (is_cfg_file(pathname)) {
        return CFG_SLOT_ID;
    }

    /* Unknown file */
    return -1;
}

/* ============================================
 * Save/Config file support (single nonvolatile slot 10)
 *
 * One nonvolatile .sav file holds all 6 sub-saves (384KB total).
 * Bridge auto-loads SD → CRAM1 at boot, auto-saves CRAM1 → SD at
 * shutdown.  Derive flag (0x04) gives per-game filenames from the
 * IWAD name (e.g. DOOM.sav, DOOM2.sav).
 *
 * CRAM1 layout: 6 × 64KB sub-saves at offsets 0x00000–0x50000.
 * Each sub-save: [4-byte game_id] [4-byte size] [save data]
 *
 * At boot the firmware erases (zeros) any sub-save whose stored
 * game_id doesn't match the current game.
 *
 * CPU reads/writes CRAM1 directly (uncached PSRAM — no D-cache
 * coherency issues).
 *
 * Config (default.cfg) is read-only from deferload data slot 3.
 * ============================================ */

#define FILE_FLAG_WRITE     1
#define FILE_FLAG_CFG       2   /* config file — flush to SD via dataslot_write */

#define SAV_CRAM1_ADDR      0x30000000   /* CRAM1 save region (uncached PSRAM) */
#define SAV_REGION_UC       ((volatile uint8_t *)SAV_CRAM1_ADDR)
#define SAV_SLOT_COUNT      6            /* Save slots 0-5 */

/* Config PSRAM region — separate nonvolatile slot at 0x30060000 (after saves) */
#define CFG_PSRAM_ADDR      0x30060000
#define CFG_PSRAM_UC        ((volatile uint8_t *)CFG_PSRAM_ADDR)
#define CFG_PSRAM_SIZE      0x1000       /* 4KB */

/* Validate that PSRAM config data looks like Doom key-value config.
 * Returns the size of valid config text, or 0 if invalid.
 * Valid config: printable ASCII lines with at least one tab separator.
 * Rejects uninitialized PSRAM (0xFF), binary garbage, or all zeros. */
static uint32_t cfg_validate_psram(void) {
    int has_tab = 0;
    int has_newline = 0;
    uint32_t size = 0;
    uint32_t max = CFG_PSRAM_SIZE < CFG_MAX_SIZE ? CFG_PSRAM_SIZE : CFG_MAX_SIZE;

    for (uint32_t i = 0; i < max; i++) {
        uint8_t b = CFG_PSRAM_UC[i];
        if (b == 0) break;
        if (b == '\t') has_tab = 1;
        else if (b == '\n') has_newline = 1;
        else if (b != '\r' && (b < 0x20 || b > 0x7E))
            return 0;  /* non-printable byte = not valid config */
        size = i + 1;
    }

    /* Valid config has at least one key\tvalue\n line */
    if (size < 3 || !has_tab || !has_newline)
        return 0;

    return size;
}
#define SAV_SLOT_SIZE       0x10000      /* 64KB per slot in PSRAM (header + compressed) */
#define SAV_SLOT_ID_BASE    10           /* Data slot ID (single nonvolatile slot) */
#define SAV_V2_HEADER_SIZE  16           /* magic[4] + game_id[4] + comp_size[4] + uncomp_size[4] */
#define SAV_PAD_SIZE        4            /* Padding before header (absorbs burst-mode corruption) */
#define SAV_V1_HEADER_SIZE  8            /* game_id[4] + size[4] (old format) */
#define SAV_BUF_SIZE        0x30000      /* 192KB uncompressed buffer (SAVEGAMESIZE = 180KB) */

/* Old v1 max data size (for validation during migration) */
#define SAV_V1_MAX_DATA     (0x10000 - SAV_V1_HEADER_SIZE)

/* Game ID register — written by instance JSON memory_writes via bridge.
 * Doom=1, Doom2=2, UltimateDoom=3, TNT=4, Plutonia=5.
 * If 0, we derive an ID from the WAD headers at first use. */
#define SYS_GAME_ID     (*(volatile uint32_t *)0x40000068)

/* Auto-derived game ID from WAD headers (used when SYS_GAME_ID == 0) */
static uint32_t sav_derived_id = 0;
static int      sav_derived_id_valid = 0;

/* FNV-1a hash — simple, good distribution */
static uint32_t fnv1a(const uint8_t *data, int len, uint32_t hash) {
    for (int i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 0x01000193;
    }
    return hash;
}

/* Derive a game ID by hashing the IWAD + PWAD headers.
 * Each WAD header is 12 bytes: magic[4] + numlumps[4] + diroffset[4].
 * This uniquely identifies the WAD combination.
 * Bit 31 is set to avoid collision with manual IDs (1-5). */
static uint32_t sav_get_game_id(void) {
    uint32_t id = SYS_GAME_ID;
    if (id != 0) return id;

    if (sav_derived_id_valid) return sav_derived_id;
    sav_derived_id_valid = 1;

    uint32_t hash = 0x811c9dc5;  /* FNV offset basis */

    /* Read IWAD header (slot 0, 12 bytes) */
    if (dataslot_read(0, 0, (void *)DMA_BUFFER, 12) == 0) {
        uint8_t hdr[12];
        memcpy(hdr, SDRAM_UNCACHED(DMA_BUFFER), 12);
        hash = fnv1a(hdr, 12, hash);
    }

    /* Read PWAD header (slot 2, 12 bytes) — may not exist */
    if (dataslot_read(2, 0, (void *)DMA_BUFFER, 12) == 0) {
        uint8_t hdr[12];
        memcpy(hdr, SDRAM_UNCACHED(DMA_BUFFER), 12);
        hash = fnv1a(hdr, 12, hash);
    }

    /* Set bit 31 to distinguish from manual IDs (1-5) */
    hash |= 0x80000000;
    if (hash == 0x80000000) hash = 0x80000001;  /* avoid edge case */

    sav_derived_id = hash;
    return hash;
}

/* Single shared buffer for save/config file operations.
 * Only one save file can be open for writing at a time. */
static char sav_buf[SAV_BUF_SIZE] __attribute__((section(".bss")));

/* Which sub-save is currently open for writing (-1 = none) */
static int sav_write_sub_idx = -1;   /* 0-5 = sub-save index */

/* Return pointer to basename (after last '/') */
static const char *path_basename(const char *pathname) {
    const char *last = pathname;
    while (*pathname) {
        if (*pathname == '/')
            last = pathname + 1;
        pathname++;
    }
    return last;
}

/* Extract Doom save slot number from filename.
 * "doomsav0.dsg" → 0, "doomsav5.dsg" → 5.  Returns -1 if not a save file. */
static int sav_slot_from_name(const char *pathname) {
    const char *base = path_basename(pathname);
    /* Match "doomsav%d.dsg" */
    if (strncmp(base, "doomsav", 7) != 0) return -1;
    char c = base[7];
    if (c < '0' || c > '5') return -1;
    if (strcmp(base + 8, ".dsg") != 0) return -1;
    return c - '0';
}

/* Check if path is a .dsg save file */
static int is_save_file(const char *pathname) {
    return str_ends_with_ci(pathname, ".dsg");
}

/* Check if path is a config file (.cfg or "doomrc") */
static int is_cfg_file(const char *pathname) {
    if (str_ends_with_ci(pathname, ".cfg")) return 1;
    const char *base = path_basename(pathname);
    if (strcasecmp(base, "doomrc") == 0) return 1;
    return 0;
}

static void flush_dcache(void);
extern void term_printf(const char *fmt, ...);

/* ============================================
 * LZSS compression for save data
 *
 * Format: stream of 9-byte groups (1 flag byte + up to 8 tokens).
 * Flag bit 0 = literal (1 byte), 1 = match (2 bytes).
 * Match encoding: byte0 = offset_lo, byte1 = (offset_hi:4 | length-3:4).
 * Offset 1-4096, length 3-18.
 * ============================================ */
#define LZ_WINDOW    4096
#define LZ_MIN_MATCH 3
#define LZ_MAX_MATCH 18
#define LZ_HASH_BITS 12
#define LZ_HASH_SIZE (1 << LZ_HASH_BITS)

static int lz_htab[LZ_HASH_SIZE];

static inline uint32_t lz_hash3(const uint8_t *p) {
    return (((uint32_t)p[0] << 5) ^ ((uint32_t)p[1] << 3) ^ p[2])
           & (LZ_HASH_SIZE - 1);
}

/* Compress src[0..slen-1] → dst[0..dmax-1].
 * Returns compressed size, or -1 if output overflows dmax. */
static int sav_compress(const uint8_t *src, int slen,
                        volatile uint8_t *dst, int dmax)
{
    for (int i = 0; i < LZ_HASH_SIZE; i++) lz_htab[i] = -1;
    int si = 0, di = 0;

    while (si < slen) {
        if (di >= dmax) return -1;
        int flag_pos = di++;
        uint8_t flags = 0;

        for (int bit = 0; bit < 8 && si < slen; bit++) {
            int best_len = 0, best_off = 0;

            if (si + LZ_MIN_MATCH <= slen) {
                uint32_t h = lz_hash3(src + si);
                int cand = lz_htab[h];
                lz_htab[h] = si;

                if (cand >= 0 && (si - cand) <= LZ_WINDOW) {
                    int len = 0;
                    int max_len = slen - si;
                    if (max_len > LZ_MAX_MATCH) max_len = LZ_MAX_MATCH;
                    while (len < max_len && src[cand + len] == src[si + len])
                        len++;
                    if (len >= LZ_MIN_MATCH) {
                        best_len = len;
                        best_off = si - cand;
                    }
                }
            }

            if (best_len >= LZ_MIN_MATCH) {
                flags |= (1 << bit);
                if (di + 2 > dmax) return -1;
                int off_enc = best_off - 1;
                dst[di++] = (uint8_t)(off_enc & 0xFF);
                dst[di++] = (uint8_t)(((off_enc >> 8) & 0x0F) << 4
                                      | (best_len - LZ_MIN_MATCH));
                si += best_len;
            } else {
                if (di + 1 > dmax) return -1;
                dst[di++] = src[si++];
            }
        }
        dst[flag_pos] = flags;
    }
    return di;
}

/* Decompress src[0..slen-1] → dst[0..dmax-1].
 * Returns decompressed size, or -1 on error. */
static int sav_decompress(volatile const uint8_t *src, int slen,
                          uint8_t *dst, int dmax)
{
    int si = 0, di = 0;

    while (si < slen) {
        uint8_t flags = src[si++];
        for (int bit = 0; bit < 8; bit++) {
            if (flags & (1 << bit)) {
                /* Back-reference */
                if (si + 2 > slen) goto done;
                int b0 = src[si++];
                int b1 = src[si++];
                int offset = (b0 | ((b1 >> 4) << 8)) + 1;
                int length = (b1 & 0x0F) + LZ_MIN_MATCH;
                if (di - offset < 0 || di + length > dmax) return -1;
                for (int k = 0; k < length; k++)
                    dst[di + k] = dst[di - offset + k];
                di += length;
            } else {
                /* Literal */
                if (si >= slen) goto done;
                if (di >= dmax) return -1;
                dst[di++] = src[si++];
            }
        }
    }
done:
    return di;
}

/* ============================================
 * Save slot management (v2 with compression, v1 backward compat)
 *
 * v2 per-slot layout in PSRAM (128KB):
 *   [0]  magic "PD2C" (compressed) or "PD2R" (raw)
 *   [4]  game_id
 *   [8]  compressed_size  (== uncomp_size if raw)
 *   [12] uncompressed_size
 *   [16] data (compressed or raw)
 *
 * v1 per-slot layout (old 64KB format, read-only for migration):
 *   [0]  game_id
 *   [4]  size
 *   [8]  data (uncompressed)
 * ============================================ */
#define SYS_DISPLAY_MODE_F (*(volatile uint32_t *)0x4000000C)

/* ============================================
 * PSRAM Slot Abstraction Layer
 *
 * The first SAV_PAD_SIZE bytes of each PSRAM slot are sacrificial — they
 * may be corrupted during burst-mode transitions or power events.  All
 * reads and writes go through these helpers so callers don't need to
 * worry about the padding.
 *
 * Write:  sav_slot_wbase() returns pointer after pad, writes pad bytes.
 * Read:   sav_slot_rbase() scans for a known marker at offset 0 and
 *         SAV_PAD_SIZE, returning whichever is valid (backward compat
 *         with old unpadded saves).
 * Capacity: sav_slot_capacity() returns usable bytes per slot (minus pad).
 * ============================================ */

/* Check if 4 bytes at p[] are a v2 magic ("PD2C" or "PD2R") */
static int sav_magic_at(volatile uint8_t *p) {
    return p[0] == 'P' && p[1] == 'D' && p[2] == '2'
        && (p[3] == 'C' || p[3] == 'R');
}

/* Raw PSRAM pointer for a slot (offset 0, including pad zone). */
static volatile uint8_t *sav_slot_raw(int slot_idx) {
    return SAV_REGION_UC + (uint32_t)slot_idx * SAV_SLOT_SIZE;
}

/* Write base: returns pointer to the safe region after the pad.
 * Writes the sacrificial padding bytes (0xA5 pattern). */
static volatile uint8_t *sav_slot_wbase(int slot_idx) {
    volatile uint8_t *slot = sav_slot_raw(slot_idx);
    for (int i = 0; i < SAV_PAD_SIZE; i++)
        slot[i] = 0xA5;
    return slot + SAV_PAD_SIZE;
}

/* Read base: finds the actual header start by scanning for the v2 magic.
 * Checks SAV_PAD_SIZE first (new padded format), then offset 0 (old format).
 * Returns pointer to the header, or NULL if no valid header found. */
static volatile uint8_t *sav_slot_rbase(int slot_idx) {
    volatile uint8_t *slot = sav_slot_raw(slot_idx);
    if (sav_magic_at(slot + SAV_PAD_SIZE)) return slot + SAV_PAD_SIZE;
    if (sav_magic_at(slot))                return slot;
    return NULL;  /* No v2 header found */
}

/* Usable capacity per slot (total minus sacrificial pad). */
static uint32_t sav_slot_capacity(void) {
    return SAV_SLOT_SIZE - SAV_PAD_SIZE;
}

/* Read game_id from a PSRAM slot header (v1 or v2, with or without pad). */
static uint32_t sav_slot_game_id(volatile uint8_t *slot) {
    /* Try v2 at padded offset first, then offset 0 */
    if (sav_magic_at(slot + SAV_PAD_SIZE)) {
        volatile uint32_t *w = (volatile uint32_t *)(slot + SAV_PAD_SIZE);
        return w[1];
    }
    if (sav_magic_at(slot)) {
        volatile uint32_t *w = (volatile uint32_t *)slot;
        return w[1];
    }
    /* v1 (no magic): game_id is at word 0 */
    return ((volatile uint32_t *)slot)[0];
}

static int sav_slots_cleared = 0;
static void sav_clear_incompatible(void) {
    if (sav_slots_cleared) return;
    sav_slots_cleared = 1;

    uint32_t my_id = sav_get_game_id();
    if (my_id == 0) return;

    for (int i = 0; i < SAV_SLOT_COUNT; i++) {
        volatile uint8_t *raw = sav_slot_raw(i);
        uint32_t stored_id = sav_slot_game_id(raw);

        if (stored_id == 0 || stored_id == 0xFFFFFFFF || stored_id == my_id)
            continue;

        /* Erase mismatched slot */
        for (uint32_t j = 0; j < SAV_SLOT_SIZE; j++)
            raw[j] = 0;
    }
}

/* Read save data from PSRAM slot into sav_buf (decompressing if v2).
 * Returns uncompressed size on success, 0 on empty/incompatible. */
static uint32_t sav_read_from_slot(int slot_idx) {
    sav_clear_incompatible();

    uint32_t my_id = sav_get_game_id();

    /* Try v2 format via the slot abstraction layer (handles padding) */
    volatile uint8_t *base = sav_slot_rbase(slot_idx);
    if (base) {
        volatile uint32_t *hdr = (volatile uint32_t *)base;
        uint32_t game_id     = hdr[1];
        uint32_t comp_size   = hdr[2];
        uint32_t uncomp_size = hdr[3];

        /* Compute max data size from base to end of slot */
        uint32_t base_off = (uint32_t)(base - sav_slot_raw(slot_idx));
        uint32_t max_data = SAV_SLOT_SIZE - base_off - SAV_V2_HEADER_SIZE;

        if (my_id != 0 && game_id != my_id)
            return 0;
        if (uncomp_size == 0 || uncomp_size > SAV_BUF_SIZE)
            return 0;
        if (comp_size == 0 || comp_size > max_data)
            return 0;

        memset(sav_buf, 0, SAV_BUF_SIZE);

        if (base[3] == 'C') {
            int result = sav_decompress(base + SAV_V2_HEADER_SIZE, (int)comp_size,
                                        (uint8_t *)sav_buf, (int)uncomp_size);
            if (result < 0 || (uint32_t)result != uncomp_size)
                return 0;
        } else {
            volatile uint8_t *src = base + SAV_V2_HEADER_SIZE;
            for (uint32_t i = 0; i < uncomp_size; i++)
                ((uint8_t *)sav_buf)[i] = src[i];
        }
        return uncomp_size;
    }

    /* v1 format (backward compat, no padding): [game_id][size][data]
     * Apply strict validation to avoid treating uninitialized PSRAM
     * (garbage after a short bridge load) as a valid save. */
    volatile uint8_t *raw = sav_slot_raw(slot_idx);
    volatile uint32_t *hdr = (volatile uint32_t *)raw;
    uint32_t game_id    = hdr[0];
    uint32_t saved_size = hdr[1];

    if (my_id != 0 && game_id != my_id)
        return 0;
    if (saved_size == 0 || saved_size == 0xFFFFFFFF || saved_size > SAV_V1_MAX_DATA)
        return 0;
    /* v1 game_id must be a known manual ID (1-5) — reject garbage values.
     * Derived IDs (bit 31 set) only exist in v2 format. */
    if (game_id == 0 || game_id > 5)
        return 0;
    /* Doom saves start with a version string like " E1M1" or " MAP01".
     * The first data byte (after header) must be 0x20 (space). */
    if (raw[SAV_V1_HEADER_SIZE] != 0x20)
        return 0;

    memset(sav_buf, 0, SAV_BUF_SIZE);
    volatile uint8_t *src = raw + SAV_V1_HEADER_SIZE;
    for (uint32_t i = 0; i < saved_size; i++)
        ((uint8_t *)sav_buf)[i] = src[i];

    return saved_size;
}

/* Flush all dirty D-cache lines to physical SDRAM by reading 128KB
 * of unrelated memory, evicting every line in the 64KB 2-way cache. */
#define CACHE_FLUSH_ADDR    0x10380000
#define CACHE_FLUSH_SIZE    (128 * 1024)
static void flush_dcache(void) {
    volatile uint32_t *p = (volatile uint32_t *)CACHE_FLUSH_ADDR;
    volatile uint32_t sink;
    for (int i = 0; i < (int)(CACHE_FLUSH_SIZE / 4); i++)
        sink = p[i];
    (void)sink;
}

/* Compress sav_buf and write to PSRAM slot (v2 format).
 * Tries LZSS first; falls back to raw if compression doesn't help
 * or if data fits uncompressed. */
static void sav_persist(int slot_idx, uint32_t actual_size) {
    /* sav_slot_wbase writes the sacrificial pad and returns the safe area */
    volatile uint8_t *hdr_base = sav_slot_wbase(slot_idx);
    volatile uint32_t *hdr = (volatile uint32_t *)hdr_base;
    uint32_t data_max = sav_slot_capacity() - SAV_V2_HEADER_SIZE;
    uint32_t my_id = sav_get_game_id();

    /* Try compression */
    int comp_size = sav_compress((const uint8_t *)sav_buf, (int)actual_size,
                                 hdr_base + SAV_V2_HEADER_SIZE, (int)data_max);

    if (comp_size > 0 && (uint32_t)comp_size < actual_size) {
        /* Compressed and smaller — write PD2C header */
        hdr_base[0] = 'P'; hdr_base[1] = 'D'; hdr_base[2] = '2'; hdr_base[3] = 'C';
        hdr[1] = my_id;
        hdr[2] = (uint32_t)comp_size;
        hdr[3] = actual_size;
    } else if (actual_size <= data_max) {
        /* Store raw (compression didn't help) — write PD2R header */
        hdr_base[0] = 'P'; hdr_base[1] = 'D'; hdr_base[2] = '2'; hdr_base[3] = 'R';
        hdr[1] = my_id;
        hdr[2] = actual_size;
        hdr[3] = actual_size;
        volatile uint8_t *dst = hdr_base + SAV_V2_HEADER_SIZE;
        for (uint32_t i = 0; i < actual_size; i++)
            dst[i] = ((uint8_t *)sav_buf)[i];
    } else {
        /* Data too large even uncompressed — truncate as last resort */
        hdr_base[0] = 'P'; hdr_base[1] = 'D'; hdr_base[2] = '2'; hdr_base[3] = 'R';
        hdr[1] = my_id;
        hdr[2] = data_max;
        hdr[3] = data_max;
        volatile uint8_t *dst = hdr_base + SAV_V2_HEADER_SIZE;
        for (uint32_t i = 0; i < data_max; i++)
            dst[i] = ((uint8_t *)sav_buf)[i];
    }
}



/* Read WAD header + directory from a data slot to determine file size.
 * WAD header: char[4] magic, int32 numlumps, int32 diroffset
 * Some PWADs place the directory before lump data, so we must scan all
 * directory entries to find the true file extent. */
static uint32_t wad_get_size(int slot_id) {
    uint32_t header[3];
    if (dataslot_read(slot_id, 0, (void *)DMA_BUFFER, 12) != 0)
        return 0;
    memcpy(header, SDRAM_UNCACHED(DMA_BUFFER), 12);

    uint32_t magic = header[0];
    if (magic != 0x44415749 && magic != 0x44415750)  /* IWAD / PWAD */
        return 0;

    uint32_t numlumps = header[1];
    uint32_t diroffset = header[2];
    uint32_t dir_end = diroffset + numlumps * 16;

    /* Scan directory entries to find max(filepos + size).
     * Each entry is 16 bytes: int32 filepos, int32 size, char[8] name.
     * Read in chunks via DMA bounce buffer. */
    uint32_t max_end = dir_end;
    uint32_t dir_remaining = numlumps * 16;
    uint32_t dir_off = diroffset;

    while (dir_remaining > 0) {
        uint32_t chunk = dir_remaining > DMA_CHUNK_SIZE ? DMA_CHUNK_SIZE : dir_remaining;
        if (dataslot_read(slot_id, dir_off, (void *)DMA_BUFFER, chunk) != 0)
            break;
        uint32_t *entries = (uint32_t *)SDRAM_UNCACHED(DMA_BUFFER);
        uint32_t n_entries = chunk / 16;
        for (uint32_t i = 0; i < n_entries; i++) {
            uint32_t lump_pos  = entries[i * 4 + 0];
            uint32_t lump_size = entries[i * 4 + 1];
            uint32_t lump_end = lump_pos + lump_size;
            if (lump_end > max_end)
                max_end = lump_end;
        }
        dir_off += chunk;
        dir_remaining -= chunk;
    }

    return max_end;
}

/* ============================================
 * High-level file operations
 * ============================================ */

FILE *fopen(const char *pathname, const char *mode) {
    /* Config file: read-only from deferload slot 3.
     * Read in small chunks to avoid reading past EOF (bridge errors). */
    if (is_cfg_file(pathname)) {
        if (mode[0] == 'w') {
            /* Write mode: buffer in sav_buf, persist to PSRAM on fclose */
            FILE *f = alloc_file();
            if (!f) return NULL;
            memset(sav_buf, 0, CFG_MAX_SIZE);
            f->slot_id = CFG_SLOT_ID;
            f->offset = 0;
            f->size = CFG_MAX_SIZE;
            f->flags = FILE_FLAG_WRITE | FILE_FLAG_CFG;
            f->data = sav_buf;
            return f;
        }

        /* Read mode: try PSRAM config region first (previously saved),
         * fall back to SD card deferload slot 3 (original default.cfg).
         * Validates that PSRAM contains parseable config (printable ASCII
         * with key\tvalue\n lines).  Hand-edited files are accepted. */
        uint32_t cfg_size = cfg_validate_psram();
        if (cfg_size > 0) {
            memset(sav_buf, 0, CFG_MAX_SIZE);
            for (uint32_t i = 0; i < cfg_size; i++)
                sav_buf[i] = CFG_PSRAM_UC[i];
        }
        if (cfg_size == 0) {
            /* No saved config in PSRAM — load from SD card */
            memset(sav_buf, 0, CFG_MAX_SIZE);
            flush_dcache();
            cfg_size = 0;
            uint32_t chunk = 512;
            while (cfg_size < CFG_MAX_SIZE) {
                uint32_t want = CFG_MAX_SIZE - cfg_size;
                if (want > chunk) want = chunk;
                if (dataslot_read(CFG_SLOT_ID, cfg_size,
                                  (void *)DMA_BUFFER, want) != 0)
                    break;
                memcpy(sav_buf + cfg_size, SDRAM_UNCACHED(DMA_BUFFER), want);
                cfg_size += want;
            }
            /* Trim trailing NULs */
            while (cfg_size > 0 && sav_buf[cfg_size - 1] == '\0')
                cfg_size--;
        }
        if (cfg_size == 0) return NULL;
        FILE *f = alloc_file();
        if (!f) return NULL;
        f->slot_id = CFG_SLOT_ID;
        f->offset = 0;
        f->size = cfg_size;
        f->flags = 0;
        f->data = sav_buf;
        return f;
    }

    /* All other writes not supported via fopen */
    if (mode[0] == 'w') return NULL;

    /* WAD file read mode */
    int slot_id = filename_to_slot(pathname);
    if (slot_id == -1) {
        return NULL;  /* Unknown file */
    }

    FILE *f = alloc_file();
    if (f == NULL) {
        return NULL;
    }

    f->slot_id = slot_id;
    f->offset = 0;
    f->flags = 0;
    f->data = NULL;  /* On-demand: read via dataslot_read */

    if (str_ends_with_ci(pathname, ".wad")) {
        /* Parse WAD header to get real file size */
        f->size = wad_get_size(slot_id);
        if (f->size == 0) {
            free_file(f);
            return NULL;
        }
        wad_open_count++;
    } else {
        /* Get slot size from dataslot system */
        if (dataslot_get_size(slot_id, &f->size) != 0) {
            free_file(f);
            return NULL;
        }
    }

    return f;
}

int fclose(FILE *stream) {
    if (stream == NULL) {
        return -1;
    }

    /* Config write-back: persist to dedicated PSRAM region at 0x30060000.
     * Bridge auto-saves to SD on shutdown (separate nonvolatile slot).
     * Update datatable with actual size so the .cfg file has no padding. */
    if ((stream->flags & FILE_FLAG_CFG) && stream->offset > 0) {
        uint32_t sz = stream->offset;
        if (sz > CFG_PSRAM_SIZE) sz = CFG_PSRAM_SIZE;
        volatile uint8_t *dst = CFG_PSRAM_UC;
        for (uint32_t i = 0; i < sz; i++)
            dst[i] = (uint8_t)sav_buf[i];
        /* Zero-fill remainder for clean validation on next boot */
        for (uint32_t i = sz; i < CFG_PSRAM_SIZE; i++)
            dst[i] = 0;
    }
    /* Save write-back: persist to PSRAM */
    else if ((stream->flags & FILE_FLAG_WRITE) && stream->offset > 0 && sav_write_sub_idx >= 0) {
        sav_persist(sav_write_sub_idx, stream->offset);
        sav_write_sub_idx = -1;
    }

    free_file(stream);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (stream == NULL || ptr == NULL || size == 0 || nmemb == 0) {
        return 0;
    }

    size_t total_bytes = size * nmemb;
    size_t available = stream->size - stream->offset;

    if (total_bytes > available) {
        total_bytes = available;
        nmemb = total_bytes / size;
        total_bytes = nmemb * size;  /* Round down to whole elements */
    }

    if (total_bytes == 0) {
        return 0;
    }

    /* If data is loaded in memory (via mmap), copy directly from it */
    if (stream->data != NULL) {
        memcpy(ptr, (uint8_t *)stream->data + stream->offset, total_bytes);
        stream->offset += total_bytes;
        return nmemb;
    }

    /* DMA to bounce buffer, then copy via uncacheable alias to avoid
     * stale D-cache lines at the destination address. */
    uint8_t *dest = (uint8_t *)ptr;
    size_t remaining = total_bytes;
    while (remaining > 0) {
        size_t chunk = remaining > DMA_CHUNK_SIZE ? DMA_CHUNK_SIZE : remaining;
        if (dataslot_read(stream->slot_id, stream->offset, (void *)DMA_BUFFER, chunk) != 0) {
            return 0;
        }
        memcpy(dest, SDRAM_UNCACHED(DMA_BUFFER), chunk);
        dest += chunk;
        stream->offset += chunk;
        remaining -= chunk;
    }
    return nmemb;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (stream && (stream->flags & FILE_FLAG_WRITE)) {
        size_t total = size * nmemb;
        size_t remaining = stream->size - stream->offset;
        if (total > remaining) total = remaining;
        if (total > 0) {
            memcpy((char *)stream->data + stream->offset, ptr, total);
            stream->offset += total;
        }
        return total / size;
    }
    return 0;
}

int fseek(FILE *stream, long offset, int whence) {
    if (stream == NULL) {
        return -1;
    }

    long new_offset;

    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = (long)stream->offset + offset;
            break;
        case SEEK_END:
            new_offset = (long)stream->size + offset;
            break;
        default:
            return -1;
    }

    if (new_offset < 0 || (size_t)new_offset > stream->size) {
        return -1;
    }

    stream->offset = (uint32_t)new_offset;
    return 0;
}

long ftell(FILE *stream) {
    if (stream == NULL) {
        return -1;
    }
    return (long)stream->offset;
}

void rewind(FILE *stream) {
    if (stream != NULL) {
        stream->offset = 0;
    }
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;  /* Nothing to flush for read-only files */
}

int feof(FILE *stream) {
    if (stream == NULL) {
        return 1;
    }
    return stream->offset >= stream->size;
}

int ferror(FILE *stream) {
    (void)stream;
    return 0;  /* No error tracking implemented */
}

/* ============================================
 * Formatted I/O (minimal implementation)
 * ============================================ */

int vfprintf(FILE *stream, const char *format, va_list args) {
    if (stream && (stream->flags & FILE_FLAG_WRITE)) {
        /* Format into write buffer at current offset */
        int remaining = (int)(stream->size - stream->offset);
        if (remaining > 1) {
            int n = vsnprintf((char *)stream->data + stream->offset, remaining, format, args);
            if (n > 0) {
                stream->offset += (n < remaining) ? n : remaining - 1;
            }
        }
        return 0;
    }

    /* Fallback: format and print to terminal */
    char buf[256];
    int result = vsnprintf(buf, sizeof(buf), format, args);
    extern void term_puts(const char *s);
    term_puts(buf);
    return result;
}

int fprintf(FILE *stream, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = vfprintf(stream, format, args);
    va_end(args);
    return result;
}

int printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = vfprintf(stdout, format, args);
    va_end(args);
    return result;
}

void setbuf(FILE *stream, char *buf) {
    (void)stream;
    (void)buf;
    /* No buffering implemented */
}

int sprintf(char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsprintf(str, format, args);
    va_end(args);
    return result;
}

/* Core formatted print: va_list version with size limit */
int vsnprintf(char *str, size_t size, const char *format, va_list args) {
    char *out = str;
    char *end = str + size - 1;  /* Leave room for null */
    const char *p = format;

    if (size == 0) return 0;

    while (*p && out < end) {
        if (*p == '%') {
            p++;
            /* Parse flags */
            int left_align = 0;
            char pad_char = ' ';
            if (*p == '-') { left_align = 1; p++; }
            if (*p == '0') { pad_char = '0'; p++; }

            /* Parse width */
            int width = 0;
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }

            /* Parse precision */
            int precision = -1;
            if (*p == '.') {
                p++;
                precision = 0;
                while (*p >= '0' && *p <= '9') {
                    precision = precision * 10 + (*p - '0');
                    p++;
                }
            }

            /* Parse length modifier */
            int is_long = 0;
            if (*p == 'l') { is_long = 1; p++; }

            switch (*p) {
                case 'd':
                case 'i': {
                    long val = is_long ? va_arg(args, long) : (long)va_arg(args, int);
                    char buf[20];
                    int i = 0, neg = 0;
                    if (val < 0) { neg = 1; val = -val; }
                    do { buf[i++] = '0' + (val % 10); val /= 10; } while (val > 0);
                    /* Precision: pad with leading zeros to reach minimum digits */
                    while (i < precision) buf[i++] = '0';
                    int len = i + neg;
                    if (!left_align) while (len < width && out < end) { *out++ = pad_char; len++; }
                    if (neg && out < end) *out++ = '-';
                    while (i > 0 && out < end) *out++ = buf[--i];
                    if (left_align) while (len < width && out < end) { *out++ = ' '; len++; }
                    break;
                }
                case 'u': {
                    unsigned long val = is_long ? va_arg(args, unsigned long) : (unsigned long)va_arg(args, unsigned int);
                    char buf[20]; int i = 0;
                    do { buf[i++] = '0' + (val % 10); val /= 10; } while (val > 0);
                    while (i < precision) buf[i++] = '0';
                    int len = i;
                    if (!left_align) while (len < width && out < end) { *out++ = pad_char; len++; }
                    while (i > 0 && out < end) *out++ = buf[--i];
                    if (left_align) while (len < width && out < end) { *out++ = ' '; len++; }
                    break;
                }
                case 'x':
                case 'X': {
                    unsigned long val = is_long ? va_arg(args, unsigned long) : (unsigned long)va_arg(args, unsigned int);
                    const char *hex = (*p == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
                    char buf[16]; int i = 0;
                    do { buf[i++] = hex[val & 0xF]; val >>= 4; } while (val > 0);
                    while (i < precision) buf[i++] = '0';
                    int len = i;
                    if (!left_align) while (len < width && out < end) { *out++ = pad_char; len++; }
                    while (i > 0 && out < end) *out++ = buf[--i];
                    if (left_align) while (len < width && out < end) { *out++ = ' '; len++; }
                    break;
                }
                case 'f': {
                    double val = va_arg(args, double);
                    int prec = (precision >= 0) ? precision : 6;
                    if (val < 0) { if (out < end) *out++ = '-'; val = -val; }
                    int ipart = (int)val;
                    float fpart = (float)(val - ipart);
                    /* Integer part */
                    char buf[20]; int i = 0;
                    do { buf[i++] = '0' + (ipart % 10); ipart /= 10; } while (ipart > 0);
                    int numlen = i + (prec > 0 ? 1 + prec : 0);
                    if (!left_align) while (numlen < width && out < end) { *out++ = pad_char; numlen++; }
                    while (i > 0 && out < end) *out++ = buf[--i];
                    /* Decimal part */
                    if (prec > 0) {
                        if (out < end) *out++ = '.';
                        for (i = 0; i < prec && out < end; i++) {
                            fpart *= 10.0f;
                            int d = (int)fpart;
                            *out++ = '0' + d;
                            fpart -= d;
                        }
                    }
                    break;
                }
                case 's': {
                    const char *s = va_arg(args, const char *);
                    if (!s) s = "(null)";
                    int len = 0; const char *t = s;
                    while (*t) { t++; len++; }
                    if (!left_align) while (len < width && out < end) { *out++ = ' '; width--; }
                    while (*s && out < end) *out++ = *s++;
                    if (left_align) while (len < width && out < end) { *out++ = ' '; len++; }
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    if (out < end) *out++ = c;
                    break;
                }
                case 'p': {
                    unsigned long val = (unsigned long)va_arg(args, void *);
                    if (out + 1 < end) { *out++ = '0'; *out++ = 'x'; }
                    char buf[16]; int i = 0;
                    do { buf[i++] = "0123456789abcdef"[val & 0xF]; val >>= 4; } while (val > 0);
                    while (i > 0 && out < end) *out++ = buf[--i];
                    break;
                }
                case '%':
                    if (out < end) *out++ = '%';
                    break;
                default:
                    if (out < end) *out++ = '%';
                    if (out < end) *out++ = *p;
                    break;
            }
        } else {
            *out++ = *p;
        }
        p++;
    }

    *out = '\0';
    return out - str;
}

int vsprintf(char *str, const char *format, va_list args) {
    return vsnprintf(str, 0x7FFFFFFF, format, args);
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(str, size, format, args);
    va_end(args);
    return result;
}

int sscanf(const char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);

    int count = 0;
    const char *s = str;
    const char *f = format;

    while (*f && *s) {
        if (*f == '%') {
            f++;
            switch (*f) {
                case 'd':
                case 'i': {
                    int *ptr = va_arg(args, int *);
                    int sign = 1;
                    int val = 0;

                    /* Skip whitespace */
                    while (isspace(*s)) s++;

                    if (*s == '-') {
                        sign = -1;
                        s++;
                    } else if (*s == '+') {
                        s++;
                    }

                    if (!isdigit(*s)) break;

                    while (isdigit(*s)) {
                        val = val * 10 + (*s - '0');
                        s++;
                    }

                    *ptr = val * sign;
                    count++;
                    break;
                }
                case 'f': {
                    float *ptr = va_arg(args, float *);

                    /* Skip whitespace */
                    while (isspace(*s)) s++;

                    /* Parse float using atof logic */
                    const char *start = s;
                    int sign = 1;
                    float val = 0.0f;
                    float frac = 0.0f;
                    float div = 1.0f;
                    int in_frac = 0;

                    if (*s == '-') { sign = -1; s++; }
                    else if (*s == '+') { s++; }

                    while (*s && (isdigit(*s) || *s == '.')) {
                        if (*s == '.') {
                            if (in_frac) break;
                            in_frac = 1;
                        } else if (in_frac) {
                            div *= 10.0f;
                            frac += (*s - '0') / div;
                        } else {
                            val = val * 10.0f + (*s - '0');
                        }
                        s++;
                    }

                    if (s == start) break;

                    *ptr = (val + frac) * sign;
                    count++;
                    break;
                }
                case 'x':
                case 'X': {
                    unsigned int *ptr = va_arg(args, unsigned int *);
                    unsigned int val = 0;

                    while (isspace(*s)) s++;

                    /* Skip 0x prefix if present */
                    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                        s += 2;
                    }

                    while (*s) {
                        if (isdigit(*s)) {
                            val = val * 16 + (*s - '0');
                        } else if (*s >= 'a' && *s <= 'f') {
                            val = val * 16 + (*s - 'a' + 10);
                        } else if (*s >= 'A' && *s <= 'F') {
                            val = val * 16 + (*s - 'A' + 10);
                        } else {
                            break;
                        }
                        s++;
                    }

                    *ptr = val;
                    count++;
                    break;
                }
                default:
                    break;
            }
            f++;
        } else if (isspace(*f)) {
            /* Skip whitespace in both format and input */
            while (isspace(*f)) f++;
            while (isspace(*s)) s++;
        } else {
            /* Literal match */
            if (*f != *s) break;
            f++;
            s++;
        }
    }

    va_end(args);
    return count;
}

/* ============================================
 * POSIX-style file operations
 * ============================================ */

/* Map slot IDs to file descriptors (3+ to avoid stdin/stdout/stderr) */
#define FD_TO_SLOT(fd) ((fd) - 3)
#define SLOT_TO_FD(slot) ((slot) + 3)

static uint32_t fd_offset[16] = {0};
static uint32_t fd_size[16] = {0};
static int fd_used[16] = {0};
static void *fd_data[16] = {0};  /* Non-NULL = preloaded in SDRAM */

/* Save file descriptor tracking.
 * Separate from WAD fds to avoid array conflicts.
 * FD range: 100-106 for saves, 107 for config. */
#define SAV_FD_BASE     100
#define SAV_FD_CFG      (SAV_FD_BASE + SAV_SLOT_COUNT)

static uint32_t sav_fd_offset = 0;
static uint32_t sav_fd_size = 0;
static int      sav_fd_used = 0;
static int      sav_fd_writing = 0;
static int      sav_fd_sub_idx = -1;   /* 0-5 sub-save index */

int open(const char *pathname, int flags, ...) {
    /* Save file (.dsg) handling */
    if (is_save_file(pathname)) {
        int sub_idx = sav_slot_from_name(pathname);
        if (sub_idx < 0) return -1;

        sav_clear_incompatible();  /* Erase stale data from other games */

        if (sav_fd_used) return -1;  /* Only one save fd at a time */

        if (flags & O_WRONLY) {
            /* Write mode: prepare sav_buf for writing */
            memset(sav_buf, 0, SAV_BUF_SIZE);
            sav_fd_offset = 0;
            sav_fd_size = SAV_BUF_SIZE;
            sav_fd_writing = 1;
            sav_fd_sub_idx = sub_idx;
            sav_write_sub_idx = sub_idx;
            sav_fd_used = 1;
            return SAV_FD_BASE + sub_idx;
        } else {
            /* Read mode: load from SD card via deferload */
            uint32_t saved_size = sav_read_from_slot(sub_idx);
            if (saved_size == 0) return -1;  /* Empty slot */
            sav_fd_offset = 0;
            sav_fd_size = saved_size;
            sav_fd_writing = 0;
            sav_fd_sub_idx = sub_idx;
            sav_fd_used = 1;
            return SAV_FD_BASE + sub_idx;
        }
    }

    /* WAD file handling */
    int slot_id = filename_to_slot(pathname);

    if (str_ends_with_ci(pathname, ".wad")) {
        if (fd_used[slot_id]) return -1;
        uint32_t size = wad_get_size(slot_id);
        if (size == 0) return -1;
        fd_size[slot_id] = size;
        fd_offset[slot_id] = 0;
        fd_data[slot_id] = NULL;
        fd_used[slot_id] = 1;
        wad_open_count++;
        return SLOT_TO_FD(slot_id);
    }

    if (slot_id < 0 || slot_id >= 16) return -1;
    if (fd_used[slot_id]) return -1;

    if (dataslot_get_size(slot_id, &fd_size[slot_id]) != 0)
        return -1;

    fd_offset[slot_id] = 0;
    fd_used[slot_id] = 1;
    return SLOT_TO_FD(slot_id);
}

int close(int fd) {
    /* Save file fd */
    if (fd >= SAV_FD_BASE && fd <= SAV_FD_CFG && sav_fd_used) {
        if (sav_fd_writing && sav_fd_offset > 0 && sav_fd_sub_idx >= 0) {
            sav_persist(sav_fd_sub_idx, sav_fd_offset);
            sav_write_sub_idx = -1;
        }
        sav_fd_used = 0;
        sav_fd_writing = 0;
        sav_fd_sub_idx = -1;
        return 0;
    }

    /* WAD/other fd */
    int slot_id = FD_TO_SLOT(fd);
    if (slot_id < 0 || slot_id >= 16 || !fd_used[slot_id]) {
        return -1;
    }

    fd_used[slot_id] = 0;
    fd_data[slot_id] = NULL;
    return 0;
}

ssize_t read(int fd, void *buf, size_t count) {
    /* Save file read — from sav_buf */
    if (fd >= SAV_FD_BASE && fd <= SAV_FD_CFG && sav_fd_used && !sav_fd_writing) {
        uint32_t available = sav_fd_size - sav_fd_offset;
        if (count > available) count = available;
        if (count == 0) return 0;
        memcpy(buf, sav_buf + sav_fd_offset, count);
        sav_fd_offset += count;
        return count;
    }

    /* WAD/other fd */
    int slot_id = FD_TO_SLOT(fd);
    if (slot_id < 0 || slot_id >= 16 || !fd_used[slot_id]) {
        return -1;
    }

    uint32_t available = fd_size[slot_id] - fd_offset[slot_id];
    if (count > available) {
        count = available;
    }

    if (count == 0) {
        return 0;
    }

    /* If data is preloaded in SDRAM, copy directly */
    if (fd_data[slot_id] != NULL) {
        memcpy(buf, (uint8_t *)fd_data[slot_id] + fd_offset[slot_id], count);
        fd_offset[slot_id] += count;
        return count;
    }

    /* DMA to bounce buffer, copy via uncacheable alias */
    uint8_t *dest = (uint8_t *)buf;
    size_t remaining = count;
    uint32_t off = fd_offset[slot_id];
    while (remaining > 0) {
        size_t chunk = remaining > DMA_CHUNK_SIZE ? DMA_CHUNK_SIZE : remaining;
        if (dataslot_read(slot_id, off, (void *)DMA_BUFFER, chunk) != 0) {
            return -1;
        }
        memcpy(dest, SDRAM_UNCACHED(DMA_BUFFER), chunk);
        dest += chunk;
        off += chunk;
        remaining -= chunk;
    }

    fd_offset[slot_id] += count;
    return count;
}

off_t lseek(int fd, off_t offset, int whence) {
    /* Save file seek */
    if (fd >= SAV_FD_BASE && fd <= SAV_FD_CFG && sav_fd_used) {
        off_t new_offset;
        switch (whence) {
            case SEEK_SET: new_offset = offset; break;
            case SEEK_CUR: new_offset = sav_fd_offset + offset; break;
            case SEEK_END: new_offset = sav_fd_size + offset; break;
            default: return -1;
        }
        if (new_offset < 0) return -1;
        sav_fd_offset = new_offset;
        return new_offset;
    }

    /* WAD/other fd */
    int slot_id = FD_TO_SLOT(fd);
    if (slot_id < 0 || slot_id >= 16 || !fd_used[slot_id]) {
        return -1;
    }

    off_t new_offset;
    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = fd_offset[slot_id] + offset;
            break;
        case SEEK_END:
            new_offset = fd_size[slot_id] + offset;
            break;
        default:
            return -1;
    }

    if (new_offset < 0) {
        return -1;
    }

    fd_offset[slot_id] = new_offset;
    return new_offset;
}

int fgetc(FILE *stream) {
    unsigned char c;
    if (fread(&c, 1, 1, stream) == 1)
        return c;
    return EOF;
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int unlink(const char *pathname) {
    (void)pathname;
    return -1;  /* Not supported */
}

ssize_t write(int fd, const void *buf, size_t count) {
    /* Save file write — to sav_buf */
    if (fd >= SAV_FD_BASE && fd <= SAV_FD_CFG && sav_fd_used && sav_fd_writing) {
        uint32_t remaining = sav_fd_size - sav_fd_offset;
        if (count > remaining) count = remaining;
        if (count > 0) {
            memcpy(sav_buf + sav_fd_offset, buf, count);
            sav_fd_offset += count;
        }
        return count;
    }

    /* Write to terminal for stdout/stderr */
    const char *p = (const char *)buf;
    size_t i;
    for (i = 0; i < count; i++) {
        extern void term_putchar(char c);
        term_putchar(p[i]);
    }
    return count;
}

int fscanf(FILE *stream, const char *format, ...) {
    /* Very limited fscanf - just enough for Doom's savegame loading */
    /* Read a line from the file and then sscanf it */
    char buf[256];
    int i = 0;
    int c;

    while (i < 255) {
        c = fgetc(stream);
        if (c == EOF || c == '\n')
            break;
        buf[i++] = (char)c;
    }
    buf[i] = '\0';

    va_list args;
    va_start(args, format);
    int result = 0;
    /* Basic: just pass to sscanf */
    /* We can't easily forward varargs to sscanf, so handle common patterns */
    /* Doom save files use simple %d and %f patterns */
    const char *s = buf;
    const char *f = format;

    while (*f && *s) {
        if (*f == '%') {
            f++;
            /* Skip optional width specifier (e.g., %79s) */
            int width = 0;
            while (isdigit(*f)) { width = width * 10 + (*f - '0'); f++; }

            if (*f == 'd' || *f == 'i') {
                int *ptr = va_arg(args, int *);
                int val = 0, sign = 1;
                while (isspace(*s)) s++;
                if (*s == '-') { sign = -1; s++; }
                while (isdigit(*s)) { val = val * 10 + (*s - '0'); s++; }
                *ptr = val * sign;
                result++;
                f++;
            } else if (*f == 'f') {
                float *ptr = va_arg(args, float *);
                *ptr = (float)atof(s);
                while (*s && !isspace(*s)) s++;
                result++;
                f++;
            } else if (*f == 's') {
                char *ptr = va_arg(args, char *);
                int maxlen = width > 0 ? width : 255;
                int n = 0;
                while (isspace(*s)) s++;
                while (*s && !isspace(*s) && n < maxlen) { *ptr++ = *s++; n++; }
                *ptr = '\0';
                result++;
                f++;
            } else if (*f == '[') {
                /* Scanset: %[^\n] = read until newline */
                f++;
                int negate = 0;
                if (*f == '^') { negate = 1; f++; }
                char stop_char = *f;
                f++;  /* skip the char (e.g., '\n') */
                if (*f == ']') f++;  /* skip closing ] */
                char *ptr = va_arg(args, char *);
                int maxlen = width > 0 ? width : 255;
                int n = 0;
                if (negate) {
                    while (*s && *s != stop_char && n < maxlen) { *ptr++ = *s++; n++; }
                } else {
                    while (*s && *s == stop_char && n < maxlen) { *ptr++ = *s++; n++; }
                }
                *ptr = '\0';
                if (n > 0) result++;
            } else {
                f++;
            }
        } else if (isspace(*f)) {
            while (isspace(*f)) f++;
            while (isspace(*s)) s++;
        } else {
            if (*f != *s) break;
            f++; s++;
        }
    }

    va_end(args);
    return result;
}

/* ============================================
 * mmap emulation
 * ============================================ */

/* Static buffer for mmap'd data - we allocate from SDRAM heap */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr;
    (void)prot;
    (void)flags;

    int slot_id = FD_TO_SLOT(fd);
    if (slot_id < 0 || slot_id >= 16 || !fd_used[slot_id]) {
        return MAP_FAILED;
    }

    /* Allocate memory for the mapped region */
    void *ptr = malloc(length);
    if (ptr == NULL) {
        return MAP_FAILED;
    }

    /* DMA to bounce buffer in chunks, copy via uncacheable alias to
     * avoid stale D-cache lines at the malloc'd destination. */
    uint8_t *dest = (uint8_t *)ptr;
    size_t remaining = length;
    uint32_t slot_off = (uint32_t)offset;
    while (remaining > 0) {
        size_t chunk = remaining > DMA_CHUNK_SIZE ? DMA_CHUNK_SIZE : remaining;
        if (dataslot_read(slot_id, slot_off, (void *)DMA_BUFFER, chunk) != 0) {
            free(ptr);
            return MAP_FAILED;
        }
        memcpy(dest, SDRAM_UNCACHED(DMA_BUFFER), chunk);
        dest += chunk;
        slot_off += chunk;
        remaining -= chunk;
    }

    return ptr;
}

int munmap(void *addr, size_t length) {
    (void)length;
    if (addr != NULL && addr != MAP_FAILED) {
        free(addr);
    }
    return 0;
}

/* Stubs for stat/fstat - minimal support for Doom */
struct stat;
int stat(const char *path, struct stat *buf) {
    (void)path;
    (void)buf;
    errno = ENOENT;
    return -1;
}

int fstat(int fd, struct stat *buf) {
    (void)fd;
    (void)buf;
    errno = ENOENT;
    return -1;
}
