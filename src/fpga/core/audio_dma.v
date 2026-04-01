//
// Audio DMA Engine for PocketDoom — Pull Mode
//
// Watches the audio_upsample ring buffer level. When it drops below a
// threshold and the CPU has provided data, the DMA pulls samples from a
// circular SDRAM buffer and writes them into the ring buffer.
//
// The CPU mixes audio into SDRAM (via uncached alias) and advances WR_PTR.
// The DMA advances RD_PTR as it consumes.  An IRQ fires when the available
// data (WR_PTR − RD_PTR) falls below a configurable threshold so the CPU
// can mix more.
//
// MMIO register map (0x4F00_0000):
//   0x00  CTRL       [0]=enable  [1]=irq_en
//   0x04  STATUS     [0]=active  [1]=irq_pending (W1C)
//   0x08  BASE_ADDR  SDRAM byte address (aligned to buffer size)
//   0x0C  BUF_MASK   (power-of-2 size − 1) for index wrapping
//   0x10  WR_PTR     Written by CPU after filling samples (word index)
//   0x14  RD_PTR     Read by CPU; advanced by DMA (word index)
//   0x18  THRESHOLD  IRQ when available <= threshold
//

`default_nettype none

module audio_dma (
    input  wire        clk,
    input  wire        reset_n,

    // MMIO register interface (from axi_periph_slave)
    input  wire        reg_wr,
    input  wire [4:0]  reg_addr,       // Word address (byte offset >> 2)
    input  wire [31:0] reg_wdata,
    output reg  [31:0] reg_rdata,

    // AXI4 read master (to SDRAM arbiter)
    output reg         m_axi_arvalid,
    input  wire        m_axi_arready,
    output reg  [31:0] m_axi_araddr,
    output reg  [7:0]  m_axi_arlen,

    input  wire        m_axi_rvalid,
    input  wire [31:0] m_axi_rdata,
    input  wire        m_axi_rlast,

    // Write interface to audio_upsample ring buffer
    output reg         sample_wr,
    output reg  [31:0] sample_data,
    input  wire [9:0]  buf_level,

    // Interrupt output (active high, level-triggered)
    output wire        irq
);

// ============================================
// MMIO Registers
// ============================================
reg [31:0] ctrl_reg;       // 0x00
reg [31:0] status_reg;     // 0x04
reg [31:0] base_addr_reg;  // 0x08
reg [31:0] buf_mask_reg;   // 0x0C
reg [31:0] wr_ptr_reg;     // 0x10  (CPU writes)
reg [31:0] rd_ptr_reg;     // 0x14  (DMA advances)
reg [31:0] threshold_reg;  // 0x18

wire ctrl_enable = ctrl_reg[0];
wire ctrl_irq_en = ctrl_reg[1];

// Available samples: how much the CPU has written ahead of the DMA
wire [31:0] available = wr_ptr_reg - rd_ptr_reg;

// IRQ: fire when available data is at or below threshold
assign irq = status_reg[1] & ctrl_irq_en;

// ============================================
// State machine
// ============================================
localparam S_IDLE    = 3'd0;
localparam S_POLL    = 3'd1;
localparam S_AXI_AR  = 3'd2;
localparam S_AXI_R   = 3'd3;

reg [2:0] state;
reg [7:0] burst_len;
reg       irq_armed;     // Edge detect: fire IRQ once, re-arm when refilled

// Ring buffer room: don't overflow the 512-entry upsampler buffer
localparam [9:0] RING_ROOM_THRESHOLD = 10'd496;  // 512 - 16
localparam [7:0] MAX_BURST = 8'd15;               // 16-word AXI bursts

wire reset = ~reset_n;

// ============================================
// Register read mux
// ============================================
always @(*) begin
    case (reg_addr[2:0])
        3'd0: reg_rdata = ctrl_reg;
        3'd1: reg_rdata = status_reg;
        3'd2: reg_rdata = base_addr_reg;
        3'd3: reg_rdata = buf_mask_reg;
        3'd4: reg_rdata = wr_ptr_reg;
        3'd5: reg_rdata = rd_ptr_reg;
        3'd6: reg_rdata = threshold_reg;
        default: reg_rdata = 32'h0;
    endcase
end

// ============================================
// Main FSM + register writes
// ============================================
always @(posedge clk or posedge reset) begin
    if (reset) begin
        state          <= S_IDLE;
        ctrl_reg       <= 32'h0;
        status_reg     <= 32'h0;
        base_addr_reg  <= 32'h0;
        buf_mask_reg   <= 32'h0;
        wr_ptr_reg     <= 32'h0;
        rd_ptr_reg     <= 32'h0;
        threshold_reg  <= 32'h0;
        burst_len      <= 8'h0;
        irq_armed      <= 1'b1;
        m_axi_arvalid  <= 1'b0;
        m_axi_araddr   <= 32'h0;
        m_axi_arlen    <= 8'h0;
        sample_wr      <= 1'b0;
        sample_data    <= 32'h0;
    end else begin
        // Default: deassert single-cycle strobes
        sample_wr <= 1'b0;

        // ============================================
        // MMIO register writes (non-STATUS, active in all states)
        // ============================================
        if (reg_wr) begin
            case (reg_addr[2:0])
                3'd0: ctrl_reg  <= reg_wdata;
                // STATUS W1C is handled AFTER the FSM (see below)
                3'd2: base_addr_reg <= {reg_wdata[31:2], 2'b00};
                3'd3: buf_mask_reg  <= reg_wdata;
                3'd4: wr_ptr_reg    <= reg_wdata;
                // 3'd5: rd_ptr_reg is DMA-driven (read-only to CPU)
                3'd6: threshold_reg <= reg_wdata;
            endcase
        end

        // ============================================
        // State machine
        // ============================================
        case (state)

        S_IDLE: begin
            m_axi_arvalid  <= 1'b0;
            status_reg[0]  <= 1'b0;
            rd_ptr_reg     <= 32'd0;
            irq_armed      <= 1'b1;
            if (ctrl_enable)
                state <= S_POLL;
        end

        S_POLL: begin
            if (!ctrl_enable) begin
                state <= S_IDLE;
            end else begin
                status_reg[0] <= 1'b1;  // active

                // Edge-detected IRQ: fire once when available drops to threshold,
                // re-arm only after CPU refills past the threshold.
                if (available <= threshold_reg) begin
                    if (irq_armed && ctrl_irq_en) begin
                        status_reg[1] <= 1'b1;  // irq_pending (fire once)
                        irq_armed <= 1'b0;       // disarm until refilled
                    end
                end else begin
                    irq_armed <= 1'b1;            // re-arm: CPU has refilled
                end

                // Pull condition: ring buffer has room AND SDRAM has data
                if (buf_level <= RING_ROOM_THRESHOLD && available != 32'd0) begin
                    // Calculate burst: min(available, 16, words_before_wrap)
                    // Prevent burst from crossing the circular buffer boundary.
                    begin : burst_calc
                        reg [31:0] idx;
                        reg [31:0] before_wrap;
                        reg [7:0]  bl;
                        idx = rd_ptr_reg & buf_mask_reg;
                        before_wrap = (buf_mask_reg + 32'd1) - idx;  // words until end of buffer
                        bl = MAX_BURST;
                        if (available - 32'd1 < {24'd0, bl})
                            bl = available[7:0] - 8'd1;
                        if (before_wrap - 32'd1 < {24'd0, bl})
                            bl = before_wrap[7:0] - 8'd1;
                        burst_len <= bl;
                    end
                    state <= S_AXI_AR;
                end
            end
        end

        S_AXI_AR: begin
            if (!ctrl_enable) begin
                m_axi_arvalid <= 1'b0;
                state <= S_IDLE;
            end else begin
                m_axi_arvalid <= 1'b1;
                m_axi_araddr  <= base_addr_reg +
                                 {(rd_ptr_reg & buf_mask_reg), 2'b00};
                m_axi_arlen   <= burst_len;
                if (m_axi_arready && m_axi_arvalid) begin
                    m_axi_arvalid <= 1'b0;
                    state <= S_AXI_R;
                end
            end
        end

        S_AXI_R: begin
            if (m_axi_rvalid) begin
                sample_wr   <= 1'b1;
                sample_data <= m_axi_rdata;
                rd_ptr_reg  <= rd_ptr_reg + 32'd1;
                if (m_axi_rlast)
                    state <= S_POLL;
            end
        end

        endcase

        // ============================================
        // STATUS W1C — AFTER the FSM so CPU clear always wins
        // (last non-blocking assignment takes priority in Verilog)
        // ============================================
        if (reg_wr && reg_addr[2:0] == 3'd1 && reg_wdata[1])
            status_reg[1] <= 1'b0;
    end
end

endmodule
