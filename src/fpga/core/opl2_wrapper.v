//
// OPL2 hardware wrapper for PocketDoom
// - Wraps jtopl2 (jotego) with bus-stalling MMIO interface
// - Divide-by-29 clock enable: 100.227 MHz / 29 = 3.456 MHz cen
//   -> OPL sample rate = 3.456 MHz / 72 = 48,002 Hz (matches I2S)
// - Write timing enforcement: 12 cen cycles after addr, 84 after data
// - CPU bus stalls until opl_ack asserts
//

`default_nettype none

module opl2_wrapper (
    input  wire        clk,            // 100 MHz
    input  wire        reset_n,
    // CPU MMIO interface
    input  wire        opl_write_req,  // pulse from cpu_system
    input  wire        opl_write_addr, // 0=addr register, 1=data register
    input  wire [7:0]  opl_write_data,
    output reg         opl_ack,        // delayed ACK (stalls CPU until ready)
    // Audio output
    output reg  signed [15:0] opl_audio_out,
    output reg                opl_sample_toggle  // toggles on each new sample (for CDC)
);

// ============================================
// Clock enable: divide-by-29 -> 100.227 MHz / 29 = 3.456 MHz
// OPL sample rate = 3.456 MHz / 72 = 48,002 Hz — matches the 48 kHz
// I2S output so no resampling is needed (eliminates aliasing artifacts).
// F-number table in firmware is scaled up by 49716/48002 = 1.0357 to
// compensate for the ~3.4% slower clock vs the standard 3.5795 MHz.
// ============================================
reg [4:0] cen_cnt;
reg       cen;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        cen_cnt <= 5'd0;
        cen     <= 1'b0;
    end else begin
        if (cen_cnt == 5'd28) begin
            cen_cnt <= 5'd0;
            cen     <= 1'b1;
        end else begin
            cen_cnt <= cen_cnt + 5'd1;
            cen     <= 1'b0;
        end
    end
end

// ============================================
// jtopl2 instance
// ============================================
wire signed [15:0] opl_snd;
wire               opl_sample;
wire        [7:0]  opl_dout;
wire               opl_irq_n;

// Active-high reset
wire opl_rst = ~reset_n;

// Write strobe control - directly driven by state machine
reg       opl_cs_n;
reg       opl_wr_n;
reg       opl_addr_reg;
reg [7:0] opl_din_reg;

jtopl2 opl_core (
    .rst    (opl_rst),
    .clk    (clk),
    .cen    (cen),
    .din    (opl_din_reg),
    .addr   (opl_addr_reg),
    .cs_n   (opl_cs_n),
    .wr_n   (opl_wr_n),
    .dout   (opl_dout),
    .irq_n  (opl_irq_n),
    .snd    (opl_snd),
    .sample (opl_sample)
);

// ============================================
// Write timing state machine
// ============================================
// After address write: wait 12 cen cycles (~3.4 us)
// After data write:    wait 84 cen cycles (~23.5 us)

localparam IDLE     = 2'd0;
localparam STROBE   = 2'd1;
localparam WAIT_CEN = 2'd2;

reg [1:0]  state;
reg [6:0]  wait_cnt;
reg [6:0]  wait_target;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        state       <= IDLE;
        wait_cnt    <= 7'd0;
        wait_target <= 7'd0;
        opl_ack     <= 1'b0;
        opl_cs_n    <= 1'b1;
        opl_wr_n    <= 1'b1;
        opl_addr_reg <= 1'b0;
        opl_din_reg  <= 8'd0;
    end else begin
        opl_ack <= 1'b0;

        case (state)
        IDLE: begin
            opl_cs_n <= 1'b1;
            opl_wr_n <= 1'b1;
            if (opl_write_req) begin
                // Latch inputs and begin write
                opl_addr_reg <= opl_write_addr;
                opl_din_reg  <= opl_write_data;
                opl_cs_n     <= 1'b0;
                opl_wr_n     <= 1'b0;
                wait_target  <= opl_write_addr ? 7'd83 : 7'd11;  // 84 or 12 cen cycles
                state        <= STROBE;
            end
        end

        STROBE: begin
            // Hold cs_n/wr_n low for one cen cycle, then release
            if (cen) begin
                opl_cs_n <= 1'b1;
                opl_wr_n <= 1'b1;
                wait_cnt <= 7'd0;
                state    <= WAIT_CEN;
            end
        end

        WAIT_CEN: begin
            if (cen) begin
                if (wait_cnt >= wait_target) begin
                    opl_ack <= 1'b1;
                    state   <= IDLE;
                end else begin
                    wait_cnt <= wait_cnt + 7'd1;
                end
            end
        end

        default: state <= IDLE;
        endcase
    end
end

// ============================================
// Audio output latch
// ============================================
// OPL sample rate ≈ 48,002 Hz (div-29), close enough to the 48 kHz
// I2S clock that no resampling is needed — just latch directly.
always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        opl_audio_out     <= 16'sd0;
        opl_sample_toggle <= 1'b0;
    end else if (opl_sample) begin
        opl_audio_out     <= opl_snd;
        opl_sample_toggle <= ~opl_sample_toggle;
    end
end

endmodule
