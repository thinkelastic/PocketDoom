/*
 * Doom sound driver for Analogue Pocket (PocketDoom)
 *
 * Implements the low-level i_sound interface using the FPGA's audio hardware.
 * Mixer ported from the Linux reference (doom/linux-x11/i_sound.c).
 *
 * Pull-mode DMA audio pipeline:
 *   CPU mixes 11,025 Hz stereo into a circular SDRAM buffer (uncached alias).
 *   The FPGA DMA watches the ring-buffer level and autonomously pulls samples
 *   from SDRAM into the HW upsampler.  An IRQ fires when the DMA is running
 *   low on data so the CPU can mix more.
 *
 * Music is handled by doom_music.c (OPL3 synthesizer).
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "i_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"
#include "m_argv.h"

/* From doom_music.c — advance MUS parser based on real elapsed time */
extern void OPL_AdvanceMusic(void);

/* ============================================
 * Hardware audio registers
 * ============================================ */

/* Legacy MMIO (unused in DMA mode, kept for reference) */
#define AUDIO_SAMPLE    (*(volatile uint32_t *)0x4C000000)
#define AUDIO_STATUS    (*(volatile uint32_t *)0x4C000004)

/* Audio DMA registers (0x4F00_0000) — pull mode */
#define ADMA_CTRL       (*(volatile uint32_t *)0x4F000000)
#define ADMA_STATUS     (*(volatile uint32_t *)0x4F000004)
#define ADMA_BASE_ADDR  (*(volatile uint32_t *)0x4F000008)
#define ADMA_BUF_MASK   (*(volatile uint32_t *)0x4F00000C)
#define ADMA_WR_PTR     (*(volatile uint32_t *)0x4F000010)
#define ADMA_RD_PTR     (*(volatile uint32_t *)0x4F000014)
#define ADMA_THRESHOLD  (*(volatile uint32_t *)0x4F000018)

#define ADMA_CTRL_ENABLE (1u << 0)
#define ADMA_CTRL_IRQ_EN (1u << 1)
#define ADMA_STATUS_IRQ  (1u << 1)

/* ============================================
 * DMA circular buffer in SDRAM
 * ============================================ */

/* Must be power of 2.  2048 samples = ~186 ms at 11,025 Hz. */
#define ADMA_RING_SIZE   2048
#define ADMA_RING_MASK   (ADMA_RING_SIZE - 1)
#define ADMA_BATCH       128          /* Samples per mix call (small = low latency) */
#define ADMA_IRQ_THRESH  384          /* Refill when available drops below this (~35 ms) */

/* Uncached SDRAM alias: 0x10xxxxxx (cached) → 0x50xxxxxx (uncached IO bus) */
#define SDRAM_UNCACHED_OFFSET  0x40000000u
#define UNCACHED_PTR(p) ((uint32_t *)((uintptr_t)(p) + SDRAM_UNCACHED_OFFSET))

/* Circular buffer in SDRAM (.bss → linker places in SDRAM). */
static uint32_t adma_ring[ADMA_RING_SIZE] __attribute__((aligned(8192)));

/* CPU-side write position (mirrors ADMA_WR_PTR, avoids MMIO read). */
static uint32_t adma_wr_pos;

/* ISR → main-loop flag.  In BRAM BSS (zeroed by bootloader). */
static volatile int adma_need_fill __attribute__((section(".bss.boot")));

/* ============================================
 * Mixer constants
 * ============================================ */

#define SAMPLECOUNT     512
#define NUM_CHANNELS    8
#define SAMPLERATE      11025

/* ============================================
 * Mixer state (ported from linux-x11/i_sound.c)
 * ============================================ */

static int lengths[NUMSFX];
static unsigned char *channels[NUM_CHANNELS];
static unsigned char *channelsend[NUM_CHANNELS];
static unsigned int channelstep[NUM_CHANNELS];
static unsigned int channelstepremainder[NUM_CHANNELS];
static int channelstart[NUM_CHANNELS];
static int channelhandles[NUM_CHANNELS];
static int channelids[NUM_CHANNELS];
static int steptable[256];
static int vol_lookup[128 * 256];
static int *channelleftvol_lookup[NUM_CHANNELS];
static int *channelrightvol_lookup[NUM_CHANNELS];

/* ============================================
 * Audio DMA ISR — called from trap handler
 * ============================================ */

void audio_dma_isr(void)
{
    ADMA_STATUS = ADMA_STATUS_IRQ;   /* W1C: clear irq_pending */
    adma_need_fill = 1;
}

/* ============================================
 * getsfx - Load sound from WAD lump
 * ============================================ */

static void *
getsfx(char *sfxname, int *len)
{
    unsigned char *sfx;
    unsigned char *paddedsfx;
    int i;
    int size;
    int paddedsize;
    char name[20];
    int sfxlump;

    sprintf(name, "ds%s", sfxname);

    if (W_CheckNumForName(name) == -1)
        sfxlump = W_GetNumForName("dspistol");
    else
        sfxlump = W_GetNumForName(name);

    size = W_LumpLength(sfxlump);
    sfx = (unsigned char *)W_CacheLumpNum(sfxlump, PU_STATIC);

    paddedsize = ((size - 8 + (SAMPLECOUNT - 1)) / SAMPLECOUNT) * SAMPLECOUNT;

    paddedsfx = (unsigned char *)Z_Malloc(paddedsize + 8, PU_STATIC, 0);
    memcpy(paddedsfx, sfx, size);
    for (i = size; i < paddedsize + 8; i++)
        paddedsfx[i] = 128;

    Z_Free(sfx);

    *len = paddedsize;
    return (void *)(paddedsfx + 8);
}

/* ============================================
 * addsfx - Add sound to a mixing channel
 * ============================================ */

static int
addsfx(int sfxid, int volume, int step, int seperation)
{
    static unsigned short handlenums = 0;
    int i;
    int rc = -1;
    int oldest = gametic;
    int oldestnum = 0;
    int slot;
    int rightvol;
    int leftvol;

    if (sfxid == sfx_sawup
        || sfxid == sfx_sawidl
        || sfxid == sfx_sawful
        || sfxid == sfx_sawhit
        || sfxid == sfx_stnmov
        || sfxid == sfx_pistol)
    {
        for (i = 0; i < NUM_CHANNELS; i++)
        {
            if (channels[i] && channelids[i] == sfxid)
            {
                channels[i] = 0;
                break;
            }
        }
    }

    for (i = 0; (i < NUM_CHANNELS) && (channels[i]); i++)
    {
        if (channelstart[i] < oldest)
        {
            oldestnum = i;
            oldest = channelstart[i];
        }
    }

    if (i == NUM_CHANNELS)
        slot = oldestnum;
    else
        slot = i;

    channels[slot] = (unsigned char *)S_sfx[sfxid].data;
    channelsend[slot] = channels[slot] + lengths[sfxid];

    if (!handlenums)
        handlenums = 100;
    channelhandles[slot] = rc = handlenums++;

    channelstep[slot] = step;
    channelstepremainder[slot] = 0;
    channelstart[slot] = gametic;

    seperation += 1;

    leftvol = volume - ((volume * seperation * seperation) >> 16);
    seperation = seperation - 257;
    rightvol = volume - ((volume * seperation * seperation) >> 16);

    if (rightvol < 0) rightvol = 0;
    if (rightvol > 127) rightvol = 127;
    if (leftvol < 0) leftvol = 0;
    if (leftvol > 127) leftvol = 127;

    channelleftvol_lookup[slot] = &vol_lookup[leftvol * 256];
    channelrightvol_lookup[slot] = &vol_lookup[rightvol * 256];

    channelids[slot] = sfxid;

    return rc;
}

/* ============================================
 * I_SetChannels - Build lookup tables
 * ============================================ */

void I_SetChannels(void)
{
    int i;
    int j;
    int *steptablemid = steptable + 128;

    steptablemid[0] = 65536;
    for (i = 1; i < 128; i++)
        steptablemid[i] = (int)(((int64_t)steptablemid[i-1] * 66250) >> 16);
    for (i = 1; i <= 128; i++)
        steptablemid[-i] = (int)(((int64_t)steptablemid[-i+1] * 64830) >> 16);
    steptablemid[64] = 131072;
    steptablemid[-64] = 32768;

    for (i = 0; i < 128; i++)
        for (j = 0; j < 256; j++)
            vol_lookup[i * 256 + j] = (i * (j - 128) * 256) / 127;
}

/* ============================================
 * SFX API
 * ============================================ */

void I_SetSfxVolume(int volume) { snd_SfxVolume = volume; }

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    (void)priority;
    return addsfx(id, vol, steptable[pitch], sep);
}

void I_StopSound(int handle) { (void)handle; }

int I_SoundIsPlaying(int handle)
{
    int i;
    for (i = 0; i < NUM_CHANNELS; i++)
        if (channels[i] && channelhandles[i] == handle)
            return 1;
    return 0;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    (void)handle; (void)vol; (void)sep; (void)pitch;
}

/* ============================================
 * I_UpdateSound — Mix a batch into the DMA circular buffer
 * ============================================ */

PD_FASTTEXT void I_UpdateSound(void)
{
    if (!adma_need_fill)
        return;
    adma_need_fill = 0;

    uint32_t *buf = UNCACHED_PTR(adma_ring);

    /* Fill multiple batches until available is well above the threshold.
     * This ensures the DMA's irq_armed re-arms (needs available > threshold). */
    int batches = 0;
    while (batches < 8) {  /* Safety limit */
        uint32_t rd = ADMA_RD_PTR;
        uint32_t avail = adma_wr_pos - rd;
        if (avail > (uint32_t)(ADMA_IRQ_THRESH + ADMA_BATCH))
            break;

        uint32_t wr = adma_wr_pos;
        register unsigned int sample;
        register int dl;
        register int dr;
        int i, chan;

        for (i = 0; i < ADMA_BATCH; i++)
        {
            dl = 0;
            dr = 0;

            for (chan = 0; chan < NUM_CHANNELS; chan++)
            {
                if (channels[chan])
                {
                    sample = *channels[chan];
                    dl += channelleftvol_lookup[chan][sample];
                    dr += channelrightvol_lookup[chan][sample];

                    channelstepremainder[chan] += channelstep[chan];
                    channels[chan] += channelstepremainder[chan] >> 16;
                    channelstepremainder[chan] &= 0xFFFF;

                    if (channels[chan] >= channelsend[chan])
                        channels[chan] = 0;
                }
            }

            if (dl > 0x7fff)      dl = 0x7fff;
            else if (dl < -0x8000) dl = -0x8000;
            if (dr > 0x7fff)      dr = 0x7fff;
            else if (dr < -0x8000) dr = -0x8000;

            buf[(wr + i) & ADMA_RING_MASK] = ((uint32_t)(uint16_t)dr << 16) | (uint16_t)dl;
        }

        adma_wr_pos = wr + ADMA_BATCH;
        ADMA_WR_PTR = adma_wr_pos;
        batches++;
    }
}

/* ============================================
 * I_SubmitSound - No-op (DMA handles submission)
 * ============================================ */

void I_SubmitSound(void) { }

/* ============================================
 * I_InitSound - Pre-cache sounds, init mixer, start DMA
 * ============================================ */

void I_InitSound(void)
{
    int i;

    printf("I_InitSound: initializing sound\n");

    for (i = 1; i < NUMSFX; i++)
    {
        if (!S_sfx[i].link)
            S_sfx[i].data = getsfx(S_sfx[i].name, &lengths[i]);
        else
        {
            S_sfx[i].data = S_sfx[i].link->data;
            lengths[i] = lengths[S_sfx[i].link - S_sfx];
        }
    }

    printf("I_InitSound: pre-cached all sound data\n");

    for (i = 0; i < NUM_CHANNELS; i++)
        channels[i] = 0;

    /* Configure DMA — no pre-fill, start empty.
     * The IRQ fires immediately (available=0 < threshold), so the first
     * I_UpdateSound call mixes real audio with no silence queued ahead. */
    ADMA_CTRL = 0;
    ADMA_BASE_ADDR = (uint32_t)(uintptr_t)UNCACHED_PTR(adma_ring);
    ADMA_BUF_MASK  = ADMA_RING_MASK;
    ADMA_THRESHOLD = ADMA_IRQ_THRESH;
    ADMA_WR_PTR    = 0;
    adma_wr_pos    = 0;
    adma_need_fill = 0;

    /* Enable machine external interrupt */
    __asm__ volatile ("csrs mie, %0" :: "r"(1u << 11));
    __asm__ volatile ("csrs mstatus, %0" :: "r"(1u << 3));

    /* Start DMA with interrupt */
    ADMA_CTRL = ADMA_CTRL_ENABLE | ADMA_CTRL_IRQ_EN;

    printf("I_InitSound: DMA pull-mode audio started\n");
    printf("I_InitSound: sound module ready\n");
}

void I_ShutdownSound(void)
{
    ADMA_CTRL = 0;
    __asm__ volatile ("csrc mie, %0" :: "r"(1u << 11));
}

/* Music API is now in doom_music.c */
