/*
 * Trap handler for RISC-V
 * Handles misaligned access emulation and external interrupts (audio DMA)
 */

#include "terminal.h"
/* Debug variables (defined in main.c) */
extern volatile unsigned int pd_dbg_stage;
extern volatile unsigned int pd_dbg_info;

/* Audio DMA ISR (defined in doom_sound.c) */
extern void audio_dma_isr(void);

/* Music timer ISR (defined in doom_music.c) */
extern void music_timer_isr(void);

/* Trap frame layout (matches start.S) */
typedef struct {
    unsigned int regs[32];   /* x0-x31 (x0 always 0) at offset 0 */
    unsigned int mepc;       /* at offset 128 */
    unsigned int mcause;     /* at offset 132 */
    unsigned int mtval;      /* at offset 136 */
    unsigned int fregs[32];  /* f0-f31 at offset 140 */
} trap_frame_t;

/* RISC-V instruction encodings */
#define OPCODE_LOAD   0x03
#define OPCODE_STORE  0x23
#define OPCODE_FLW    0x07  /* Float load (I-type, funct3=010) */
#define OPCODE_FSW    0x27  /* Float store (S-type, funct3=010) */

#define FUNCT3_LB     0x0
#define FUNCT3_LH     0x1
#define FUNCT3_LW     0x2
#define FUNCT3_LBU    0x4
#define FUNCT3_LHU    0x5

#define FUNCT3_SB     0x0
#define FUNCT3_SH     0x1
#define FUNCT3_SW     0x2

/* mcause values */
#define CAUSE_LOAD_MISALIGNED   4
#define CAUSE_STORE_MISALIGNED  6

/* Valid memory regions for emulation */
#define BRAM_START      0x00000000
#define BRAM_END        0x00010000
#define SDRAM_START     0x10000000
#define SDRAM_END       0x14000000
#define PSRAM_START     0x30000000
#define PSRAM_END       0x38000000
#define SDRAM_UC_START  0x50000000  /* Uncached SDRAM alias */
#define SDRAM_UC_END    0x54000000

/* Check if address range is in valid memory */
__attribute__((section(".text.boot")))
static int addr_valid(unsigned int addr, unsigned int len) {
    unsigned int end = addr + len - 1;
    /* Check for overflow */
    if (end < addr) return 0;
    /* BRAM */
    if (addr >= BRAM_START && end < BRAM_END) return 1;
    /* SDRAM (cached) */
    if (addr >= SDRAM_START && end < SDRAM_END) return 1;
    /* PSRAM */
    if (addr >= PSRAM_START && end < PSRAM_END) return 1;
    /* SDRAM (uncached alias — used for PAK data) */
    if (addr >= SDRAM_UC_START && end < SDRAM_UC_END) return 1;
    return 0;
}

/* Read byte from memory */
__attribute__((section(".text.boot")))
static inline unsigned char read_byte(unsigned int addr) {
    return *(volatile unsigned char *)addr;
}

/* Write byte to memory */
__attribute__((section(".text.boot")))
static inline void write_byte(unsigned int addr, unsigned char val) {
    *(volatile unsigned char *)addr = val;
}

/* Emulate misaligned load */
__attribute__((section(".text.boot")))
static unsigned int emulate_load(unsigned int addr, int funct3) {
    unsigned int val = 0;

    switch (funct3) {
    case FUNCT3_LH:  /* Load halfword (signed) */
        val = read_byte(addr) | (read_byte(addr + 1) << 8);
        val = (int)(signed short)val;
        break;

    case FUNCT3_LHU: /* Load halfword (unsigned) */
        val = read_byte(addr) | (read_byte(addr + 1) << 8);
        break;

    case FUNCT3_LW:  /* Load word */
        val = read_byte(addr) |
              (read_byte(addr + 1) << 8) |
              (read_byte(addr + 2) << 16) |
              (read_byte(addr + 3) << 24);
        break;
    }

    return val;
}

/* Emulate misaligned store */
__attribute__((section(".text.boot")))
static void emulate_store(unsigned int addr, unsigned int val, int funct3) {
    switch (funct3) {
    case FUNCT3_SH:  /* Store halfword */
        write_byte(addr, val & 0xFF);
        write_byte(addr + 1, (val >> 8) & 0xFF);
        break;

    case FUNCT3_SW:  /* Store word */
        write_byte(addr, val & 0xFF);
        write_byte(addr + 1, (val >> 8) & 0xFF);
        write_byte(addr + 2, (val >> 16) & 0xFF);
        write_byte(addr + 3, (val >> 24) & 0xFF);
        break;
    }
}

/* Debug counter for misaligned traps */
static unsigned int misaligned_count = 0;

/* Decode and handle misaligned access
 * Returns 1 if handled, 0 if should trap normally */
__attribute__((section(".text.boot")))
int handle_misaligned(trap_frame_t *frame) {
    unsigned int mcause = frame->mcause;

    /* Interrupts: MSB of mcause is set */
    if (mcause & 0x80000000) {
        unsigned int code = mcause & 0x7FFFFFFF;
        if (code == 11) {  /* Machine external interrupt */
            audio_dma_isr();
            return 1;
        }
        if (code == 7) {   /* Machine timer interrupt */
            music_timer_isr();
            return 1;
        }
        return 0;  /* Unhandled interrupt → fatal */
    }

    /* Only handle misaligned load/store traps */
    if (mcause != CAUSE_LOAD_MISALIGNED && mcause != CAUSE_STORE_MISALIGNED)
        return 0;

    unsigned int instr = *(unsigned int *)frame->mepc;
    unsigned int opcode = instr & 0x7F;
    unsigned int funct3 = (instr >> 12) & 0x7;
    unsigned int rd = (instr >> 7) & 0x1F;
    unsigned int rs1 = (instr >> 15) & 0x1F;
    unsigned int rs2 = (instr >> 20) & 0x1F;
    int imm;
    unsigned int addr;

    /* Debug: print first few traps */
    misaligned_count++;
    if (misaligned_count <= 5) {
        term_printf("T#%d mc=%x pc=%x i=%x\n",
                    misaligned_count, mcause, frame->mepc, instr);
    }

    /* Handle based on opcode (trust the instruction, not just mcause) */
    if (opcode == OPCODE_LOAD) {
        /* I-type immediate: instr[31:20] sign-extended */
        imm = ((int)instr) >> 20;
        addr = frame->regs[rs1] + imm;

        /* Validate address before accessing */
        unsigned int access_len = (funct3 == FUNCT3_LW) ? 4 :
                                  (funct3 == FUNCT3_LH || funct3 == FUNCT3_LHU) ? 2 : 1;
        if (!addr_valid(addr, access_len))
            return 0;

        /* Emulate the load */
        unsigned int val = emulate_load(addr, funct3);

        /* Write to destination register (rd=0 is hardwired to 0, ignore) */
        if (rd != 0) {
            frame->regs[rd] = val;
        }

        /* Advance PC past the instruction */
        frame->mepc += 4;
        return 1;
    }

    if (opcode == OPCODE_STORE) {
        /* S-type immediate: {instr[31:25], instr[11:7]} sign-extended */
        imm = ((instr >> 7) & 0x1F) | (((int)instr >> 20) & 0xFFFFFFE0);
        addr = frame->regs[rs1] + imm;

        /* Validate address before accessing */
        unsigned int access_len = (funct3 == FUNCT3_SW) ? 4 :
                                  (funct3 == FUNCT3_SH) ? 2 : 1;
        if (!addr_valid(addr, access_len))
            return 0;

        /* Get value from source register */
        unsigned int val = frame->regs[rs2];

        /* Emulate the store */
        emulate_store(addr, val, funct3);

        /* Advance PC past the instruction */
        frame->mepc += 4;
        return 1;
    }

    /* FLW: float load word (I-type, opcode 0x07, funct3=010) */
    if (opcode == OPCODE_FLW) {
        /* I-type immediate: instr[31:20] sign-extended */
        imm = ((int)instr) >> 20;
        addr = frame->regs[rs1] + imm;

        if (!addr_valid(addr, 4))
            return 0;

        /* Emulate 4-byte load using byte reads */
        unsigned int val = emulate_load(addr, FUNCT3_LW);

        /* Write raw bits to float register in trap frame */
        frame->fregs[rd] = val;

        frame->mepc += 4;
        return 1;
    }

    /* FSW: float store word (S-type, opcode 0x27, funct3=010) */
    if (opcode == OPCODE_FSW) {
        /* S-type immediate: {instr[31:25], instr[11:7]} sign-extended */
        imm = ((instr >> 7) & 0x1F) | (((int)instr >> 20) & 0xFFFFFFE0);
        addr = frame->regs[rs1] + imm;

        if (!addr_valid(addr, 4))
            return 0;

        /* Read raw bits from float register in trap frame */
        unsigned int val = frame->fregs[rs2];

        /* Emulate 4-byte store using byte writes */
        emulate_store(addr, val, FUNCT3_SW);

        frame->mepc += 4;
        return 1;
    }

    /* Not a load/store instruction - can't handle */
    return 0;
}

/* Fatal trap handler - called when we can't handle the exception */
__attribute__((section(".text.boot")))
void fatal_trap(trap_frame_t *frame) {
    /* Ensure terminal is visible for fatal diagnostics. */
    (*(volatile unsigned int *)0x4000000C) = 0;

    /* term_printf can itself trap (misaligned access), so snapshot first.
     * Nested traps reuse the same trap-frame slot at top of BRAM stack. */
    trap_frame_t snap = *frame;
    unsigned int dbg_stage = pd_dbg_stage;
    unsigned int dbg_info = pd_dbg_info;
    unsigned int handled = misaligned_count;

    term_printf("\n!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    term_printf("!!! CPU TRAP OCCURRED !!!\n");
    term_printf("!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    term_printf("mcause: 0x%08x\n", snap.mcause);
    term_printf("mepc:   0x%08x\n", snap.mepc);
    term_printf("mtval:  0x%08x\n", snap.mtval);
    term_printf("sp:     0x%08x\n", snap.regs[2]);
    term_printf("ra:     0x%08x\n", snap.regs[1]);
    term_printf("dbg_stage: 0x%08x\n", dbg_stage);
    term_printf("dbg_info:  0x%08x\n", dbg_info);
    term_printf("traps handled: %d\n", handled);

    if (addr_valid(snap.mepc, 4)) {
        unsigned int instr = *(volatile unsigned int *)snap.mepc;
        term_printf("instr@mepc: 0x%08x\n", instr);
        if (snap.mepc < BRAM_END && snap.mepc >= 8) {
            unsigned int im1 = *(volatile unsigned int *)(snap.mepc - 4);
            unsigned int ip1 = *(volatile unsigned int *)(snap.mepc + 4);
            term_printf("instr-1:    0x%08x\n", im1);
            term_printf("instr+1:    0x%08x\n", ip1);
        }
    }

    term_printf("!!!!!!!!!!!!!!!!!!!!!!!!!\n");

    while (1) {}
}
