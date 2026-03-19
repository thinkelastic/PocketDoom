//
// OPL3 hardware wrapper for PocketDoom
// - Wraps Greg Taylor's opl3_fpga (YMF262, bit-true reverse-engineered)
// - Provides the same CPU MMIO interface as the old opl2_wrapper
// - OPL3 runs in OPL2 compatibility mode (bank 0 only)
// - Synthesis clock: 12.288 MHz -> 48 kHz sample rate (exact, no resampling)
// - 24-bit stereo output scaled to 16-bit signed for mixer
//

`default_nettype none

module opl3_wrapper (
    input  wire        clk,            // 100 MHz (CPU/system clock)
    input  wire        clk_opl,        // 12.288 MHz (OPL3 synthesis clock)
    input  wire        reset_n,
    // CPU MMIO interface (identical to opl2_wrapper)
    input  wire        opl_write_req,  // pulse from cpu_system
    input  wire [1:0]  opl_write_addr, // {bank, addr/data}
    input  wire [7:0]  opl_write_data,
    output reg         opl_ack,        // delayed ACK (stalls CPU until ready)
    // Audio output (clk_opl domain)
    output reg  signed [15:0] opl_audio_out,
    output reg                opl_sample_toggle
);

// ============================================
// Write timing state machine (clk domain)
// ============================================
// host_if needs cs_n+wr_n held low for >=2 clk_host cycles for its
// pipeline to detect the write edge and push into the async FIFO.
// We hold for 4 cycles for margin, then wait the standard OPL timing.

localparam IDLE    = 2'd0;
localparam STROBE  = 2'd1;
localparam WAIT    = 2'd2;

reg  [1:0]  state;
reg  [11:0] wait_cnt;
reg  [11:0] wait_target;
reg  [2:0]  strobe_cnt;

// Bus signals to OPL3 host_if (active-low, clk domain)
reg        opl3_cs_n;
reg        opl3_wr_n;
reg  [1:0] opl3_address;
reg  [7:0] opl3_din;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        state       <= IDLE;
        wait_cnt    <= 12'd0;
        wait_target <= 12'd0;
        strobe_cnt  <= 3'd0;
        opl_ack     <= 1'b0;
        opl3_cs_n   <= 1'b1;
        opl3_wr_n   <= 1'b1;
        opl3_address <= 2'd0;
        opl3_din     <= 8'd0;
    end else begin
        opl_ack <= 1'b0;

        case (state)
        IDLE: begin
            opl3_cs_n <= 1'b1;
            opl3_wr_n <= 1'b1;
            if (opl_write_req) begin
                // Pass 2-bit address directly: {bank, addr/data}
                opl3_address <= opl_write_addr;
                opl3_din     <= opl_write_data;
                opl3_cs_n    <= 1'b0;
                opl3_wr_n    <= 1'b0;
                // ~4 us after addr write, ~24 us after data write
                wait_target  <= opl_write_addr[0] ? 12'd2400 : 12'd400;
                strobe_cnt   <= 3'd0;
                state        <= STROBE;
            end
        end

        STROBE: begin
            // Hold cs_n/wr_n low for 4 clk cycles so host_if pipeline
            // reliably detects the write edge and captures address+data.
            if (strobe_cnt >= 3'd3) begin
                opl3_cs_n <= 1'b1;
                opl3_wr_n <= 1'b1;
                wait_cnt  <= 12'd0;
                state     <= WAIT;
            end else begin
                strobe_cnt <= strobe_cnt + 3'd1;
            end
        end

        WAIT: begin
            if (wait_cnt >= wait_target) begin
                opl_ack <= 1'b1;
                state   <= IDLE;
            end else begin
                wait_cnt <= wait_cnt + 12'd1;
            end
        end

        default: state <= IDLE;
        endcase
    end
end

// ============================================
// OPL3 core instance
// ============================================
wire signed [23:0] sample_l, sample_r;
wire               sample_valid;
wire        [7:0]  opl3_dout;
wire        [3:0]  opl3_led;
wire               opl3_irq_n;

opl3 opl3_core (
    .clk        (clk_opl),       // 12.288 MHz — ÷256 = exactly 48 kHz (matches I2S)
    .clk_host   (clk),           // 100 MHz for register writes
    .clk_dac    (clk_opl),       // unused (INSTANTIATE_SAMPLE_SYNC_TO_DAC_CLK=0)
    .ic_n       (reset_n),
    .cs_n       (opl3_cs_n),
    .rd_n       (1'b1),          // never read status
    .wr_n       (opl3_wr_n),
    .address    (opl3_address),
    .din        (opl3_din),
    .dout       (opl3_dout),
    .sample_valid (sample_valid),
    .sample_l   (sample_l),
    .sample_r   (sample_r),
    .led        (opl3_led),
    .irq_n      (opl3_irq_n)
);

// ============================================
// Audio output latch (clk_opl domain)
// ============================================
// OPL3 outputs 24-bit signed stereo at 48 kHz on clk_opl.
// Same clock domain as audio_output's clk_audio — no CDC needed.
always @(posedge clk_opl or negedge reset_n) begin
    if (!reset_n) begin
        opl_audio_out     <= 16'sd0;
        opl_sample_toggle <= 1'b0;
    end else if (sample_valid) begin
        opl_audio_out     <= sample_l >>> 8;
        opl_sample_toggle <= ~opl_sample_toggle;
    end
end

endmodule
