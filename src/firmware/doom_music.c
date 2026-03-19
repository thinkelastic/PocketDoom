/*
 * Doom music driver for Analogue Pocket (PocketDoom)
 *
 * MUS format parser driving Nuked OPL3 (bit-perfect YMF262 emulator).
 * Reads instrument patches from the WAD's GENMIDI lump.
 * Drives the FPGA OPL2 hardware; audio is mixed in audio_output.v.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "doomdef.h"
#include "doomstat.h"
#include "i_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

/* Hardware OPL2 MMIO registers */
#define OPL2_ADDR  (*(volatile uint32_t *)0x4E000000)
#define OPL2_DATA  (*(volatile uint32_t *)0x4E000004)

/* ============================================
 * Constants
 * ============================================ */

#define SAMPLERATE       11025
#define MUS_TICKRATE     140
#define SAMPLES_PER_TICK ((SAMPLERATE << 8) / MUS_TICKRATE)  /* 16.8 fixed-point */

#define NUM_OPL_VOICES   9
#define MUS_CHANNELS     16
#define PERCUSSION_CHAN   15
#define GENMIDI_NUM_INSTRS 175
#define PERCUSSION_BASE  128

/* ============================================
 * GENMIDI data structures (matches binary layout)
 * ============================================ */

typedef struct {
    uint8_t tremolo;    /* Reg 0x20: AM/VIB/EGT/KSR/MULT  */
    uint8_t attack;     /* Reg 0x60: Attack/Decay           */
    uint8_t sustain;    /* Reg 0x80: Sustain/Release        */
    uint8_t waveform;   /* Reg 0xE0: Waveform               */
    uint8_t scale;      /* Reg 0x40 KSL value (0-3)         */
    uint8_t level;      /* Reg 0x40 TL value  (0-63)        */
} __attribute__((packed)) genmidi_op_t;   /* 6 bytes */

typedef struct {
    genmidi_op_t modulator;             /*  6 bytes */
    uint8_t      feedback;              /*  1 byte: fb[3:1], conn[0] */
    genmidi_op_t carrier;               /*  6 bytes */
    uint8_t      unused;                /*  1 byte  */
    int16_t      base_note_offset;      /*  2 bytes, little-endian */
} __attribute__((packed)) genmidi_voice_t; /* 16 bytes */

typedef struct {
    uint16_t        flags;              /*  2 bytes */
    uint8_t         fine_tune;          /*  1 byte  */
    uint8_t         fixed_note;         /*  1 byte  */
    genmidi_voice_t voice[2];           /* 32 bytes */
} __attribute__((packed)) genmidi_instr_t; /* 36 bytes */

#define GENMIDI_FLAG_FIXED   (1 << 0)
#define GENMIDI_FLAG_DOUBLE  (1 << 2)

static genmidi_instr_t genmidi[GENMIDI_NUM_INSTRS];
static int genmidi_loaded;

/* ============================================
 * OPL register offset tables (9-channel OPL2)
 * ============================================ */

static const uint8_t op1_off[9] = {
    0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12
};
static const uint8_t op2_off[9] = {
    0x03, 0x04, 0x05, 0x0B, 0x0C, 0x0D, 0x13, 0x14, 0x15
};

/* ============================================
 * Voice allocation state
 * ============================================ */

typedef struct {
    int     active;
    int     mus_channel;
    int     note;
    int     instrument;
    uint8_t car_level;       /* Carrier TL from GENMIDI (0-63) */
    uint8_t mod_level;       /* Modulator TL from GENMIDI (0-63) */
    uint8_t car_scale;       /* Carrier KSL (0-3) */
    uint8_t mod_scale;       /* Modulator KSL (0-3) */
    uint8_t connection;      /* 0 = FM, 1 = additive */
} voice_t;

static voice_t voices[NUM_OPL_VOICES];
static uint8_t voice_reg_b0[NUM_OPL_VOICES];   /* Cached 0xB0 register */

/* ============================================
 * OPL2 F-number table (one octave: C..B)
 * freq = fnum * 49716 / 2^(20 - block)
 * ============================================ */

static const uint16_t fnum_table[12] = {
    0x157, 0x16B, 0x181, 0x198, 0x1B0, 0x1CA,
    0x1E5, 0x202, 0x220, 0x241, 0x263, 0x287
};

/* ============================================
 * Volume curve (from Chocolate Doom)
 * MIDI velocity 0-127 -> OPL loudness 0-127
 * ============================================ */

static const uint8_t volume_curve[128] = {
      0,   1,   3,   5,   6,   8,  10,  11,
     13,  14,  16,  17,  19,  20,  22,  23,
     25,  26,  27,  29,  30,  32,  33,  34,
     36,  37,  39,  41,  43,  45,  47,  49,
     50,  52,  54,  55,  57,  59,  60,  61,
     63,  64,  66,  67,  68,  69,  71,  72,
     73,  74,  75,  76,  77,  79,  80,  81,
     82,  83,  84,  84,  85,  86,  87,  88,
     89,  90,  91,  92,  92,  93,  94,  95,
     96,  96,  97,  98,  99,  99, 100, 101,
    101, 102, 103, 103, 104, 105, 105, 106,
    107, 107, 108, 109, 109, 110, 110, 111,
    112, 112, 113, 113, 114, 114, 115, 115,
    116, 117, 117, 118, 118, 119, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 123,
    124, 124, 125, 125, 126, 126, 127, 127
};

/* ============================================
 * MUS parser state
 * ============================================ */

static uint8_t *mus_data;
static int mus_length;
static int mus_pos;
static int mus_score_start;
static int mus_looping;
static int mus_playing;
static int mus_paused;
static int mus_delay;           /* 16.8 fixed-point samples */

static int mus_chan_instr[MUS_CHANNELS];
static int mus_chan_volume[MUS_CHANNELS];
static int mus_chan_pitch[MUS_CHANNELS];

static int music_volume = 127;  /* 0-127 internal */

/* ============================================
 * OPL helpers
 * ============================================ */

PD_FASTTEXT static inline void
opl_write(uint16_t reg, uint8_t val)
{
    OPL2_ADDR = (uint32_t)(reg & 0xFF);
    OPL2_DATA = (uint32_t)val;
}

PD_FASTTEXT static void
load_operator(int ch, const genmidi_op_t *op, int is_carrier)
{
    int off = is_carrier ? op2_off[ch] : op1_off[ch];

    opl_write(0xE0 + off, op->tremolo);          /* byte 0 = waveform select */
    opl_write(0x40 + off, op->scale | 0x3F);   /* byte 4 = KSL+TL, silent initially */
    opl_write(0x60 + off, op->attack);
    opl_write(0x80 + off, op->sustain);
    opl_write(0x20 + off, op->waveform);       /* byte 3 = AM/VIB/EGT/KSR/MULT */
}

PD_FASTTEXT static void
set_instrument(int ch, const genmidi_voice_t *v)
{
    load_operator(ch, &v->modulator, 0);
    load_operator(ch, &v->carrier, 1);

    /* Feedback/connection + enable both stereo outputs */
    opl_write(0xC0 + ch, (v->feedback & 0x0F) | 0x30);

    voices[ch].car_level  = v->carrier.level & 0x3F;
    voices[ch].mod_level  = v->modulator.level & 0x3F;
    voices[ch].car_scale  = v->carrier.scale & 0xC0;
    voices[ch].mod_scale  = v->modulator.scale & 0xC0;
    voices[ch].connection = v->feedback & 0x01;

}

PD_FASTTEXT static void
set_volume(int ch, int velocity)
{
    /* Scale velocity by music_volume (0-127) so the menu slider works */
    int vol = volume_curve[velocity & 0x7F];
    vol = (vol * music_volume) / 127;

    /* Carrier: scale "loudness" portion by velocity */
    int car_tl = 0x3F - ((0x3F - voices[ch].car_level) * vol) / 128;
    opl_write(0x40 + op2_off[ch],
              voices[ch].car_scale | (car_tl & 0x3F));

    /* Modulator: adjust only in additive mode; keep GENMIDI value in FM */
    if (voices[ch].connection) {
        int mod_tl = 0x3F - ((0x3F - voices[ch].mod_level) * vol) / 128;
        opl_write(0x40 + op1_off[ch],
                  voices[ch].mod_scale | (mod_tl & 0x3F));
    } else {
        opl_write(0x40 + op1_off[ch],
                  voices[ch].mod_scale | (voices[ch].mod_level & 0x3F));
    }
}

PD_FASTTEXT static void
set_frequency(int ch, int note, int key_on)
{
    int semi  = note % 12;
    int block = (note / 12) - 1;
    uint16_t fnum = fnum_table[semi];

    if (block < 0) {
        fnum >>= (-block);
        block = 0;
    }
    if (block > 7)
        block = 7;

    uint8_t b0 = (key_on ? 0x20 : 0x00)
               | ((block & 0x07) << 2)
               | ((fnum >> 8) & 0x03);

    opl_write(0xA0 + ch, fnum & 0xFF);
    opl_write(0xB0 + ch, b0);
    voice_reg_b0[ch] = b0;
}

PD_FASTTEXT static void
voice_key_off(int ch)
{
    opl_write(0xB0 + ch, voice_reg_b0[ch] & ~0x20);
    voice_reg_b0[ch] &= ~0x20;
}

/* ============================================
 * Voice allocation
 * ============================================ */

PD_FASTTEXT static int
alloc_voice(int mus_channel)
{
    int i;

    for (i = 0; i < NUM_OPL_VOICES; i++)
        if (!voices[i].active)
            return i;

    for (i = 0; i < NUM_OPL_VOICES; i++)
        if (voices[i].mus_channel == mus_channel)
            return i;

    return 0;
}

/* ============================================
 * Note on / off
 * ============================================ */

PD_FASTTEXT static void
note_on(int mus_channel, int note, int velocity)
{
    int idx, instr_idx, play_note;
    const genmidi_instr_t *instr;

    if (!genmidi_loaded) return;

    if (mus_channel == PERCUSSION_CHAN) {
        instr_idx = note - 35 + PERCUSSION_BASE;
        if (instr_idx < PERCUSSION_BASE || instr_idx >= GENMIDI_NUM_INSTRS)
            return;
        play_note = 60;
    } else {
        instr_idx = mus_chan_instr[mus_channel];
        if (instr_idx < 0 || instr_idx >= 128)
            return;
        play_note = note;
    }

    instr = &genmidi[instr_idx];

    if (instr->flags & GENMIDI_FLAG_FIXED)
        play_note = instr->fixed_note;

    play_note += instr->voice[0].base_note_offset;
    if (play_note < 0)   play_note = 0;
    if (play_note > 127) play_note = 127;

    idx = alloc_voice(mus_channel);

    if (voices[idx].active)
        voice_key_off(idx);

    set_instrument(idx, &instr->voice[0]);
    set_volume(idx, velocity);
    set_frequency(idx, play_note, 1);

    voices[idx].active      = 1;
    voices[idx].mus_channel = mus_channel;
    voices[idx].note        = note;
    voices[idx].instrument  = instr_idx;
}

PD_FASTTEXT static void
note_off(int mus_channel, int note)
{
    int i;
    for (i = 0; i < NUM_OPL_VOICES; i++) {
        if (voices[i].active
            && voices[i].mus_channel == mus_channel
            && voices[i].note == note) {
            voice_key_off(i);
            voices[i].active = 0;
            break;
        }
    }
}

/* ============================================
 * GENMIDI loader
 * ============================================ */

static void
load_genmidi(void)
{
    int lump;
    uint8_t *data;
    int len;

    lump = W_CheckNumForName("GENMIDI");
    if (lump < 0) {
        printf("I_InitMusic: GENMIDI lump not found\n");
        return;
    }

    data = (uint8_t *)W_CacheLumpNum(lump, PU_STATIC);
    len  = W_LumpLength(lump);

    if (len < (int)(8 + GENMIDI_NUM_INSTRS * sizeof(genmidi_instr_t))
        || memcmp(data, "#OPL_II#", 8) != 0) {
        printf("I_InitMusic: invalid GENMIDI lump\n");
        Z_Free(data);
        return;
    }

    memcpy(genmidi, data + 8, GENMIDI_NUM_INSTRS * sizeof(genmidi_instr_t));

    Z_Free(data);
    genmidi_loaded = 1;

    printf("I_InitMusic: loaded %d instruments\n", GENMIDI_NUM_INSTRS);
}

/* ============================================
 * MUS parser
 * ============================================ */

PD_FASTTEXT static int
mus_read_byte(void)
{
    if (!mus_data || mus_pos >= mus_length)
        return -1;
    return mus_data[mus_pos++];
}

PD_FASTTEXT static int
mus_read_delay(void)
{
    int delay = 0;
    int b;
    do {
        b = mus_read_byte();
        if (b < 0) return -1;
        delay = (delay << 7) | (b & 0x7F);
    } while (b & 0x80);
    return delay;
}

PD_FASTTEXT static void
mus_process_events(void)
{
    int done = 0;

    while (!done) {
        int header = mus_read_byte();
        if (header < 0) { mus_playing = 0; return; }

        int last    = header & 0x80;
        int type    = (header >> 4) & 0x07;
        int channel = header & 0x0F;
        int b1, b2;

        switch (type) {
        case 0: /* Release note */
            b1 = mus_read_byte();
            if (b1 < 0) { mus_playing = 0; return; }
            note_off(channel, b1 & 0x7F);
            break;

        case 1: /* Play note */
            b1 = mus_read_byte();
            if (b1 < 0) { mus_playing = 0; return; }
            if (b1 & 0x80) {
                b2 = mus_read_byte();
                if (b2 < 0) { mus_playing = 0; return; }
                mus_chan_volume[channel] = b2 & 0x7F;
            }
            note_on(channel, b1 & 0x7F, mus_chan_volume[channel]);
            break;

        case 2: /* Pitch bend */
            b1 = mus_read_byte();
            if (b1 < 0) { mus_playing = 0; return; }
            mus_chan_pitch[channel] = b1;
            break;

        case 3: /* System event (ignored) */
            mus_read_byte();
            break;

        case 4: /* Controller */
            b1 = mus_read_byte();
            b2 = mus_read_byte();
            if (b1 < 0 || b2 < 0) { mus_playing = 0; return; }
            if (b1 == 0)
                mus_chan_instr[channel] = b2;
            else if (b1 == 3)
                mus_chan_volume[channel] = b2;
            break;

        case 6: /* Score end */
            if (mus_looping) {
                mus_pos = mus_score_start;
            } else {
                mus_playing = 0;
                return;
            }
            break;

        default:
            break;
        }

        if (last) {
            int delay = mus_read_delay();
            if (delay < 0) { mus_playing = 0; return; }
            mus_delay += delay * SAMPLES_PER_TICK;
            done = 1;
        }
    }
}

/* ============================================
 * Cycle counter (100 MHz) for real-time MUS pacing
 * ============================================ */

#define MUS_CYCLE_LO  (*(volatile uint32_t *)0x40000004)

static uint32_t mus_last_cycles;
static int      mus_time_init;

/* ============================================
 * OPL_AdvanceMusic — called once per game loop
 * Advances MUS parser based on real elapsed time,
 * decoupled from mixer sample count so music
 * never slows down even if rendering is slow.
 * ============================================ */

PD_FASTTEXT void
OPL_AdvanceMusic(void)
{
    if (!genmidi_loaded || !mus_playing || mus_paused)
        return;

    uint32_t now = MUS_CYCLE_LO;

    if (!mus_time_init) {
        mus_last_cycles = now;
        mus_time_init = 1;
        return;
    }

    /* Elapsed cycles (handles 32-bit wrap naturally) */
    uint32_t elapsed = now - mus_last_cycles;
    mus_last_cycles = now;

    /* Convert elapsed CPU cycles to 11025 Hz samples in 16.8 fixed-point.
     * samples_fp = elapsed * SAMPLERATE * 256 / CPU_HZ
     * Carry over fractional remainder to prevent progressive drift. */
    static uint32_t mus_frac_accum = 0;
    uint64_t total = (uint64_t)elapsed * 2822400 + mus_frac_accum;
    int samples_fp = (int)(total / 100227260);
    mus_frac_accum = (uint32_t)(total % 100227260);

    /* Cap at ~100ms worth to prevent burst after long stalls */
    if (samples_fp > 1102 * 256)
        samples_fp = 1102 * 256;

    mus_delay -= samples_fp;

    while (mus_delay <= 0 && mus_playing)
        mus_process_events();
}

/* ============================================
 * Music API
 * ============================================ */

void
I_InitMusic(void)
{
    int i;

    printf("I_InitMusic: initializing hardware OPL2\n");

    /* Clear all OPL2 registers (0x01-0xF5) via MMIO */
    for (i = 1; i <= 0xF5; i++)
        opl_write((uint16_t)i, 0x00);

    /* Enable OPL2 waveform selection */
    opl_write(0x01, 0x20);

    memset(voices, 0, sizeof(voices));
    memset(voice_reg_b0, 0, sizeof(voice_reg_b0));

    for (i = 0; i < MUS_CHANNELS; i++) {
        mus_chan_instr[i]  = 0;
        mus_chan_volume[i] = 100;
        mus_chan_pitch[i]  = 128;
    }

    load_genmidi();

    printf("I_InitMusic: ready\n");
}

void
I_ShutdownMusic(void)
{
}


void
I_SetMusicVolume(int volume)
{
    snd_MusicVolume = volume;
    music_volume = volume * 127 / 15;
    if (music_volume > 127) music_volume = 127;

    /* Re-apply volume to all active OPL voices so the slider takes
     * effect immediately, not just on the next note-on. */
    for (int i = 0; i < NUM_OPL_VOICES; i++) {
        if (voices[i].active)
            set_volume(i, mus_chan_volume[voices[i].mus_channel]);
    }
}

int
I_RegisterSong(void *data)
{
    uint8_t *d = (uint8_t *)data;

    if (!d || d[0] != 'M' || d[1] != 'U' || d[2] != 'S' || d[3] != 0x1A) {
        printf("I_RegisterSong: bad MUS header\n");
        return 0;
    }

    mus_data        = d;
    mus_length      = (d[4] | (d[5] << 8)) + (d[6] | (d[7] << 8));
    mus_score_start = d[6] | (d[7] << 8);
    mus_pos         = mus_score_start;
    mus_playing     = 0;
    mus_delay       = 0;

    return 1;
}

void
I_PlaySong(int handle, int looping)
{
    int i;
    (void)handle;

    if (!mus_data || !genmidi_loaded) return;

    for (i = 0; i < NUM_OPL_VOICES; i++) {
        if (voices[i].active)
            voice_key_off(i);
        voices[i].active = 0;
    }

    mus_looping = looping;
    mus_playing = 1;
    mus_paused  = 0;
    mus_pos     = mus_score_start;
    mus_delay   = 0;
    mus_time_init = 0;  /* Reset real-time tracking */

    for (i = 0; i < MUS_CHANNELS; i++) {
        mus_chan_volume[i] = 100;
        mus_chan_pitch[i]  = 128;
    }
}

void
I_StopSong(int handle)
{
    int i;
    (void)handle;

    mus_playing = 0;

    for (i = 0; i < NUM_OPL_VOICES; i++) {
        if (voices[i].active)
            voice_key_off(i);
        voices[i].active = 0;
    }
}

void
I_PauseSong(int handle)
{
    (void)handle;
    mus_paused = 1;
}

void
I_ResumeSong(int handle)
{
    (void)handle;
    mus_paused = 0;
}

void
I_UnRegisterSong(int handle)
{
    (void)handle;
    mus_data    = 0;
    mus_playing = 0;
}
