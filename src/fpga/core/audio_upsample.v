//
// Hardware audio upsampler for PocketDoom
//
// - Single-clock BRAM ring buffer (512 x 32-bit, clk_sys domain)
// - CPU writes 11,025 Hz stereo samples via MMIO
// - HW linear interpolation upsamples to 48 kHz
// - Output feeds the proven dcfifo in audio_output for CDC to clk_audio
//

`default_nettype none

module audio_upsample (
    input  wire        clk,          // clk_sys (66 MHz)
    input  wire        reset_n,

    // CPU write interface (from axi_periph_slave)
    input  wire        sample_wr,    // Write strobe (one cycle)
    input  wire [31:0] sample_data,  // {left[15:0], right[15:0]}
    output wire [9:0]  buf_level,    // Ring buffer fill level (for CPU MMIO)

    // Output to audio_output FIFO (48 kHz, clk_sys domain)
    output reg         out_wr,
    output reg  [31:0] out_data,
    input  wire        out_full      // FIFO backpressure (safety)
);

// ============================================
// 48 kHz tick generator (66 MHz / 1375 = 48 kHz)
// ============================================
// clk = 100 MHz (clk_cpu = clk_ram_controller from mf_pllram_133)
// 100,000,000 / 48,000 = 2083.33 → 2083
localparam [11:0] TICK_DIV = 12'd2083;

reg [11:0] tick_cnt;
wire tick_48k = (tick_cnt == 12'd0);

always @(posedge clk or negedge reset_n) begin
    if (!reset_n)
        tick_cnt <= TICK_DIV - 12'd1;
    else if (tick_cnt == 12'd0)
        tick_cnt <= TICK_DIV - 12'd1;
    else
        tick_cnt <= tick_cnt - 12'd1;
end

// ============================================
// Ring buffer (512 x 32-bit, single-clock BRAM)
// ============================================
reg [31:0] ringbuf [0:511];

// Write pointer (10-bit for wrap detection)
reg [9:0] wr_ptr;

always @(posedge clk or negedge reset_n) begin
    if (!reset_n)
        wr_ptr <= 10'd0;
    else if (sample_wr) begin
        ringbuf[wr_ptr[8:0]] <= sample_data;
        wr_ptr <= wr_ptr + 10'd1;
    end
end

// Read pointer (10-bit for wrap detection)
reg [9:0] rd_ptr;

// Single-clock BRAM read port
reg [8:0] rd_addr;
reg [31:0] rd_data;

always @(posedge clk)
    rd_data <= ringbuf[rd_addr];

// Buffer level (same clock domain, no CDC needed)
assign buf_level = wr_ptr - rd_ptr;
wire buf_has_pair = (buf_level >= 10'd2);

// ============================================
// Upsampler state machine
// ============================================

// Fractional position (0.16 fixed-point)
reg [15:0] sub_frac;

// Upsample step: (11025 << 16) / 48000 ≈ 15052.8 → 15053
localparam [15:0] UPSAMPLE_STEP = 16'd15053;

// Advance calculation
wire [16:0] next_frac = {1'b0, sub_frac} + {1'b0, UPSAMPLE_STEP};

// States
localparam S_IDLE    = 2'd0;
localparam S_FETCH_A = 2'd1;
localparam S_FETCH_B = 2'd2;
localparam S_OUTPUT  = 2'd3;

reg [1:0] state;

// Latched sample A
reg signed [15:0] samp_a_l, samp_a_r;

// Held output for underrun ramp
reg signed [15:0] hold_l, hold_r;

// Interpolation wires — explicit sign extension to avoid unsigned poisoning
// Verilog concatenation is always unsigned; mixed signed/unsigned → unsigned.
// Be explicit: sign-extend operands before subtraction, use $signed on fraction.
wire signed [16:0] diff_l = {rd_data[31], rd_data[31:16]} - {samp_a_l[15], samp_a_l};
wire signed [16:0] diff_r = {rd_data[15], rd_data[15:0]}  - {samp_a_r[15], samp_a_r};
wire signed [16:0] frac_s = $signed({1'b0, sub_frac});
wire signed [33:0] prod_l = diff_l * frac_s;
wire signed [33:0] prod_r = diff_r * frac_s;
wire signed [33:0] base_l = $signed({{2{samp_a_l[15]}}, samp_a_l, 16'b0});
wire signed [33:0] base_r = $signed({{2{samp_a_r[15]}}, samp_a_r, 16'b0});
wire signed [33:0] full_l = base_l + prod_l;
wire signed [33:0] full_r = base_r + prod_r;
wire signed [15:0] lerp_l = full_l[31:16];
wire signed [15:0] lerp_r = full_r[31:16];

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        state    <= S_IDLE;
        rd_ptr   <= 10'd0;
        sub_frac <= 16'd0;
        samp_a_l <= 16'sh0;
        samp_a_r <= 16'sh0;
        hold_l   <= 16'sh0;
        hold_r   <= 16'sh0;
        rd_addr  <= 9'd0;
        out_wr   <= 1'b0;
        out_data <= 32'd0;
    end else begin
        out_wr <= 1'b0;  // default: no write

        case (state)

        S_IDLE: begin
            if (tick_48k) begin
                if (buf_has_pair) begin
                    // Start BRAM fetch: set address for sample A
                    rd_addr <= rd_ptr[8:0];
                    state   <= S_FETCH_A;
                end else if (!out_full) begin
                    // Underrun: ramp held value toward zero, push to FIFO
                    hold_l <= hold_l - (hold_l >>> 6);
                    hold_r <= hold_r - (hold_r >>> 6);
                    out_wr   <= 1'b1;
                    out_data <= {hold_l - (hold_l >>> 6), hold_r - (hold_r >>> 6)};
                end
            end
        end

        S_FETCH_A: begin
            // BRAM will have sample A on next cycle
            // Set address for sample B
            rd_addr <= rd_ptr[8:0] + 9'd1;
            state   <= S_FETCH_B;
        end

        S_FETCH_B: begin
            // rd_data now has sample A — latch it
            samp_a_l <= $signed(rd_data[31:16]);
            samp_a_r <= $signed(rd_data[15:0]);
            // BRAM will have sample B on next cycle
            state <= S_OUTPUT;
        end

        S_OUTPUT: begin
            // rd_data now has sample B — interpolation wires valid
            hold_l <= lerp_l;
            hold_r <= lerp_r;

            if (!out_full) begin
                out_wr   <= 1'b1;
                out_data <= {lerp_l, lerp_r};
            end

            // Advance fractional position
            sub_frac <= next_frac[15:0];
            if (next_frac[16])
                rd_ptr <= rd_ptr + 10'd1;

            state <= S_IDLE;
        end

        endcase
    end
end

endmodule
