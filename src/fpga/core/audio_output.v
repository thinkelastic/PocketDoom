//
// Audio output module for PocketDoom
// - Dual-clock FIFO bridges CPU clock to audio clock domain
// - I2S serializer outputs 48 kHz 16-bit stereo
// - Based on openfpga-litex audio.sv and sound_i2s.sv patterns
//

`default_nettype none

module audio_output (
    input  wire        clk_sys,       // CPU clock (FIFO write side)
    input  wire        clk_audio,     // 12.288 MHz (FIFO read side, audio master clock)
    input  wire        reset_n,

    // CPU write interface (SFX samples)
    input  wire        sample_wr,     // Write strobe (one clk_sys cycle)
    input  wire [31:0] sample_data,   // {left[15:0], right[15:0]}
    output wire [11:0] fifo_level,    // Write-side fill level
    output wire        fifo_full,

    // OPL2 hardware audio input (from opl2_wrapper, clk_sys domain)
    input  wire signed [15:0] opl_audio_in,
    input  wire               opl_toggle_in,   // toggles on each new OPL sample

    // I2S output
    output wire        audio_mclk,    // 12.288 MHz passthrough
    output wire        audio_lrck,    // Left/right clock (48 kHz)
    output wire        audio_dac      // Serial data
);

// ============================================
// MCLK passthrough
// ============================================
assign audio_mclk = clk_audio;

// ============================================
// Dual-clock FIFO (clk_sys -> clk_audio)
// ============================================
wire [15:0] fifo_l;
wire [15:0] fifo_r;
wire        fifo_empty;

dcfifo dcfifo_audio (
    .wrclk   (clk_sys),
    .rdclk   (clk_audio),

    .data    (sample_data),
    .wrreq   (sample_wr),

    .q       ({fifo_l, fifo_r}),
    .rdreq   (audio_pop && !fifo_empty),

    .rdempty (fifo_empty),
    .wrusedw (fifo_level),
    .wrfull  (fifo_full),

    .aclr    (~reset_n)
);
defparam dcfifo_audio.intended_device_family = "Cyclone V",
    dcfifo_audio.lpm_numwords  = 4096,
    dcfifo_audio.lpm_showahead = "OFF",
    dcfifo_audio.lpm_type      = "dcfifo",
    dcfifo_audio.lpm_width     = 32,
    dcfifo_audio.lpm_widthu    = 12,
    dcfifo_audio.overflow_checking  = "ON",
    dcfifo_audio.underflow_checking = "ON",
    dcfifo_audio.rdsync_delaypipe   = 5,
    dcfifo_audio.wrsync_delaypipe   = 5,
    dcfifo_audio.use_eab       = "ON";

// ============================================
// 48 kHz sample pop (12.288 MHz / 256 = 48 kHz)
// ============================================
reg [7:0] mclk_div = 8'hFF;
reg       audio_pop = 0;

always @(posedge clk_audio) begin
    audio_pop <= 0;
    if (mclk_div > 0) begin
        mclk_div <= mclk_div - 8'd1;
    end else begin
        mclk_div  <= 8'hFF;
        audio_pop <= 1;
    end
end

// ============================================
// SCLK generation (3.072 MHz = MCLK / 4)
// ============================================
reg [1:0] sclk_div;
wire      audgen_sclk = sclk_div[1] /* synthesis keep */;

always @(posedge clk_audio) begin
    sclk_div <= sclk_div + 2'd1;
end

// ============================================
// I2S serializer (16-bit signed stereo)
// ============================================
// Data format: 32 bits per channel (16 data + 16 dummy), MSB first
// LRCK toggles every 32 SCLK cycles

// Hold last valid sample on FIFO underrun, then ramp to zero.
// Immediate drop to silence causes a hard discontinuity = audible pop.
// Instead, decay the held value toward zero over ~5ms (256 samples at 48kHz).
reg signed [15:0] hold_l = 16'sh0;
reg signed [15:0] hold_r = 16'sh0;

always @(posedge clk_audio) begin
    if (audio_pop) begin
        if (!fifo_empty) begin
            hold_l <= $signed(fifo_l);
            hold_r <= $signed(fifo_r);
        end else begin
            // Ramp toward zero: arithmetic right shift by 6 (~64 sample fade)
            // Keeps sign, converges to 0 for both positive and negative values
            hold_l <= hold_l - (hold_l >>> 6);
            hold_r <= hold_r - (hold_r >>> 6);
        end
    end
end

wire signed [15:0] sfx_l = fifo_empty ? hold_l : $signed(fifo_l);
wire signed [15:0] sfx_r = fifo_empty ? hold_r : $signed(fifo_r);

// CDC: toggle-handshake for 16-bit OPL audio (clk_sys → clk_audio).
// A simple double-register on a multi-bit bus can glitch when routing
// skew causes different bits to be captured from different samples.
// Instead, sync only the 1-bit toggle; when an edge is detected the
// 16-bit data has been stable for 2+ clk_audio periods (~163 ns >>
// any intra-FPGA routing skew) so it can be safely latched.
reg        opl_tog_s1, opl_tog_s2, opl_tog_s3;
reg signed [15:0] opl_latched;
always @(posedge clk_audio) begin
    opl_tog_s1 <= opl_toggle_in;
    opl_tog_s2 <= opl_tog_s1;
    opl_tog_s3 <= opl_tog_s2;
    if (opl_tog_s2 != opl_tog_s3)
        opl_latched <= opl_audio_in;   // data stable — safe to latch
end

// Mix SFX + OPL into both L and R channels, then 1.5x boost (×3 >>> 1).
wire signed [15:0] opl_scaled = opl_latched;
wire signed [16:0] mix_l = {sfx_l[15], sfx_l} + {opl_scaled[15], opl_scaled};
wire signed [16:0] mix_r = {sfx_r[15], sfx_r} + {opl_scaled[15], opl_scaled};

// 3x boost
wire signed [18:0] boost_l = {{2{mix_l[16]}}, mix_l} + {mix_l[16], mix_l, 1'b0};
wire signed [18:0] boost_r = {{2{mix_r[16]}}, mix_r} + {mix_r[16], mix_r, 1'b0};

// Clamp to 16-bit signed
wire [15:0] mix_clamp_l = (boost_l > 19'sd32767)  ? 16'h7FFF :
                           (boost_l < -19'sd32768) ? 16'h8000 :
                           boost_l[15:0];
wire [15:0] mix_clamp_r = (boost_r > 19'sd32767)  ? 16'h7FFF :
                           (boost_r < -19'sd32768) ? 16'h8000 :
                           boost_r[15:0];

// ============================================
// IIR low-pass filter + DC blocker + audio mix
// (ported from openfpgaOS audio filter chain)
// ============================================
wire [15:0] filt_out_l;
wire [15:0] filt_out_r;

audio_filters #(.CLK_RATE(12288000)) audio_filt (
    .clk       (clk_audio),
    .reset     (~reset_n),

    // Filter coefficients — light low-pass (~12 kHz cutoff)
    .flt_rate  (32'd11000000),
    .cx        (40'd16777216),
    .cx0       (8'd3),
    .cx1       (8'd3),
    .cx2       (8'd1),
    .cy0       (-24'd5765342),
    .cy1       ( 24'd5285916),
    .cy2       (-24'd1611482),

    // Audio mix controls (passthrough, no attenuation)
    .att       (5'b0),
    .mix       (2'b0),

    .is_signed (1'b1),
    .core_l    (mix_clamp_l),
    .core_r    (mix_clamp_r),

    .audio_l   (filt_out_l),
    .audio_r   (filt_out_r)
);

// Latch filtered output on audio_pop (48 kHz) so the I2S serializer
// always reads a stable value — eliminates race with FIFO read timing.
reg [15:0] active_l = 16'h0;
reg [15:0] active_r = 16'h0;
always @(posedge clk_audio) begin
    if (audio_pop) begin
        active_l <= mix_clamp_l;
        active_r <= mix_clamp_r;
    end
end

reg [31:0] audgen_sampshift;
reg [4:0]  audgen_lrck_cnt;
reg        audgen_lrck;
reg        audgen_dac;

always @(negedge audgen_sclk) begin
    // Output next bit
    audgen_dac <= audgen_sampshift[31];

    // 48 kHz * 64 bits = 3.072 MHz
    audgen_lrck_cnt <= audgen_lrck_cnt + 5'd1;
    if (audgen_lrck_cnt == 5'd31) begin
        // Switch channels
        audgen_lrck <= ~audgen_lrck;

        // Reload sample data at start of left channel
        if (~audgen_lrck) begin
            audgen_sampshift <= {active_l, active_r};
        end
    end else if (audgen_lrck_cnt < 5'd16) begin
        // Shift out 16 active bits per channel
        audgen_sampshift <= {audgen_sampshift[30:0], 1'b0};
    end
end

assign audio_lrck = audgen_lrck;
assign audio_dac  = audgen_dac;

endmodule
