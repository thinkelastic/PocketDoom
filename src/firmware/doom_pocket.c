/*
 * Doom platform layer for Analogue Pocket (PocketDoom)
 *
 * Implements the Doom I/O interfaces (i_video, i_system) using:
 * - SDRAM double-buffered 8-bit indexed framebuffer with hardware palette
 * - 110 MHz cycle counter for timing
 * - Analogue Pocket controller input via MMIO registers
 */

#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "d_event.h"
#include "d_player.h"
#include "g_game.h"
#include "m_misc.h"
#include "i_system.h"
#include "i_sound.h"
#include "i_video.h"
#include "v_video.h"

/* From doom_music.c — advance MUS parser (does OPL MMIO writes) */
extern void OPL_AdvanceMusic(void);

/* From doom_sound.c — mix and fill ring buffer */
extern void I_UpdateSound(void);


/* ============================================
 * Hardware register definitions
 * ============================================ */

#define SYS_BASE            0x40000000
#define SYS_STATUS          (*(volatile uint32_t *)(SYS_BASE + 0x00))
#define SYS_CYCLE_LO        (*(volatile uint32_t *)(SYS_BASE + 0x04))
#define SYS_CYCLE_HI        (*(volatile uint32_t *)(SYS_BASE + 0x08))
#define SYS_DISPLAY_MODE    (*(volatile uint32_t *)(SYS_BASE + 0x0C))
#define SYS_FB_DISPLAY      (*(volatile uint32_t *)(SYS_BASE + 0x10))
#define SYS_FB_DRAW         (*(volatile uint32_t *)(SYS_BASE + 0x14))
#define SYS_FB_SWAP         (*(volatile uint32_t *)(SYS_BASE + 0x18))
#define SYS_PAL_INDEX       (*(volatile uint32_t *)(SYS_BASE + 0x40))
#define SYS_PAL_DATA        (*(volatile uint32_t *)(SYS_BASE + 0x44))
#define SYS_CONT1_KEY       (*(volatile uint32_t *)(SYS_BASE + 0x50))
#define SYS_CONT1_JOY       (*(volatile uint32_t *)(SYS_BASE + 0x54))
#define SYS_CONT1_TRIG      (*(volatile uint32_t *)(SYS_BASE + 0x58))
#define SYS_SHUTDOWN        (*(volatile uint32_t *)(SYS_BASE + 0x6C))
#define SYS_ORIGINAL_MODE   (*(volatile uint32_t *)(SYS_BASE + 0x70))
#define SYS_RUN_MODE        (*(volatile uint32_t *)(SYS_BASE + 0x74))
#define SYS_REFRESH_RATE    (*(volatile uint32_t *)(SYS_BASE + 0x78))
#define SYS_V_TOTAL         (*(volatile uint32_t *)(SYS_BASE + 0x7C))

/* VRR limits: 47-62 Hz at 12.288 MHz pixel clock, H_TOTAL=780 */
#define VRR_V_MIN   262   /* 60 Hz */
#define VRR_V_MAX   335   /* 47 Hz */

/* Run mode values */
#define RUN_MODE_ALWAYS     0   /* Always run (default) */
#define RUN_MODE_USE_RUN    1   /* Use button also triggers run */
#define RUN_MODE_DOUBLETAP  2   /* Double-tap forward to run */


/* SDRAM framebuffer addresses (CPU byte addresses) */
#define SDRAM_BASE          0x10000000
#define FB_VOFFSET_LINES    0                         /* Full 320x240 — no vertical offset needed */
#define FB_STRIDE           320                       /* Bytes per line */
#define FB_LINES            240                       /* Total scanout lines */
#define FB_VOFFSET          (FB_STRIDE * FB_VOFFSET_LINES)
#define FB0_ADDR            (SDRAM_BASE + FB_VOFFSET + 0x000000)  /* Framebuffer 0 */
#define FB1_ADDR            (SDRAM_BASE + FB_VOFFSET + 0x100000)  /* Framebuffer 1 */


/* Convert SDRAM word address from register to CPU byte address */
#define FB_REG_TO_CPU(reg)  (SDRAM_BASE + ((reg) << 1))

/* Cache eviction region: reading contiguous SDRAM forces the 128KB 2-way
 * D-cache to evict all dirty lines.  128KB fills both ways of every set. */
#define CACHE_FLUSH_ADDR    0x10380000
#define CACHE_FLUSH_SIZE    (128 * 1024)

/* CPU clock frequency */
#define CPU_HZ              100000000
#define TICRATE             35
#define CYCLES_PER_TIC      (CPU_HZ / TICRATE)

/* Precomputed reciprocal for tic accumulator (16.16 fixed-point):
 * (1 << 48) / CYCLES_PER_TIC.  Multiply by dt then >> 16 to get
 * the same result as (dt << 32) / CYCLES_PER_TIC, avoiding the
 * expensive 64-bit software division on rv32.
 * Value: 281474976710656 / 2857142 = 98,517,999 */
#define TIC_RECIP_48        ((uint64_t)((((uint64_t)1 << 48) + CYCLES_PER_TIC - 1) / CYCLES_PER_TIC))

/* Analogue Pocket controller button bits (active high) */
#define PAD_DPAD_UP         (1 << 0)
#define PAD_DPAD_DOWN       (1 << 1)
#define PAD_DPAD_LEFT       (1 << 2)
#define PAD_DPAD_RIGHT      (1 << 3)
#define PAD_FACE_A          (1 << 4)
#define PAD_FACE_B          (1 << 5)
#define PAD_FACE_X          (1 << 6)
#define PAD_FACE_Y          (1 << 7)
#define PAD_TRIG_L1         (1 << 8)
#define PAD_TRIG_R1         (1 << 9)
#define PAD_TRIG_L2         (1 << 10)
#define PAD_TRIG_R2         (1 << 11)
#define PAD_TRIG_L3         (1 << 12)
#define PAD_TRIG_R3         (1 << 13)
#define PAD_FACE_SELECT     (1 << 14)
#define PAD_FACE_START      (1 << 15)
/* Analog trigger press threshold (0..255) */
#define TRIG_PRESS_THRESHOLD 64

/* ============================================
 * Entry point (called by bootloader)
 * ============================================ */

/* Original Mode global — set from interact.json Core Option */
int original_mode = 0;

/* Run Mode global — set from interact.json Core Option */
static int run_mode = RUN_MODE_ALWAYS;

/* Linker symbols for heap region */
extern char _heap_start[], _heap_end[];

void doom_main(void)
{
    /* Initialize heap allocator (must happen before any malloc) */
    heap_init(_heap_start, _heap_end - _heap_start);

    D_DoomMain();  /* Never returns */
}

/* ============================================
 * Video interface (i_video.h)
 * ============================================ */

void I_InitGraphics(void)
{
    /* Read settings from interact.json Core Options */
    original_mode = (SYS_ORIGINAL_MODE != 0) ? 1 : 0;
    run_mode = SYS_RUN_MODE;

    /* Set default gamma */
    usegamma = 1;

    /* Clear both framebuffers to black via uncached alias so the
     * video scanout hardware sees zeros immediately. */
    memset((void *)(0x50000000 + 0x000000), 0, FB_STRIDE * FB_LINES);
    memset((void *)(0x50000000 + 0x100000), 0, FB_STRIDE * FB_LINES);

    /* Point screens[0] at the current draw framebuffer (cached SDRAM).
     * Doom will render directly into it — no memcpy needed in I_FinishUpdate. */
    uint32_t draw_reg = SYS_FB_DRAW;
    screens[0] = (byte *)FB_REG_TO_CPU(draw_reg) + FB_VOFFSET;

    /* Clear terminal text and switch to framebuffer display mode */
    extern void term_clear(void);
    term_clear();
    SYS_DISPLAY_MODE = 1;
}

void I_ShutdownGraphics(void)
{
}

void I_SetPalette(byte *palette)
{
    /* Write all 256 palette entries via auto-incrementing registers */
    SYS_PAL_INDEX = 0;
    for (int i = 0; i < 256; i++) {
        byte r = gammatable[usegamma][*palette++];
        byte g = gammatable[usegamma][*palette++];
        byte b = gammatable[usegamma][*palette++];
        SYS_PAL_DATA = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

void I_UpdateNoBlit(void)
{
}

PD_FASTTEXT void I_FinishUpdate(void)
{
    /* screens[0] points directly at the draw framebuffer in cached SDRAM.
     * Doom has already rendered into it.  Force D-cache writeback by reading
     * 128KB of unrelated SDRAM — this evicts all dirty FB lines from the
     * 64KB 2-way D-cache, flushing them to physical SDRAM where the video
     * scanout can read them.
     *
     * Only one read per cache line is needed to trigger a fill (and evict
     * the old dirty line).  Stride by 32 bytes (minimum RISC-V D-cache
     * line size) so we do ~4096 SDRAM reads instead of ~32768.  The
     * ~28K redundant same-line hits were pure overhead (~1.5ms saved).
     *
     * Stride by 64 bytes (16 words) — matches VexiiRiscv's 64-byte
     * D-cache line size.  2048 reads instead of 4096. */
    volatile uint32_t *flush = (volatile uint32_t *)CACHE_FLUSH_ADDR;
    uint32_t sink;
    for (int i = 0; i < (int)(CACHE_FLUSH_SIZE / 64); i++)
        sink = flush[i * 16];
    (void)sink;

    /* Black bars in original mode (320x200) are cleared once on mode
     * switch (I_StartFrame).  Nothing renders into y < RENDER_YOFF or
     * y >= RENDER_YOFF + 200: the renderer starts at RENDER_YOFF,
     * HUD messages are offset by RENDER_YOFF, and menus trigger a
     * bar re-clear on state change.  No per-frame clearing needed. */

    /* Request buffer swap on next vsync.  I_StartFrame() at the top
     * of the game loop waits for this swap to complete before any
     * drawing begins, preventing the CPU from writing into the
     * buffer that the FPGA is still scanning out. */
    SYS_FB_SWAP = 1;

    /* Toggle screens[0] between the two framebuffers */
    uint32_t cur_draw = (uint32_t)screens[0];
    screens[0] = (byte *)(cur_draw == FB0_ADDR ? FB1_ADDR : FB0_ADDR);
}

void I_WaitVBL(int count)
{
    /* Simple delay - wait for approximately count vblanks (60 Hz) */
    uint32_t cycles_per_vbl = CPU_HZ / 60;
    uint32_t start = SYS_CYCLE_LO;
    while ((SYS_CYCLE_LO - start) < (uint32_t)count * cycles_per_vbl)
        ;
}

void I_ReadScreen(byte *scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

/* ============================================
 * System interface (i_system.h)
 * ============================================ */

void I_Init(void)
{
    I_InitSound();
    I_InitMusic();
}

byte *I_ZoneBase(int *size)
{
    /* Give Doom's zone allocator as much heap as possible.
     * Heap runs from __doom_bss_end to the stack bottom (~54MB).
     * Leave 1MB headroom for malloc overhead and other allocations. */
    int available = (_heap_end - _heap_start) - (1024 * 1024);
    if (available < 6 * 1024 * 1024)
        available = 6 * 1024 * 1024;  /* Minimum 6MB fallback */
    *size = available;
    return (byte *)malloc(*size);
}

/* ============================================
 * Vsync-locked tic accumulator
 *
 * Doom's game logic runs at 35 Hz while the display refreshes at 60 Hz.
 *
 * Previously I_GetTime derived tics from the free-running CPU cycle
 * counter.  Because the CPU clock and video clock come from different
 * PLL outputs, there is a tiny frequency mismatch.  This causes the
 * tic/frame phase to slowly drift: every few seconds a tic boundary
 * lands right on a frame boundary, giving one frame an extra tic of
 * game-logic work that pushes it past the 16.67 ms vsync deadline —
 * visible as a periodic micro-stutter.
 *
 * Fix: at each vsync, measure elapsed CPU cycles and accumulate
 * fractional tics (dt / CYCLES_PER_TIC) in 32.32 fixed-point.
 * This preserves the original game speed (set by CPU_HZ / TICRATE)
 * while guaranteeing tic boundaries always fall on a vsync.
 * ============================================ */

/* Vsync-locked tic accumulator (32.32 fixed-point).
 *
 * Each vsync, I_StartFrame measures the elapsed CPU cycles and adds
 * frame_cycles / CYCLES_PER_TIC to this accumulator.  The integer
 * part is the tic count returned by I_GetTime; the fractional part
 * drives I_GetTimeFrac for smooth interpolation.
 *
 * This preserves the original game speed (determined by CPU_HZ and
 * TICRATE) while guaranteeing that tic boundaries always fall exactly
 * on a vsync — eliminating the slow phase drift between the CPU
 * cycle counter and the video clock that caused periodic stutters. */
static uint64_t tic_accum = 0;         /* 32.32 fixed-point total tics */
static uint32_t last_vsync_cycles = 0; /* Cycle counter at previous vsync */
static uint32_t smooth_dt = 0;         /* EMA-smoothed vsync period (cycles) */


int I_GetTime(void)
{
    /* Shutdown check: bridge reads CRAM1 directly, just ack and halt */
    if (SYS_SHUTDOWN & 1) {
        SYS_SHUTDOWN = 1;  /* ack */
        while (1) {}        /* halt — hardware reset follows */
    }

    /* Integer part of the accumulated tics */
    return (int)(tic_accum >> 32);
}

int I_GetTimeFrac(void)
{
    /* Fractional part of tic_accum → 0..FRACUNIT.
     * Gives smooth sub-tic position locked to vsync timing. */
    uint32_t frac32 = (uint32_t)tic_accum;   /* low 32 bits = 0.32 fraction */
    return (int)(frac32 >> (32 - FRACBITS));  /* convert to 0.16 */
}

/* Controller state tracking */
static uint32_t prev_buttons = 0;

/* Button-to-key mapping */
static const struct {
    uint32_t button;
    int key;
} button_map[] = {
    { PAD_DPAD_UP,      KEY_UPARROW },
    { PAD_DPAD_DOWN,    KEY_DOWNARROW },
    { PAD_DPAD_LEFT,    KEY_LEFTARROW },
    { PAD_DPAD_RIGHT,   KEY_RIGHTARROW },
    { PAD_FACE_A,       KEY_RCTRL },    /* Fire */
    { PAD_FACE_B,       ' ' },          /* Use / Open */
    { PAD_FACE_X,       KEY_WPNUP },   /* Next Weapon */
    { PAD_FACE_Y,       KEY_WPNDOWN }, /* Prev Weapon */
    { PAD_FACE_START,   KEY_ESCAPE },   /* Menu */
    { PAD_FACE_SELECT,  KEY_TAB },      /* Automap */
};

#define NUM_BUTTONS (sizeof(button_map) / sizeof(button_map[0]))

/* Configurable shoulder/trigger key codes (settable via default.cfg) */
int key_l1 = ',';    /* L1: strafe left */
int key_r1 = '.';    /* R1: strafe right */
int key_l2 = KEY_WPNDOWN;  /* L2: previous weapon */
int key_r2 = KEY_WPNUP;   /* R2: next weapon */

/* gamekeydown: Doom's key state array — used to check if always-run
 * key is still held (G_DoLoadLevel clears it on every level load). */
extern boolean gamekeydown[];


/* Virtual button states for shoulder/trigger buttons */
static int l1_button_down = 0;
static int r1_button_down = 0;
static int l2_button_down = 0;
static int r2_button_down = 0;
static int menu_back_down = 0;
static int menu_enter_down = 0;

extern void R_InitSkyMap(void);
extern void ST_createWidgets(void);
extern boolean setsizeneeded;
extern gamestate_t gamestate;

void I_StartFrame(void)
{
    /* Use the vsync dead time for OPL music register writes.
     * OPL_AdvanceMusic() parses MUS events and does bus-stalling
     * OPL MMIO writes (~27us each, up to ~4ms on dense beats).
     * Running this here keeps those stalls off the rendering
     * critical path — they overlap with the vsync wait instead
     * of adding to frame time after I_FinishUpdate. */
    OPL_AdvanceMusic();

    /* Wait for any pending framebuffer swap to complete before the
     * game loop draws the next frame.  This ensures the CPU never
     * writes into the buffer that the FPGA is still scanning out
     * (which causes flickering on menus/status bar).
     * Measure wait time for VRR — long wait = headroom to spare. */
    uint32_t wait_start = SYS_CYCLE_LO;
    while (SYS_FB_SWAP)
        ;
    uint32_t vsync_wait = SYS_CYCLE_LO - wait_start;

    /* Advance the vsync-locked tic accumulator.
     *
     * The vsync period is crystal-stable but the cycle-counter reading
     * has polling jitter (a few cycles).  Use an exponential moving
     * average (α = 1/16) to lock onto the true period.  This makes
     * the tic pattern deterministic — tic boundaries never wobble
     * between frames due to measurement noise.
     *
     * Missed-vsync frames (dt ≈ 2× normal) are detected and the
     * accumulator is advanced by the correct number of periods so
     * the game maintains correct speed. */
    {
        uint32_t now_cy = SYS_CYCLE_LO;
        if (last_vsync_cycles) {
            uint32_t raw_dt = now_cy - last_vsync_cycles;
            if (!smooth_dt) {
                /* First frame: seed the filter */
                smooth_dt = raw_dt;
                tic_accum += ((uint64_t)smooth_dt * TIC_RECIP_48 >> 16);
            } else if (raw_dt < smooth_dt * 3 / 2) {
                /* Normal frame: update EMA (α = 1/16 for stability),
                 * advance accumulator by one smoothed period. */
                smooth_dt += ((int32_t)raw_dt - (int32_t)smooth_dt) / 16;
                tic_accum += ((uint64_t)smooth_dt * TIC_RECIP_48 >> 16);
            } else {
                /* Missed vsync: don't pollute the filter, but advance
                 * the accumulator by the correct number of periods so
                 * the game maintains correct speed in heavy areas. */
                uint32_t periods = (raw_dt + smooth_dt / 2) / smooth_dt;
                if (periods > 4) periods = 4;  /* safety cap */
                tic_accum += (uint64_t)(smooth_dt * periods) * TIC_RECIP_48 >> 16;
            }
        }
        last_vsync_cycles = now_cy;
    }

    /* Poll Original Mode setting — allows live switching from Core Options */
    int new_mode = (SYS_ORIGINAL_MODE != 0) ? 1 : 0;
    if (new_mode != original_mode) {
        original_mode = new_mode;
        setsizeneeded = true;   /* recalculate viewheight, viewwindowy, etc. */
        R_InitSkyMap();         /* recalculate skytexturemid */

        /* Recreate status bar widgets with new Y positions (safe — no lump access) */
        if (gamestate == GS_LEVEL)
            ST_createWidgets();

        /* Clear both framebuffers to black so the black bars are clean */
        memset((void *)(0x50000000 + 0x000000), 0, FB_STRIDE * FB_LINES);
        memset((void *)(0x50000000 + 0x100000), 0, FB_STRIDE * FB_LINES);
    }

    /* Poll Run Mode setting */
    run_mode = SYS_RUN_MODE;

    /* VRR: compute V_TOTAL directly from smoothed render time.
     * Instead of incremental ±1 adjustments (which oscillate and jitter),
     * measure how long rendering actually takes, smooth it with a fast
     * EMA (α = 1/8), and compute the exact V_TOTAL that gives ~10%
     * headroom above the render time.
     * Hard clamp to [VRR_V_MIN, VRR_V_MAX] (60–47 Hz). */
    {
        static uint32_t smooth_render = 0;
        static uint32_t vrr_v = 0;

        if (SYS_REFRESH_RATE == 3 && smooth_dt > 0) {
            /* Approximate render time = frame period minus vsync wait.
             * On missed frames (wait=0), this equals the full frame time. */
            uint32_t render_time = smooth_dt - (vsync_wait < smooth_dt ? vsync_wait : 0);

            /* Asymmetric EMA: fast attack (α = 1/4) when load increases,
             * slow decay (α = 1/16) when load decreases.  This reacts
             * quickly to heavy frames (avoids missed vsyncs) but recovers
             * gradually (avoids jitter when load fluctuates). */
            if (!smooth_render)
                smooth_render = render_time;
            else if (render_time > smooth_render)
                smooth_render = render_time;  /* instant drop Hz — never miss a vsync */
            else
                smooth_render += ((int32_t)render_time - (int32_t)smooth_render) / 64;  /* ramp back to high Hz very slowly */

            /* Derive cycles-per-video-line from measured vsync period */
            if (!vrr_v || vrr_v < VRR_V_MIN || vrr_v > VRR_V_MAX)
                vrr_v = 262;
            uint32_t cycles_per_line = smooth_dt / vrr_v;

            if (cycles_per_line > 0) {
                /* Desired frame period = render time + 10% headroom */
                uint32_t desired = smooth_render + smooth_render / 10;
                vrr_v = desired / cycles_per_line;

                /* Clamp to safe scaler range */
                if (vrr_v < VRR_V_MIN) vrr_v = VRR_V_MIN;
                if (vrr_v > VRR_V_MAX) vrr_v = VRR_V_MAX;
            }

            SYS_V_TOTAL = vrr_v;
        } else {
            /* Not in VRR mode — reset state so re-entering VRR is clean */
            smooth_render = 0;
            vrr_v = 0;
        }
    }

    /* Top up the audio ring buffer at the start of each frame too,
     * so it doesn't drain during heavy rendering.  The deficit check
     * inside I_UpdateSound self-regulates — no double-mixing risk. */
    I_UpdateSound();
}

void I_StartTic(void)
{
    /* Shutdown handshake: bridge reads CRAM1 directly, just ack and halt */
    if (SYS_SHUTDOWN & 1) {
        SYS_SHUTDOWN = 1;  /* ack */
        while (1) {}        /* halt — bridge will reset us */
    }

    event_t event;

    /* Run mode handling */
    if (run_mode == RUN_MODE_ALWAYS) {
        /* Always-run: keep KEY_RSHIFT held permanently.
         * Must re-send every tic because G_DoLoadLevel() clears gamekeydown
         * on every level transition, dropping the key state. */
        if (!gamekeydown[KEY_RSHIFT]) {
            event.type = ev_keydown;
            event.data1 = KEY_RSHIFT;
            event.data2 = 0;
            event.data3 = 0;
            D_PostEvent(&event);
        }
    } else if (run_mode == RUN_MODE_USE_RUN) {
        /* Use button also triggers run: inject KEY_RSHIFT when Use (B) is held */
        uint32_t cur_buttons = SYS_CONT1_KEY;
        int use_held = (cur_buttons & PAD_FACE_B) != 0;
        if (use_held && !gamekeydown[KEY_RSHIFT]) {
            event.type = ev_keydown;
            event.data1 = KEY_RSHIFT;
            event.data2 = 0;
            event.data3 = 0;
            D_PostEvent(&event);
        } else if (!use_held && gamekeydown[KEY_RSHIFT]) {
            event.type = ev_keyup;
            event.data1 = KEY_RSHIFT;
            event.data2 = 0;
            event.data3 = 0;
            D_PostEvent(&event);
        }
    } else if (run_mode == RUN_MODE_DOUBLETAP) {
        /* Double-tap forward to run: track D-pad Up timing */
        static uint32_t last_up_press_time = 0;
        static int doubletap_running = 0;
        uint32_t cur_buttons = SYS_CONT1_KEY;
        int up_now = (cur_buttons & PAD_DPAD_UP) != 0;
        int up_prev = (prev_buttons & PAD_DPAD_UP) != 0;

        if (up_now && !up_prev) {
            /* Rising edge: check if double-tap */
            uint32_t now = SYS_CYCLE_LO;
            uint32_t delta = now - last_up_press_time;
            /* ~300ms window at 100MHz = 30,000,000 cycles */
            if (delta < 30000000 && last_up_press_time != 0) {
                doubletap_running = 1;
            }
            last_up_press_time = now;
        }

        if (!up_now) {
            doubletap_running = 0;
        }

        if (doubletap_running && !gamekeydown[KEY_RSHIFT]) {
            event.type = ev_keydown;
            event.data1 = KEY_RSHIFT;
            event.data2 = 0;
            event.data3 = 0;
            D_PostEvent(&event);
        } else if (!doubletap_running && gamekeydown[KEY_RSHIFT]) {
            event.type = ev_keyup;
            event.data1 = KEY_RSHIFT;
            event.data2 = 0;
            event.data3 = 0;
            D_PostEvent(&event);
        }
    }

    uint32_t buttons = SYS_CONT1_KEY;
    uint32_t changed = buttons ^ prev_buttons;
    uint32_t trig = SYS_CONT1_TRIG;
    int cur_l1 = (buttons & PAD_TRIG_L1) != 0;
    int cur_r1 = (buttons & PAD_TRIG_R1) != 0;
    int cur_l2 = (buttons & PAD_TRIG_L2) != 0 || ((trig & 0xFFu) >= TRIG_PRESS_THRESHOLD);
    int cur_r2 = (buttons & PAD_TRIG_R2) != 0 || (((trig >> 8) & 0xFFu) >= TRIG_PRESS_THRESHOLD);

    for (unsigned i = 0; i < NUM_BUTTONS; i++) {
        uint32_t mask = button_map[i].button;
        if (changed & mask) {
            int k = button_map[i].key;
            int pressed = (buttons & mask) != 0;
            if ((k == KEY_WPNUP || k == KEY_WPNDOWN) && !menuactive) {
                /* Weapon cycle: set pendingweapon directly.
                 * Bypasses ticcmd 3-bit encoding (can't handle weapon 8)
                 * and Doom 2's shotgun→super-shotgun redirect. */
                if (pressed) {
                    player_t *p = &players[consoleplayer];
                    int cur = (int)p->readyweapon;
                    int dir = (k == KEY_WPNUP) ? 1 : -1;
                    int next;
                    for (next = cur + dir; next != cur; next += dir) {
                        if (next >= NUMWEAPONS) next = 0;
                        if (next < 0) next = NUMWEAPONS - 1;
                        if (p->weaponowned[next])
                            break;
                    }
                    p->pendingweapon = next;
                }
            } else {
                event.type = pressed ? ev_keydown : ev_keyup;
                event.data1 = k;
                event.data2 = 0;
                event.data3 = 0;
                D_PostEvent(&event);
            }
        }
    }

    /* In menus: any right shoulder/trigger = enter, any left = back */
    if (menuactive) {
        int enter_down = (cur_r1 || cur_r2);
        if (enter_down != menu_enter_down) {
            event.type = enter_down ? ev_keydown : ev_keyup;
            event.data1 = KEY_ENTER;
            event.data2 = 0;
            event.data3 = 0;
            D_PostEvent(&event);
            menu_enter_down = enter_down;
        }
        int back_down = (cur_l1 || cur_l2);
        if (back_down != menu_back_down) {
            event.type = back_down ? ev_keydown : ev_keyup;
            event.data1 = KEY_BACKSPACE;
            event.data2 = 0;
            event.data3 = 0;
            D_PostEvent(&event);
            menu_back_down = back_down;
        }
    } else {
        /* In gameplay: each button sends its configurable key code */
        struct { int cur; int *prev; int key; } trigs[] = {
            { cur_l1, &l1_button_down, key_l1 },
            { cur_r1, &r1_button_down, key_r1 },
            { cur_l2, &l2_button_down, key_l2 },
            { cur_r2, &r2_button_down, key_r2 },
        };
        for (int i = 0; i < 4; i++) {
            if (trigs[i].cur != *trigs[i].prev) {
                int k = trigs[i].key;
                if (k == KEY_WPNUP || k == KEY_WPNDOWN) {
                    /* Weapon cycle: set pendingweapon directly */
                    if (trigs[i].cur) {
                        player_t *p = &players[consoleplayer];
                        int cur = (int)p->readyweapon;
                        int dir = (k == KEY_WPNUP) ? 1 : -1;
                        int next;
                        for (next = cur + dir; next != cur; next += dir) {
                            if (next >= NUMWEAPONS) next = 0;
                            if (next < 0) next = NUMWEAPONS - 1;
                            if (p->weaponowned[next])
                                break;
                        }
                        p->pendingweapon = next;
                    }
                } else if (k != 0) {
                    event.type = trigs[i].cur ? ev_keydown : ev_keyup;
                    event.data1 = k;
                    event.data2 = 0;
                    event.data3 = 0;
                    D_PostEvent(&event);
                }
            }
            *trigs[i].prev = trigs[i].cur;
        }
    }

    prev_buttons = buttons;
}

ticcmd_t *I_BaseTiccmd(void)
{
    static ticcmd_t emptycmd;
    return &emptycmd;
}

void I_Quit(void)
{
    D_QuitNetGame();
    M_SaveDefaults();
    I_ShutdownGraphics();
    exit(0);
}

byte *I_AllocLow(int length)
{
    byte *mem = (byte *)malloc(length);
    if (mem)
        memset(mem, 0, length);
    return mem;
}

void I_Tactile(int on, int off, int total)
{
    on = off = total = 0;
}

void I_Error(char *error, ...)
{
    va_list argptr;

    /* Switch back to terminal so error is visible */
    SYS_DISPLAY_MODE = 0;

    va_start(argptr, error);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, error, argptr);
    fprintf(stderr, "\n");
    va_end(argptr);

    fflush(stderr);

    if (demorecording)
        G_CheckDemoStatus();

    D_QuitNetGame();
    I_ShutdownGraphics();
    exit(-1);
}
