/*
 * Doom sound driver for Analogue Pocket (PocketDoom)
 *
 * Implements the low-level i_sound interface using the FPGA's audio hardware.
 * Mixer ported from the Linux reference (doom/linux-x11/i_sound.c).
 * Mixes at 11,025 Hz internally, writes directly to the FPGA's BRAM ring
 * buffer via MMIO.  The FPGA hardware upsampler handles 11→48 kHz conversion.
 * Music is handled by doom_music.c (OPL2 synthesizer).
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

#define AUDIO_SAMPLE    (*(volatile uint32_t *)0x4C000000)  /* Write: {L[15:0], R[15:0]} */
#define AUDIO_STATUS    (*(volatile uint32_t *)0x4C000004)  /* Read: [12]=full, [11:0]=level */

#define BUF_LEVEL(s)    ((s) & 0x3FF)
#define BUF_DEPTH       512
#define BUF_TARGET      448   /* Target ~87% full — 40ms headroom for heavy frames */

/* ============================================
 * Mixer constants
 * ============================================ */

#define SAMPLECOUNT     512
#define NUM_CHANNELS    8
#define SAMPLERATE      11025

/* ============================================
 * Mixer state (ported from linux-x11/i_sound.c)
 * ============================================ */

/* Actual lengths of all sound effects */
static int lengths[NUMSFX];

/* Channel data pointers, start and end */
static unsigned char *channels[NUM_CHANNELS];
static unsigned char *channelsend[NUM_CHANNELS];

/* Channel step (pitch) and fractional remainder */
static unsigned int channelstep[NUM_CHANNELS];
static unsigned int channelstepremainder[NUM_CHANNELS];

/* Gametic when channel started (for finding oldest) */
static int channelstart[NUM_CHANNELS];

/* Handle assigned to each channel */
static int channelhandles[NUM_CHANNELS];

/* SFX id playing on each channel (for singularity check) */
static int channelids[NUM_CHANNELS];

/* Pitch-to-step lookup table */
static int steptable[256];

/* Volume lookup: [volume 0-127][sample 0-255] → signed 16-bit */
static int vol_lookup[128 * 256];

/* Per-channel volume lookup pointers (into vol_lookup) */
static int *channelleftvol_lookup[NUM_CHANNELS];
static int *channelrightvol_lookup[NUM_CHANNELS];

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

    /* Fall back to pistol if lump not found */
    if (W_CheckNumForName(name) == -1)
        sfxlump = W_GetNumForName("dspistol");
    else
        sfxlump = W_GetNumForName(name);

    size = W_LumpLength(sfxlump);
    sfx = (unsigned char *)W_CacheLumpNum(sfxlump, PU_STATIC);

    /* Pad to SAMPLECOUNT boundary */
    paddedsize = ((size - 8 + (SAMPLECOUNT - 1)) / SAMPLECOUNT) * SAMPLECOUNT;

    paddedsfx = (unsigned char *)Z_Malloc(paddedsize + 8, PU_STATIC, 0);
    memcpy(paddedsfx, sfx, size);
    for (i = size; i < paddedsize + 8; i++)
        paddedsfx[i] = 128;  /* Silence = 128 for unsigned 8-bit */

    Z_Free(sfx);

    *len = paddedsize;

    /* Skip 8-byte header */
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

    /* Chainsaw/singularity: only one instance at a time */
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

    /* Find oldest or first free channel */
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

    /* Set channel data pointers */
    channels[slot] = (unsigned char *)S_sfx[sfxid].data;
    channelsend[slot] = channels[slot] + lengths[sfxid];

    /* Assign handle */
    if (!handlenums)
        handlenums = 100;
    channelhandles[slot] = rc = handlenums++;

    /* Set step (pitch) */
    channelstep[slot] = step;
    channelstepremainder[slot] = 0;
    channelstart[slot] = gametic;

    /* Calculate stereo separation volumes */
    seperation += 1;

    leftvol = volume - ((volume * seperation * seperation) >> 16);
    seperation = seperation - 257;
    rightvol = volume - ((volume * seperation * seperation) >> 16);

    /* Clamp volumes */
    if (rightvol < 0) rightvol = 0;
    if (rightvol > 127) rightvol = 127;
    if (leftvol < 0) leftvol = 0;
    if (leftvol > 127) leftvol = 127;

    /* Point to volume lookup table slice */
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

    /* Pitch step table: 2^(i/64) * 65536
     * Built iteratively: multiply by 2^(1/64) ≈ 66250/65536 per step.
     * Key points are exact: 0→65536, ±64→2x/0.5x, ±128→4x/0.25x. */
    steptablemid[0] = 65536;
    for (i = 1; i < 128; i++)
        steptablemid[i] = (int)(((int64_t)steptablemid[i-1] * 66250) >> 16);
    for (i = 1; i <= 128; i++)
        steptablemid[-i] = (int)(((int64_t)steptablemid[-i+1] * 64830) >> 16);
    /* Fix exact octave boundaries to avoid accumulated rounding */
    steptablemid[64] = 131072;
    steptablemid[-64] = 32768;

    /* Volume lookup table: converts unsigned 8-bit sample + volume
     * to signed 16-bit output. Sample 128 = silence.
     *
     * Original Doom formula: (i * (j-128) * 256) / 127
     * That gives ±32,512 per channel — fine for DOS 8-bit DAC but
     * 8 channels sum to 260K, far exceeding 16-bit range (±32,767).
     * The FPGA also adds OPL music on top.
     *
     * Scale: max per channel = 127*108 = 13716.
     * Soft clamp in mixer and hardware clamp in audio_output.v
     * handle peaks that exceed 16-bit range. */
    for (i = 0; i < 128; i++)
        for (j = 0; j < 256; j++)
            vol_lookup[i * 256 + j] = (i * (j - 128) * 256) / 127;
}

/* ============================================
 * SFX API
 * ============================================ */

void I_SetSfxVolume(int volume)
{
    snd_SfxVolume = volume;
}

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    (void)priority;
    id = addsfx(id, vol, steptable[pitch], sep);
    return id;
}

void I_StopSound(int handle)
{
    (void)handle;
}

int I_SoundIsPlaying(int handle)
{
    /* Check if any mixer channel still holds this handle */
    int i;
    for (i = 0; i < NUM_CHANNELS; i++)
        if (channels[i] && channelhandles[i] == handle)
            return 1;
    return 0;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    (void)handle;
    (void)vol;
    (void)sep;
    (void)pitch;
}

/* ============================================
 * I_UpdateSound - Mix all active channels into HW ring buffer
 * ============================================ */

/* Set to 1 to play a 440 Hz test tone through the full pipeline.
 * If the tone sounds clean, the FPGA path works and the mix loop is suspect.
 * If distorted, the FPGA ring buffer or upsampler has a bug. */
#define AUDIO_TEST_TONE 0

PD_FASTTEXT void I_UpdateSound(void)
{
    int buf_lvl = BUF_LEVEL(AUDIO_STATUS);
    int deficit = BUF_TARGET - buf_lvl;
    if (deficit <= 0)
        return;

    int mix_count = deficit;
    if (mix_count > SAMPLECOUNT)
        mix_count = SAMPLECOUNT;
    if (mix_count < 64)
        mix_count = 64;

#if AUDIO_TEST_TONE
    /* 440 Hz square wave at 11025 Hz: period ≈ 25 samples */
    {
        static int tone_phase = 0;
        int i;
        for (i = 0; i < mix_count; i++) {
            int16_t v = (tone_phase < 13) ? (int16_t)4000 : (int16_t)-4000;
            AUDIO_SAMPLE = ((uint32_t)(uint16_t)v << 16) | (uint16_t)v;
            if (++tone_phase >= 25) tone_phase = 0;
        }
        return;
    }
#endif

    register unsigned int sample;
    register int dl;
    register int dr;

    int i;
    int chan;

    for (i = 0; i < mix_count; i++)
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

        if (dl > 0x7fff)
            dl = 0x7fff;
        else if (dl < -0x8000)
            dl = -0x8000;

        if (dr > 0x7fff)
            dr = 0x7fff;
        else if (dr < -0x8000)
            dr = -0x8000;

        /* {R[15:0], L[15:0]} — I2S sends upper half first = right channel */
        AUDIO_SAMPLE = ((uint32_t)(uint16_t)dr << 16) | (uint16_t)dl;
    }
}

/* ============================================
 * I_SubmitSound - No-op (HW upsampler handles submission)
 * ============================================ */

void I_SubmitSound(void)
{
}

/* ============================================
 * I_InitSound - Pre-cache sounds, init mixer
 * ============================================ */

void I_InitSound(void)
{
    int i;

    printf("I_InitSound: initializing sound\n");

    /* Pre-load all sound effects from WAD */
    for (i = 1; i < NUMSFX; i++)
    {
        if (!S_sfx[i].link)
        {
            S_sfx[i].data = getsfx(S_sfx[i].name, &lengths[i]);
        }
        else
        {
            S_sfx[i].data = S_sfx[i].link->data;
            lengths[i] = lengths[S_sfx[i].link - S_sfx];
        }
    }

    printf("I_InitSound: pre-cached all sound data\n");

    /* Clear channel state */
    for (i = 0; i < NUM_CHANNELS; i++)
        channels[i] = 0;

    printf("I_InitSound: sound module ready\n");
}

void I_ShutdownSound(void)
{
}

/* Music API is now in doom_music.c */
