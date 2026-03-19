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

/* Hardware OPL3 MMIO registers (bank 0 + bank 1) */
#define OPL_ADDR0  (*(volatile uint32_t *)0x4E000000)  /* Bank 0 address */
#define OPL_DATA0  (*(volatile uint32_t *)0x4E000004)  /* Bank 0 data    */
#define OPL_ADDR1  (*(volatile uint32_t *)0x4E000008)  /* Bank 1 address */
#define OPL_DATA1  (*(volatile uint32_t *)0x4E00000C)  /* Bank 1 data    */

/* ============================================
 * Constants
 * ============================================ */

#define SAMPLERATE       11025
#define MUS_TICKRATE     140
#define SAMPLES_PER_TICK ((SAMPLERATE << 8) / MUS_TICKRATE)  /* 16.8 fixed-point */

#define NUM_OPL_VOICES   18
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
 * OPL3 register offset tables (18 channels)
 * ============================================
 * Channels 0-8: bank 0 (offsets 0x000+)
 * Channels 9-17: bank 1 (offsets 0x100+)
 * opl_write selects the bank based on bit 8.
 */

static const uint16_t op1_off[18] = {
    0x000, 0x001, 0x002, 0x008, 0x009, 0x00A, 0x010, 0x011, 0x012,
    0x100, 0x101, 0x102, 0x108, 0x109, 0x10A, 0x110, 0x111, 0x112
};
static const uint16_t op2_off[18] = {
    0x003, 0x004, 0x005, 0x00B, 0x00C, 0x00D, 0x013, 0x014, 0x015,
    0x103, 0x104, 0x105, 0x10B, 0x10C, 0x10D, 0x113, 0x114, 0x115
};
/* Per-channel base for frequency (0xA0/0xB0) and feedback (0xC0) regs */
static const uint16_t ch_base[18] = {
    0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x006, 0x007, 0x008,
    0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108
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
 * OPL F-number table (one octave: C..B)
 * Scaled for 48,000 Hz sample rate (12.288 MHz / 256).
 * freq = fnum * 48000 / 2^(20 - block)
 * Standard values × 49716/48000 = 1.0357
 * ============================================ */

static const uint16_t fnum_table[12] = {
    0x163, 0x178, 0x18F, 0x1A7, 0x1BF, 0x1DA,
    0x1F6, 0x214, 0x234, 0x256, 0x279, 0x29E
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
    if (reg & 0x100) {
        OPL_ADDR1 = (uint32_t)(reg & 0xFF);
        OPL_DATA1 = (uint32_t)val;
    } else {
        OPL_ADDR0 = (uint32_t)(reg & 0xFF);
        OPL_DATA0 = (uint32_t)val;
    }
}

PD_FASTTEXT static void
load_operator(int ch, const genmidi_op_t *op, int is_carrier)
{
    int off = is_carrier ? op2_off[ch] : op1_off[ch];

    opl_write(0x20 + off, op->tremolo);          /* byte 0: AM/VIB/EGT/KSR/MULT */
    opl_write(0x40 + off, op->scale | 0x3F);   /* byte 4: KSL+TL, silent initially */

    /* Attack/Decay: cap attack rate at 11 for smoother note onset. */
    int atk_rate = (op->attack >> 4) & 0x0F;
    if (atk_rate > 11) atk_rate = 11;
    opl_write(0x60 + off, (atk_rate << 4) | (op->attack & 0x0F));
    /* Sustain/Release: slow down fast releases for smoother fade-out.
     * Release rate (lower nibble): cap at 8 (out of 15). */
    int rel_rate = op->sustain & 0x0F;
    if (rel_rate > 8) rel_rate = 8;
    opl_write(0x80 + off, (op->sustain & 0xF0) | rel_rate);

    opl_write(0xE0 + off, op->waveform);        /* byte 3: Waveform select */
}

PD_FASTTEXT static void
set_instrument(int ch, const genmidi_voice_t *v)
{
    load_operator(ch, &v->modulator, 0);
    load_operator(ch, &v->carrier, 1);

    /* Feedback/connection + enable both stereo outputs.
     * Cap feedback at 5 (out of 7) to reduce harshness. */
    int fb = (v->feedback >> 1) & 0x07;
    int conn = v->feedback & 0x01;
    if (fb > 5) fb = 5;
    opl_write(0xC0 + ch_base[ch], (fb << 1) | conn | 0x30);

    voices[ch].car_level  = v->carrier.level & 0x3F;
    voices[ch].mod_level  = v->modulator.level & 0x3F;
    voices[ch].car_scale  = v->carrier.scale & 0xC0;
    voices[ch].mod_scale  = v->modulator.scale & 0xC0;
    voices[ch].connection = v->feedback & 0x01;

}

PD_FASTTEXT static void
set_volume(int ch, int velocity)
{
    /* Match Chocolate Doom's volume calculation (i_oplmusic.c SetVoiceVolume):
     *   midi_volume = 2 * (volume_curve[min(chan_vol, music_vol)] + 1)
     *   full_volume = (volume_curve[note_vel] * midi_volume) >> 9
     *   carrier TL  = 0x3F - full_volume                          (0..63)
     * MUS uses a single volume for both channel and note velocity. */
    int vel = velocity & 0x7F;
    int capped = (vel > music_volume) ? music_volume : vel;
    int midi_vol = 2 * (volume_curve[capped] + 1);
    int full_vol = (volume_curve[vel] * midi_vol) >> 9;
    if (full_vol > 0x3F) full_vol = 0x3F;

    /* Carrier: pure volume control — ignores GENMIDI carrier TL
     * (GENMIDI carrier TL was already written at instrument load). */
    int car_tl = 0x3F - full_vol;
    opl_write(0x40 + op2_off[ch],
              voices[ch].car_scale | (car_tl & 0x3F));

    /* Modulator: in additive mode, never louder than GENMIDI intended.
     * Use max(genmidi_mod_level, car_tl) — matches Chocolate Doom.
     * In FM mode, keep GENMIDI value unchanged. */
    if (voices[ch].connection) {
        int mod_tl = voices[ch].mod_level;
        if (mod_tl < car_tl)
            mod_tl = car_tl;
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
    /* Shift very low notes up by an octave — OPL waveform stepping
     * is most audible below ~C2 (MIDI 36) where block=1 and the
     * phase accumulator advances slowly through the sine LUT. */
    while (note < 37 && note > 0)
        note += 12;

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

    opl_write(0xA0 + ch_base[ch], fnum & 0xFF);
    opl_write(0xB0 + ch_base[ch], b0);
    voice_reg_b0[ch] = b0;
}

PD_FASTTEXT static void
voice_key_off(int ch)
{
    opl_write(0xB0 + ch_base[ch], voice_reg_b0[ch] & ~0x20);
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
        play_note = note;
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

    /* Percussion: drop pitch by 1 semitone for deeper thump. */
    if (mus_channel == PERCUSSION_CHAN) {
        play_note -= 1;
        if (play_note < 0) play_note = 0;
    }


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

    /* Standard GENMIDI: 8-byte header + 175 × 36-byte patches + optional
     * 175 × 32-byte names at end.  Total 6308 or 11908 bytes. */
    if (len < (int)(8 + GENMIDI_NUM_INSTRS * sizeof(genmidi_instr_t))
        || memcmp(data, "#OPL_II#", 8) != 0) {
        printf("I_InitMusic: invalid GENMIDI lump (%d bytes)\n", len);
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

        case 3: /* System event */
            b1 = mus_read_byte();
            if (b1 < 0) { mus_playing = 0; return; }
            if (b1 == 11 || b1 == 12) {
                /* All notes off / Mono: release all voices on this channel */
                for (int v = 0; v < NUM_OPL_VOICES; v++) {
                    if (voices[v].active && voices[v].mus_channel == channel) {
                        voice_key_off(v);
                        voices[v].active = 0;
                    }
                }
            } else if (b1 == 10) {
                /* All sounds off: immediately silence all voices on channel */
                for (int v = 0; v < NUM_OPL_VOICES; v++) {
                    if (voices[v].active && voices[v].mus_channel == channel) {
                        voice_key_off(v);
                        voices[v].active = 0;
                    }
                }
            }
            break;

        case 4: /* Controller */
            b1 = mus_read_byte();
            b2 = mus_read_byte();
            if (b1 < 0 || b2 < 0) { mus_playing = 0; return; }
            if (b1 == 0)
                mus_chan_instr[channel] = b2;
            else if (b1 == 3) {
                mus_chan_volume[channel] = b2;
                /* Update volume on all active voices for this channel */
                for (int v = 0; v < NUM_OPL_VOICES; v++) {
                    if (voices[v].active && voices[v].mus_channel == channel)
                        set_volume(v, b2);
                }
            }
            break;

        case 6: /* Score end */
            if (mus_looping) {
                /* Key-off all active voices before restarting */
                for (int v = 0; v < NUM_OPL_VOICES; v++) {
                    if (voices[v].active) {
                        voice_key_off(v);
                        voices[v].active = 0;
                    }
                }
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

    printf("I_InitMusic: initializing OPL3 (18 voices)\n");

    /* Clear all bank 0 registers */
    for (i = 1; i <= 0xF5; i++)
        opl_write((uint16_t)i, 0x00);

    /* Enable OPL3 mode — MUST be done before writing bank 1 */
    opl_write(0x105, 0x01);

    /* Clear all bank 1 registers */
    for (i = 0x101; i <= 0x1F5; i++)
        opl_write((uint16_t)i, 0x00);

    /* Disable 4-operator mode (all 18 channels are independent 2-op) */
    opl_write(0x104, 0x00);

    /* Enable waveform selection on both banks */
    opl_write(0x01, 0x20);
    opl_write(0x101, 0x20);

    /* NOTE-SEL=1: changes key-scale number derivation to use fnum bit 8
     * instead of bit 9.  Matches Chocolate Doom and original DMX driver.
     * Affects KSL (volume scaling with pitch) and KSR (envelope rate
     * scaling with pitch) for every note. */
    opl_write(0x08, 0x40);
    opl_write(0x108, 0x40);

    /* Shallow tremolo + vibrato */
    opl_write(0xBD, 0x00);

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
