//
// User core top-level (PocketDoom)
//
// VexiiRiscv CPU with AXI4 bus architecture, OPL2 sound synthesis.
// Instantiated by the real top-level: apf_top
//

`default_nettype none

module core_top (

//
// physical connections
//

///////////////////////////////////////////////////
// clock inputs 74.25mhz. not phase aligned, so treat these domains as asynchronous

input   wire            clk_74a, // mainclk1
input   wire            clk_74b, // mainclk1

///////////////////////////////////////////////////
// cartridge interface
// switches between 3.3v and 5v mechanically
// output enable for multibit translators controlled by pic32

// GBA AD[15:8]
inout   wire    [7:0]   cart_tran_bank2,
output  wire            cart_tran_bank2_dir,

// GBA AD[7:0]
inout   wire    [7:0]   cart_tran_bank3,
output  wire            cart_tran_bank3_dir,

// GBA A[23:16]
inout   wire    [7:0]   cart_tran_bank1,
output  wire            cart_tran_bank1_dir,

// GBA [7] PHI#
// GBA [6] WR#
// GBA [5] RD#
// GBA [4] CS1#/CS#
//     [3:0] unwired
inout   wire    [7:4]   cart_tran_bank0,
output  wire            cart_tran_bank0_dir,

// GBA CS2#/RES#
inout   wire            cart_tran_pin30,
output  wire            cart_tran_pin30_dir,
output  wire            cart_pin30_pwroff_reset,

// GBA IRQ/DRQ
inout   wire            cart_tran_pin31,
output  wire            cart_tran_pin31_dir,

// infrared
input   wire            port_ir_rx,
output  wire            port_ir_tx,
output  wire            port_ir_rx_disable,

// GBA link port
inout   wire            port_tran_si,
output  wire            port_tran_si_dir,
inout   wire            port_tran_so,
output  wire            port_tran_so_dir,
inout   wire            port_tran_sck,
output  wire            port_tran_sck_dir,
inout   wire            port_tran_sd,
output  wire            port_tran_sd_dir,

///////////////////////////////////////////////////
// cellular psram 0 and 1, two chips (64mbit x2 dual die per chip)

output  wire    [21:16] cram0_a,
inout   wire    [15:0]  cram0_dq,
input   wire            cram0_wait,
output  wire            cram0_clk,
output  wire            cram0_adv_n,
output  wire            cram0_cre,
output  wire            cram0_ce0_n,
output  wire            cram0_ce1_n,
output  wire            cram0_oe_n,
output  wire            cram0_we_n,
output  wire            cram0_ub_n,
output  wire            cram0_lb_n,

output  wire    [21:16] cram1_a,
inout   wire    [15:0]  cram1_dq,
input   wire            cram1_wait,
output  wire            cram1_clk,
output  wire            cram1_adv_n,
output  wire            cram1_cre,
output  wire            cram1_ce0_n,
output  wire            cram1_ce1_n,
output  wire            cram1_oe_n,
output  wire            cram1_we_n,
output  wire            cram1_ub_n,
output  wire            cram1_lb_n,

///////////////////////////////////////////////////
// sdram, 512mbit 16bit

output  wire    [12:0]  dram_a,
output  wire    [1:0]   dram_ba,
inout   wire    [15:0]  dram_dq,
output  wire    [1:0]   dram_dqm,
output  wire            dram_clk,
output  wire            dram_cke,
output  wire            dram_ras_n,
output  wire            dram_cas_n,
output  wire            dram_we_n,

///////////////////////////////////////////////////
// sram, 1mbit 16bit

output  wire    [16:0]  sram_a,
inout   wire    [15:0]  sram_dq,
output  wire            sram_oe_n,
output  wire            sram_we_n,
output  wire            sram_ub_n,
output  wire            sram_lb_n,

///////////////////////////////////////////////////
// vblank driven by dock for sync in a certain mode

input   wire            vblank,

///////////////////////////////////////////////////
// i/o to 6515D breakout usb uart

output  wire            dbg_tx,
input   wire            dbg_rx,

///////////////////////////////////////////////////
// i/o pads near jtag connector user can solder to

output  wire            user1,
input   wire            user2,

///////////////////////////////////////////////////
// RFU internal i2c bus

inout   wire            aux_sda,
output  wire            aux_scl,

///////////////////////////////////////////////////
// RFU, do not use
output  wire            vpll_feed,


//
// logical connections
//

///////////////////////////////////////////////////
// video, audio output to scaler
output  wire    [23:0]  video_rgb,
output  wire            video_rgb_clock,
output  wire            video_rgb_clock_90,
output  wire            video_de,
output  wire            video_skip,
output  wire            video_vs,
output  wire            video_hs,

output  wire            audio_mclk,
input   wire            audio_adc,
output  wire            audio_dac,
output  wire            audio_lrck,

///////////////////////////////////////////////////
// bridge bus connection
// synchronous to clk_74a
output  wire            bridge_endian_little,
input   wire    [31:0]  bridge_addr,
input   wire            bridge_rd,
output  reg     [31:0]  bridge_rd_data,
input   wire            bridge_wr,
input   wire    [31:0]  bridge_wr_data,

///////////////////////////////////////////////////
// controller data
input   wire    [31:0]  cont1_key,
input   wire    [31:0]  cont2_key,
input   wire    [31:0]  cont3_key,
input   wire    [31:0]  cont4_key,
input   wire    [31:0]  cont1_joy,
input   wire    [31:0]  cont2_joy,
input   wire    [31:0]  cont3_joy,
input   wire    [31:0]  cont4_joy,
input   wire    [15:0]  cont1_trig,
input   wire    [15:0]  cont2_trig,
input   wire    [15:0]  cont3_trig,
input   wire    [15:0]  cont4_trig

);

// not using the IR port, so turn off both the LED, and
// disable the receive circuit to save power
assign port_ir_tx = 0;
assign port_ir_rx_disable = 1;

// bridge endianness
assign bridge_endian_little = 1;

// ============================================================
// Analogizer adapter (optional, directly controls cart port)
// ============================================================

//Pocket Menu settings
reg [31:0] analogizer_settings;
//wire [31:0] analogizer_settings_s;

// Resolution setting from interact.json (bridge 0xF700000C)
reg [31:0] original_mode_74a;
reg [31:0] original_mode_sync1, original_mode_sync2;
always @(posedge clk_cpu) begin
    original_mode_sync1 <= original_mode_74a;
    original_mode_sync2 <= original_mode_sync1;
end

// Run Mode setting from interact.json (bridge 0xF7000014)
reg [31:0] run_mode_74a;
reg [31:0] run_mode_sync1, run_mode_sync2;
always @(posedge clk_cpu) begin
    run_mode_sync1 <= run_mode_74a;
    run_mode_sync2 <= run_mode_sync1;
end

// Refresh Rate setting from interact.json (bridge 0xF7000018)
reg [31:0] refresh_rate_74a;
// CDC into video clock domain (for V_TOTAL control)
reg [31:0] refresh_rate_vid_sync1, refresh_rate_vid_sync2;
always @(posedge clk_core_12288) begin
    refresh_rate_vid_sync1 <= refresh_rate_74a;
    refresh_rate_vid_sync2 <= refresh_rate_vid_sync1;
end
// CDC into CPU clock domain (for firmware MMIO readback)
reg [31:0] refresh_rate_cpu_sync1, refresh_rate_cpu_sync2;
always @(posedge clk_cpu) begin
    refresh_rate_cpu_sync1 <= refresh_rate_74a;
    refresh_rate_cpu_sync2 <= refresh_rate_cpu_sync1;
end

// Game ID from instance JSON memory_writes (bridge 0xF7000010)
reg [31:0] game_id_74a;
reg [31:0] game_id_sync1, game_id_sync2;
always @(posedge clk_cpu) begin
    game_id_sync1 <= game_id_74a;
    game_id_sync2 <= game_id_sync1;
end

reg       analogizer_ena;
reg [3:0] analogizer_video_type;
reg [4:0] snac_game_cont_type /* synthesis keep */;
reg [3:0] snac_cont_assignment /* synthesis keep */;

//synch_3 #(.WIDTH(32)) sync_analogizer(analogizer_settings, analogizer_settings_s, clk_core_49152);

  //create aditional switch to blank Pocket screen.
  //assign video_rgb = (analogizer_video_type[3]) ? 24'h000000: video_rgb_reg;

always @(*) begin
  snac_game_cont_type   = analogizer_settings[4:0];
  snac_cont_assignment  = analogizer_settings[9:6];
  analogizer_video_type = analogizer_settings[13:10];
  analogizer_ena        = analogizer_settings[15];
end 



    wire pocket_blank_screen = analogizer_settings[13] && analogizer_ena;

    //create aditional switch to blank Pocket screen.
    wire [23:0] video_rgb_doom;
    //assign video_rgb_irem72 = (pocket_blank_screen && !analogizer_ena) ? 24'h000000: {core_r,core_g,core_b};
    assign video_rgb_doom = (pocket_blank_screen) ? 24'h000000: vidout_rgb;

    //switch between Analogizer SNAC and Pocket Controls for P1-P4 (P3,P4 when uses PCEngine Multitap)
    wire [15:0] p1_btn, p2_btn;
    wire [31:0] p1_joy, p2_joy;

    reg [31:0] p1_controls, p2_controls;
    reg [31:0] p1_joypad, p2_joypad;
    reg [15:0] p1_trigger, p2_trigger;

    always @(posedge clk_74a) begin
        if((snac_game_cont_type == 5'h0) || (analogizer_ena == 1'b0)) begin //SNAC is disabled
            p1_controls <= cont1_key;
            p1_joypad   <= cont1_joy;
            p1_trigger  <= cont1_trig;
            p2_controls <= cont2_key;
            p2_joypad   <= cont2_joy;
            p2_trigger  <= cont2_trig;
        end
        else begin
        case(snac_cont_assignment[1:0])
        2'h0: begin  //SNAC P1 -> Pocket P1
            p1_controls <= p1_btn;
            p1_joypad   <= p1_joy;
            p1_trigger  <= 15'h00;

            p2_controls <= cont2_key;
            p2_joypad   <= cont2_joy;
            p2_trigger  <= cont2_trig;
            end
        2'h1: begin  //SNAC P1 -> Pocket P2
            p1_controls <= cont1_key;
            p1_joypad   <= cont1_joy;
            p1_trigger  <= cont1_trig;

            p2_controls <= p1_btn;
            p2_joypad   <= p2_joy;
            p2_trigger  <= 15'h00;
            end
        2'h2: begin //SNAC P1 -> Pocket P1, SNAC P2 -> Pocket P2
            p1_controls <= p1_btn;
            p1_joypad <= p1_joy;
            p1_trigger <= 15'h00;
            p2_controls <= p2_btn;
            p2_joypad <= p2_joy;
            p2_trigger <= 15'h00;
            end
        2'h3: begin //SNAC P1 -> Pocket P2, SNAC P2 -> Pocket P1
            p1_controls <= p2_btn;
            p1_joypad <= p2_joy;
            p1_trigger <= 15'h00;
            p2_controls <= p1_btn;
            p2_joypad <= p1_joy;
            p2_trigger <= 15'h00;
            end
        default: begin 
            p1_controls <= cont1_key;
            p1_joypad   <= cont1_joy;
            p1_trigger  <= cont1_trig;

            p2_controls <= cont2_key;
            p2_joypad   <= cont2_joy;
            p2_trigger  <= cont2_trig;
            end
        endcase
        end
    end

    wire [15:0] p1_btn_CK, p2_btn_CK;
    wire [31:0] p1_joy_CK, p2_joy_CK;
    synch_3 #(
    .WIDTH(16)
    ) p1b_s (
        p1_btn_CK,
        p1_btn,
        clk_74a
    );

    synch_3 #(
        .WIDTH(16)
    ) p2b_s (
        p2_btn_CK,
        p2_btn,
        clk_74a
    );

    synch_3 #(
    .WIDTH(32)
    ) p3b_s (
        p1_joy_CK,
        p1_joy,
        clk_74a
    );
        
    synch_3 #(
        .WIDTH(32)
    ) p4b_s (
        p2_joy_CK,
        p2_joy,
        clk_74a
    );

    reg [2:0] fx /* synthesis preserve */;
    always @(posedge clk_core_49152) begin
        case (analogizer_video_type)
            4'd5, 4'd13:    begin fx <= 3'd0; end //SC  0%     1 SC 25%
            4'd6, 4'd14:    begin fx <= 3'd2; end //SC  50%    3 SC 75%
            4'd7, 4'd15:    begin fx <= 3'd4; end //hq2x
            default:        begin fx <= 3'd0; end
        endcase
    end


// Video Y/C Encoder settings
// Follows the Mike Simone Y/C encoder settings:
// https://github.com/MikeS11/MiSTerFPGA_YC_Encoder
// SET PAL and NTSC TIMING and pass through status bits. ** YC must be enabled in the qsf file **
wire [39:0] CHROMA_PHASE_INC;
wire [26:0] COLORBURST_RANGE;
wire [4:0] CHROMA_ADD;
wire [4:0] CHROMA_MULT;
wire PALFLAG;

parameter NTSC_REF = 3.579545;   
parameter PAL_REF = 4.43361875;

// Parameters to be modifed
parameter CLK_VIDEO_NTSC = 49.152; 
parameter CLK_VIDEO_PAL  = 49.152; 

localparam [39:0] NTSC_PHASE_INC = 40'd80073066196;  //print(round(3.579545 * 2**40 / 49.152)) 
localparam [39:0] PAL_PHASE_INC =  40'd99178372574; //print(round(4.43361875 * 2**40 / 49.152)) 

assign CHROMA_PHASE_INC = ((analogizer_video_type == 4'h4)|| (analogizer_video_type == 4'hC)) ? PAL_PHASE_INC : NTSC_PHASE_INC; 
assign PALFLAG = (analogizer_video_type == 4'h4) || (analogizer_video_type == 4'hC); 


// H/V offset
reg [31:0] signed_hoff;
reg [31:0] signed_voff;

wire [5:0]	hoffset = signed_hoff[5:0];
wire  [4:0]	voffset = signed_voff[4:0];
wire video_ce_pix, half_ce_pix;

jtframe_frac_cen #(.W(2)) pixel_cen
(
    .clk(clk_core_49152),
    .n(10'd1),
    .m(10'd4),
    .cen({video_ce_pix,half_ce_pix})
);
wire HSync,VSync;
jtframe_resync jtframe_resync
(
    .clk(clk_core_49152),
    .pxl_cen(video_ce_pix),
    .hs_in(crt_hs),
    .vs_in(crt_vs),
    .LVBL(crt_vblank),
    .LHBL(crt_hblank),
    .hoffset(hoffset), //5bits signed
    .voffset(voffset), //5bits signed
    .hs_out(HSync),
    .vs_out(VSync)
);

wire crt_csync;
wire crt_blankn;

assign crt_csync = ~(HSync ^ VSync);
assign crt_blankn   = ~(crt_hblank | crt_vblank);

openFPGA_Pocket_Analogizer #(
    .MASTER_CLK_FREQ(49_152_000),
    .LINE_LENGTH(640)
) analogizer (
    .i_clk(clk_core_49152), //currently 50MHz
    .i_rst(~reset_n),
    .i_ena(analogizer_ena),
    // Video interface (active but directly from our pipeline)
    .video_clk(clk_core_12288), ////currently 12.25MHz
    .analog_video_type(analogizer_video_type),       // 0 RGBS
    .R(vidout_rgb[23:16]),
    .G(vidout_rgb[15:8]),
    .B(vidout_rgb[7:0]),
    .Hblank(crt_hblank),
    .Vblank(crt_vblank),
    .BLANKn(crt_blankn),
    .Hsync(HSync),
    .Vsync(VSync),
    .Csync(crt_csync ),
    // Y/C encoder (unused)
    .CHROMA_PHASE_INC(CHROMA_PHASE_INC),
    .PALFLAG(PALFLAG),
    // Scandoubler (unused)
    .ce_pix(1'b1),
    .scandoubler(1'b1),
    .fx(fx), //0 disable, 1 scanlines 25%, 2 scanlines 50%, 3 scanlines 75%, 4 hq2x
    // SNAC controller interface
    .conf_AB(snac_game_cont_type >= 5'd16),  //0 conf. A(default), 1 conf. B (see graph above)
    .game_cont_type(snac_game_cont_type),
    .p1_btn_state(p1_btn_CK),
    .p1_joy_state(p1_joy_CK),
    .p2_btn_state(p2_btn_CK),
    .p2_joy_state(p2_joy_CK),
    .p3_btn_state(),
    .p4_btn_state(),
    // Rumble (unused)
    .i_VIB_SW1(2'b0),
    .i_VIB_DAT1(8'h0),
    .i_VIB_SW2(2'b0),
    .i_VIB_DAT2(8'h0),
    // Status
    .busy(),
    // Cartridge port (directly driven by Analogizer)
    .cart_tran_bank2(cart_tran_bank2),
    .cart_tran_bank2_dir(cart_tran_bank2_dir),
    .cart_tran_bank3(cart_tran_bank3),
    .cart_tran_bank3_dir(cart_tran_bank3_dir),
    .cart_tran_bank1(cart_tran_bank1),
    .cart_tran_bank1_dir(cart_tran_bank1_dir),
    .cart_tran_bank0(cart_tran_bank0),
    .cart_tran_bank0_dir(cart_tran_bank0_dir),
    .cart_tran_pin30(cart_tran_pin30),
    .cart_tran_pin30_dir(cart_tran_pin30_dir),
    .cart_pin30_pwroff_reset(cart_pin30_pwroff_reset),
    .cart_tran_pin31(cart_tran_pin31),
    .cart_tran_pin31_dir(cart_tran_pin31_dir),
    // Debug
    .DBG_TX(),
    .o_stb()
);

// Link port directions
assign port_tran_si = 1'bz;
assign port_tran_si_dir = 1'b0;
assign port_tran_so = link_so_oe ? link_so_out : 1'bz;
assign port_tran_so_dir = link_so_oe;
assign port_tran_sck = link_sck_oe ? link_sck_out : 1'bz;
assign port_tran_sck_dir = link_sck_oe;
assign port_tran_sd = link_sd_oe ? link_sd_out : 1'bz;
assign port_tran_sd_dir = link_sd_oe;
assign link_si_i = port_tran_si;
assign link_sck_i = port_tran_sck;
assign link_sd_i = port_tran_sd;

// PSRAM Controller for CRAM0 (16MB)
psram_controller #(
    .CLOCK_SPEED(100.0)
) psram0 (
    .clk(clk_ram_controller),
    .reset_n(reset_n),
    .word_rd(psram_mux_rd),
    .word_wr(psram_mux_wr),
    .word_addr(psram_mux_addr),
    .word_data(psram_mux_wdata),
    .word_wstrb(psram_mux_wstrb),
    .word_q(psram_mux_rdata),
    .word_busy(psram_mux_busy),
    .word_q_valid(psram_mux_rdata_valid),
    .cram_a(cram0_a),
    .cram_dq(cram0_dq),
    .cram_wait(cram0_wait),
    .cram_clk(cram0_clk),
    .cram_adv_n(cram0_adv_n),
    .cram_cre(cram0_cre),
    .cram_ce0_n(cram0_ce0_n),
    .cram_ce1_n(cram0_ce1_n),
    .cram_oe_n(cram0_oe_n),
    .cram_we_n(cram0_we_n),
    .cram_ub_n(cram0_ub_n),
    .cram_lb_n(cram0_lb_n)
);

// PSRAM Controller for CRAM1 (16MB, used for save data — uncached)
wire        psram1_rd;
wire        psram1_wr;
wire [21:0] psram1_addr;
wire [31:0] psram1_wdata;
wire [3:0]  psram1_wstrb;
wire [31:0] psram1_rdata;
wire        psram1_busy;
wire        psram1_rdata_valid;

// PSRAM1 must NOT use core reset_n: the bridge loads nonvolatile save data
// BEFORE Reset Exit (while reset_n=0).  Use PLL lock instead so PSRAM1
// is ready to accept writes during the entire boot DMA phase.
reg [2:0] pll_ram_locked_sync;
always @(posedge clk_ram_controller)
    pll_ram_locked_sync <= {pll_ram_locked_sync[1:0], pll_ram_locked};
wire psram1_reset_n = pll_ram_locked_sync[2];

psram_controller #(
    .CLOCK_SPEED(100.0)
) psram1 (
    .clk(clk_ram_controller),
    .reset_n(psram1_reset_n),
    .word_rd(psram1_rd),
    .word_wr(psram1_wr),
    .word_addr(psram1_addr),
    .word_data(psram1_wdata),
    .word_wstrb(psram1_wstrb),
    .word_q(psram1_rdata),
    .word_busy(psram1_busy),
    .word_q_valid(psram1_rdata_valid),
    .cram_a(cram1_a),
    .cram_dq(cram1_dq),
    .cram_wait(cram1_wait),
    .cram_clk(cram1_clk),
    .cram_adv_n(cram1_adv_n),
    .cram_cre(cram1_cre),
    .cram_ce0_n(cram1_ce0_n),
    .cram_ce1_n(cram1_ce1_n),
    .cram_oe_n(cram1_oe_n),
    .cram_we_n(cram1_we_n),
    .cram_ub_n(cram1_ub_n),
    .cram_lb_n(cram1_lb_n)
);

// SRAM unused
assign sram_dq   = 16'hZZZZ;
assign sram_a    = 17'h0;
assign sram_oe_n = 1'b1;
assign sram_we_n = 1'b1;
assign sram_ub_n = 1'b1;
assign sram_lb_n = 1'b1;

assign dbg_tx = 1'bZ;
assign user1 = 1'bZ;
assign aux_scl = 1'bZ;
assign vpll_feed = 1'bZ;

// ============================================================
// SDRAM word interface signals (to io_sdram)
// ============================================================
reg             ram1_word_rd;
reg             ram1_word_wr;
reg     [23:0]  ram1_word_addr;
reg     [31:0]  ram1_word_data;
reg     [3:0]   ram1_word_wstrb;
reg     [3:0]   ram1_word_burst_len;
wire    [31:0]  ram1_word_q;
wire            ram1_word_busy;
wire            ram1_word_q_valid;

// axi_sdram_slave word-level outputs (held signals, need pulse conversion)
wire            sdram_slave_rd;
wire            sdram_slave_wr;
wire    [23:0]  sdram_slave_addr;
wire    [31:0]  sdram_slave_wdata;
wire    [3:0]   sdram_slave_wstrb;
wire    [3:0]   sdram_slave_burst_len;

// ============================================================
// CPU AXI4 master buses
// ============================================================

// CPU AXI4 master → axi_sdram_slave (via arbiter)
wire        cpu_m_sdram_arvalid;
wire        cpu_m_sdram_arready;
wire [31:0] cpu_m_sdram_araddr;
wire [7:0]  cpu_m_sdram_arlen;
wire        cpu_m_sdram_rvalid;
wire [31:0] cpu_m_sdram_rdata;
wire [1:0]  cpu_m_sdram_rresp;
wire        cpu_m_sdram_rlast;
wire        cpu_m_sdram_awvalid;
wire        cpu_m_sdram_awready;
wire [31:0] cpu_m_sdram_awaddr;
wire [7:0]  cpu_m_sdram_awlen;
wire        cpu_m_sdram_wvalid;
wire        cpu_m_sdram_wready;
wire [31:0] cpu_m_sdram_wdata;
wire [3:0]  cpu_m_sdram_wstrb;
wire        cpu_m_sdram_wlast;
wire        cpu_m_sdram_bvalid;
wire [1:0]  cpu_m_sdram_bresp;

// CPU AXI4 master → axi_psram_slave
wire        cpu_m_psram_arvalid;
wire        cpu_m_psram_arready;
wire [31:0] cpu_m_psram_araddr;
wire [7:0]  cpu_m_psram_arlen;
wire        cpu_m_psram_rvalid;
wire [31:0] cpu_m_psram_rdata;
wire [1:0]  cpu_m_psram_rresp;
wire        cpu_m_psram_rlast;
wire        cpu_m_psram_awvalid;
wire        cpu_m_psram_awready;
wire [31:0] cpu_m_psram_awaddr;
wire [7:0]  cpu_m_psram_awlen;
wire        cpu_m_psram_wvalid;
wire        cpu_m_psram_wready;
wire [31:0] cpu_m_psram_wdata;
wire [3:0]  cpu_m_psram_wstrb;
wire        cpu_m_psram_wlast;
wire        cpu_m_psram_bvalid;
wire [1:0]  cpu_m_psram_bresp;

// axi_psram_slave → PSRAM mux (word-level)
wire        cpu_psram_rd;
wire        cpu_psram_wr;
wire [21:0] cpu_psram_addr;
wire [31:0] cpu_psram_wdata;
wire [3:0]  cpu_psram_wstrb;
wire [31:0] cpu_psram_rdata;
wire        cpu_psram_busy;
wire        cpu_psram_rdata_valid;
wire        cpu_psram_bank;  // 0 = CRAM0, 1 = CRAM1

// Muxed PSRAM signals (bridge or CPU)
wire        psram_mux_rd;
wire        psram_mux_wr;
wire [21:0] psram_mux_addr;
wire [31:0] psram_mux_wdata;
wire [3:0]  psram_mux_wstrb;
wire [31:0] psram_mux_rdata;
wire        psram_mux_busy;
wire        psram_mux_rdata_valid;

// Audio output interface (from axi_periph_slave — CPU MMIO path)
wire        audio_cpu_sample_wr;
wire [31:0] audio_cpu_sample_data;
wire [9:0]  audio_buf_level;

// Audio DMA interface
wire        adma_sample_wr;
wire [31:0] adma_sample_data;
wire        adma_irq;
wire        adma_reg_wr;
wire [4:0]  adma_reg_addr;
wire [31:0] adma_reg_wdata;
wire [31:0] adma_reg_rdata;

// Audio DMA AXI4 read master signals
wire        adma_m_arvalid;
wire        adma_m_arready;
wire [31:0] adma_m_araddr;
wire [7:0]  adma_m_arlen;
wire        adma_m_rvalid;
wire [31:0] adma_m_rdata;
wire        adma_m_rlast;

// Mux: DMA takes priority when active (CPU should not write while DMA is enabled)
wire        audio_sample_wr   = adma_sample_wr   | audio_cpu_sample_wr;
wire [31:0] audio_sample_data = adma_sample_wr ? adma_sample_data : audio_cpu_sample_data;

// Upsampler → audio_output FIFO bridge
wire        audio_up_wr;
wire [31:0] audio_up_data;
wire [11:0] audio_fifo_level;
wire        audio_fifo_full;

// VRR: firmware-written V_TOTAL
wire [9:0] vrr_v_total;
// CDC vrr_v_total (clk_cpu) → clk_core_12288 (video timing)
reg [9:0] vrr_vt_sync1, vrr_vt_sync2;
always @(posedge clk_core_12288) begin
    vrr_vt_sync1 <= vrr_v_total;
    vrr_vt_sync2 <= vrr_vt_sync1;
end

// OPL2 hardware interface
wire        opl_write_req;
wire [1:0]  opl_write_addr;
wire [7:0]  opl_write_data;
wire        opl_ack;
wire signed [15:0] opl_audio_out;
wire               opl_sample_toggle;

// Link MMIO register interface
wire        link_reg_wr;
wire        link_reg_rd;
wire [4:0]  link_reg_addr;
wire [31:0] link_reg_wdata;
wire [31:0] link_reg_rdata;

// Link physical interface
wire        link_si_i;
wire        link_sck_i;
wire        link_sd_i;
wire        link_so_out;
wire        link_so_oe;
wire        link_sck_out;
wire        link_sck_oe;
wire        link_sd_out;
wire        link_sd_oe;

// CPU AXI4 master → axi_periph_slave (local peripherals)
wire        cpu_m_local_arvalid;
wire        cpu_m_local_arready;
wire [31:0] cpu_m_local_araddr;
wire [7:0]  cpu_m_local_arlen;
wire        cpu_m_local_rvalid;
wire [31:0] cpu_m_local_rdata;
wire [1:0]  cpu_m_local_rresp;
wire        cpu_m_local_rlast;
wire        cpu_m_local_awvalid;
wire        cpu_m_local_awready;
wire [31:0] cpu_m_local_awaddr;
wire [7:0]  cpu_m_local_awlen;
wire        cpu_m_local_wvalid;
wire        cpu_m_local_wready;
wire [31:0] cpu_m_local_wdata;
wire [3:0]  cpu_m_local_wstrb;
wire        cpu_m_local_wlast;
wire        cpu_m_local_bvalid;
wire [1:0]  cpu_m_local_bresp;

// AXI4 arbiter output → axi_sdram_slave
wire        arb_s_arvalid, arb_s_arready;
wire [31:0] arb_s_araddr;
wire [7:0]  arb_s_arlen;
wire        arb_s_rvalid, arb_s_rlast;
wire [31:0] arb_s_rdata;
wire [1:0]  arb_s_rresp;
wire        arb_s_awvalid, arb_s_awready;
wire [31:0] arb_s_awaddr;
wire [7:0]  arb_s_awlen;
wire        arb_s_wvalid, arb_s_wready, arb_s_wlast;
wire [31:0] arb_s_wdata;
wire [3:0]  arb_s_wstrb;
wire        arb_s_bvalid;
wire [1:0]  arb_s_bresp;

// Bridge AXI4 master (from axi_bridge_master to axi_sdram_arbiter)
wire        bridge_m_arvalid, bridge_m_arready;
wire [31:0] bridge_m_araddr;
wire [7:0]  bridge_m_arlen;
wire        bridge_m_rvalid, bridge_m_rlast;
wire [31:0] bridge_m_rdata;
wire [1:0]  bridge_m_rresp;
wire        bridge_m_awvalid, bridge_m_awready;
wire [31:0] bridge_m_awaddr;
wire [7:0]  bridge_m_awlen;
wire        bridge_m_wvalid, bridge_m_wready, bridge_m_wlast;
wire [31:0] bridge_m_wdata;
wire [3:0]  bridge_m_wstrb;
wire        bridge_m_bvalid;
wire [1:0]  bridge_m_bresp;
wire        bridge_m_idle;
wire        bridge_m_wr_idle;
wire [31:0] bridge_axi_rd_data;
wire        bridge_axi_rd_done;


// ============================================================
// Bridge read data mux
// ============================================================
always @(*) begin
    casex(bridge_addr)
        default: begin
            bridge_rd_data <= 0;
        end
        32'b000000xx_xxxxxxxx_xxxxxxxx_xxxxxxxx: begin
                bridge_rd_data <= bridge_rd_data_captured;
        end
        32'h30xxxxxx: begin
                bridge_rd_data <= cram1_rd_resp_data;
        end

        32'hF7000000: begin 
        //the byte order is inverted because the bridge_endian_little = 1
        bridge_rd_data <= {analogizer_settings[7:0],analogizer_settings[15:8],analogizer_settings[23:16],analogizer_settings[31:24]};
        end
        32'hF7000004: begin 
        //the byte order is inverted because the bridge_endian_little = 1
        bridge_rd_data <= {signed_hoff[7:0],signed_hoff[15:8],signed_hoff[23:16],signed_hoff[31:24]}; //signed_hoff;
        end
        32'hF7000008: begin
        //the byte order is inverted because the bridge_endian_little = 1
        bridge_rd_data <= {signed_voff[7:0],signed_voff[15:8],signed_voff[23:16],signed_voff[31:24]}; //signed_voff;
        end
        32'hF700000C: begin
        bridge_rd_data <= {original_mode_74a[7:0],original_mode_74a[15:8],original_mode_74a[23:16],original_mode_74a[31:24]};
        end
        32'hF7000014: begin
        bridge_rd_data <= {run_mode_74a[7:0],run_mode_74a[15:8],run_mode_74a[23:16],run_mode_74a[31:24]};
        end
        32'hF7000018: begin
        bridge_rd_data <= {refresh_rate_74a[7:0],refresh_rate_74a[15:8],refresh_rate_74a[23:16],refresh_rate_74a[31:24]};
        end

        32'hF8xxxxxx: begin
            bridge_rd_data <= cmd_bridge_rd_data;
        end
    endcase
end

// Interact variable writes (SNAC adapter type from APF menu)
always @(posedge clk_74a) begin
    if (bridge_wr) begin
        casex (bridge_addr)
        32'hF7000000: analogizer_settings <= {
        bridge_wr_data[7:0], 
        bridge_wr_data[15:8], 
        bridge_wr_data[23:16], 
        bridge_wr_data[31:24]};

        32'hF7000004: signed_hoff <= {
        bridge_wr_data[7:0], 
        bridge_wr_data[15:8], 
        bridge_wr_data[23:16], 
        bridge_wr_data[31:24]};

        32'hF7000008: signed_voff <= {
        bridge_wr_data[7:0],
        bridge_wr_data[15:8],
        bridge_wr_data[23:16],
        bridge_wr_data[31:24]};

        32'hF700000C: original_mode_74a <= {
        bridge_wr_data[7:0],
        bridge_wr_data[15:8],
        bridge_wr_data[23:16],
        bridge_wr_data[31:24]};

        32'hF7000014: run_mode_74a <= {
        bridge_wr_data[7:0],
        bridge_wr_data[15:8],
        bridge_wr_data[23:16],
        bridge_wr_data[31:24]};

        32'hF7000010: game_id_74a <= {
        bridge_wr_data[7:0],
        bridge_wr_data[15:8],
        bridge_wr_data[23:16],
        bridge_wr_data[31:24]};

        32'hF7000018: refresh_rate_74a <= {
        bridge_wr_data[7:0],
        bridge_wr_data[15:8],
        bridge_wr_data[23:16],
        bridge_wr_data[31:24]};
        endcase
    end
end

// ============================================================
// Bridge SDRAM Write CDC: dcfifo (clk_74a -> clk_ram_controller)
// ============================================================
localparam integer BRIDGE_WR_SKID_DEPTH = 4;
wire        bridge_sdram_wr = bridge_wr && (bridge_addr[31:26] == 6'b000000);

wire        bridge_wr_fifo_wrreq;
wire        bridge_wr_fifo_full;
wire [55:0] bridge_wr_fifo_wdata;
wire        bridge_wr_fifo_drain;
wire        bridge_wr_fifo_empty;
wire [55:0] bridge_wr_fifo_q;
reg [55:0]  bridge_wr_skid_data [0:BRIDGE_WR_SKID_DEPTH-1];
reg [1:0]   bridge_wr_skid_wrptr;
reg [1:0]   bridge_wr_skid_rdptr;
reg [2:0]   bridge_wr_skid_count;
wire        bridge_wr_skid_empty = (bridge_wr_skid_count == 0);
wire        bridge_wr_skid_nonempty_74a = !bridge_wr_skid_empty;
wire        bridge_wr_skid_pop = !bridge_wr_skid_empty && !bridge_wr_fifo_full;
wire [55:0] bridge_wr_skid_head =
            (bridge_wr_skid_rdptr == 2'd0) ? bridge_wr_skid_data[0] :
            (bridge_wr_skid_rdptr == 2'd1) ? bridge_wr_skid_data[1] :
            (bridge_wr_skid_rdptr == 2'd2) ? bridge_wr_skid_data[2] :
                                             bridge_wr_skid_data[3];
wire        bridge_wr_skid_push = bridge_sdram_wr;
wire        bridge_wr_skid_has_space = (bridge_wr_skid_count != 3'd4);
wire        bridge_wr_skid_push_ok = bridge_wr_skid_push &&
                                     (bridge_wr_skid_has_space || bridge_wr_skid_pop);
assign bridge_wr_fifo_wrreq = bridge_wr_skid_pop;
assign bridge_wr_fifo_wdata = bridge_wr_skid_head;

always @(posedge clk_74a) begin
    if (!reset_n_apf) begin
        bridge_wr_skid_wrptr <= 2'd0;
        bridge_wr_skid_rdptr <= 2'd0;
        bridge_wr_skid_count <= 3'd0;
    end else begin
        if (bridge_wr_skid_pop) begin
            bridge_wr_skid_rdptr <= bridge_wr_skid_rdptr + 2'd1;
        end

        if (bridge_wr_skid_push_ok) begin
            case (bridge_wr_skid_wrptr)
                2'd0: bridge_wr_skid_data[0] <= {bridge_addr[25:2], bridge_wr_data[31:0]};
                2'd1: bridge_wr_skid_data[1] <= {bridge_addr[25:2], bridge_wr_data[31:0]};
                2'd2: bridge_wr_skid_data[2] <= {bridge_addr[25:2], bridge_wr_data[31:0]};
                default: bridge_wr_skid_data[3] <= {bridge_addr[25:2], bridge_wr_data[31:0]};
            endcase
            bridge_wr_skid_wrptr <= bridge_wr_skid_wrptr + 2'd1;
        end

        case ({bridge_wr_skid_push_ok, bridge_wr_skid_pop})
            2'b10: bridge_wr_skid_count <= bridge_wr_skid_count + 3'd1;
            2'b01: bridge_wr_skid_count <= bridge_wr_skid_count - 3'd1;
            default: ;
        endcase
    end
end

dcfifo bridge_wr_fifo (
    .wrclk   (clk_74a),
    .wrreq   (bridge_wr_fifo_wrreq),
    .data    (bridge_wr_fifo_wdata),
    .wrfull  (bridge_wr_fifo_full),
    .rdclk   (clk_ram_controller),
    .rdreq   (bridge_wr_fifo_drain),
    .q       (bridge_wr_fifo_q),
    .rdempty (bridge_wr_fifo_empty),
    .aclr    (1'b0),
    .wrusedw (),
    .wrempty (),
    .rdfull  (),
    .rdusedw ()
);
defparam bridge_wr_fifo.intended_device_family = "Cyclone V",
    bridge_wr_fifo.lpm_numwords  = 512,
    bridge_wr_fifo.lpm_showahead = "ON",
    bridge_wr_fifo.lpm_type      = "dcfifo",
    bridge_wr_fifo.lpm_width     = 56,
    bridge_wr_fifo.lpm_widthu    = 9,
    bridge_wr_fifo.overflow_checking  = "ON",
    bridge_wr_fifo.underflow_checking = "ON",
    bridge_wr_fifo.rdsync_delaypipe   = 5,
    bridge_wr_fifo.wrsync_delaypipe   = 5,
    bridge_wr_fifo.use_eab       = "ON";

// Synchronize skid-queue nonempty flag into RAM clock domain
reg [2:0] bridge_wr_skid_nonempty_sync;
always @(posedge clk_ram_controller) begin
    bridge_wr_skid_nonempty_sync <= {bridge_wr_skid_nonempty_sync[1:0], bridge_wr_skid_nonempty_74a};
end
wire bridge_wr_skid_nonempty = bridge_wr_skid_nonempty_sync[2];

wire bridge_wr_idle = !bridge_wr_skid_nonempty && bridge_m_wr_idle;

// Bridge DMA active tracking
reg bridge_dma_active;
reg cpu_ds_read_prev, cpu_ds_write_prev, cpu_ds_open_prev;
reg [2:0] ds_done_ram_sync;
reg [9:0] ds_done_quiet_count;
reg       ds_done_quiet_reached;
reg [7:0] ds_done_blanking;
localparam [9:0] DS_DONE_QUIET_CYCLES = 10'd1023;
localparam [7:0] DS_DONE_BLANKING_CYCLES = 8'd128;
wire cpu_ds_read_start = cpu_target_dataslot_read && !cpu_ds_read_prev;
wire cpu_ds_write_start = cpu_target_dataslot_write && !cpu_ds_write_prev;
wire cpu_ds_open_start = cpu_target_dataslot_openfile && !cpu_ds_open_prev;
wire cpu_ds_start = cpu_ds_read_start || cpu_ds_write_start || cpu_ds_open_start;
wire ds_done_blanking_active = (ds_done_blanking != 8'd0);
wire target_dataslot_done_safe = ds_done_ram_sync[2] && ds_done_quiet_reached;
always @(posedge clk_ram_controller) begin
    cpu_ds_read_prev <= cpu_target_dataslot_read;
    cpu_ds_write_prev <= cpu_target_dataslot_write;
    cpu_ds_open_prev <= cpu_target_dataslot_openfile;

    if (!reset_n_apf) begin
        bridge_dma_active <= 1'b0;
        ds_done_ram_sync <= 3'b000;
        ds_done_quiet_count <= 10'd0;
        ds_done_quiet_reached <= 1'b0;
        ds_done_blanking <= 8'd0;
    end else begin
        ds_done_ram_sync <= {ds_done_ram_sync[1:0], target_dataslot_done};

        if (ds_done_blanking != 8'd0)
            ds_done_blanking <= ds_done_blanking - 8'd1;

        if (cpu_ds_start) begin
            ds_done_quiet_count <= 10'd0;
            ds_done_quiet_reached <= 1'b0;
            ds_done_blanking <= DS_DONE_BLANKING_CYCLES;

            if (cpu_ds_read_start || cpu_ds_write_start)
                bridge_dma_active <= 1'b1;
        end else if (!ds_done_blanking_active) begin
            if (ds_done_ram_sync[2]) begin
                if (bridge_wr_idle) begin
                    if (!ds_done_quiet_reached) begin
                        ds_done_quiet_count <= ds_done_quiet_count + 10'd1;
                        if (ds_done_quiet_count == DS_DONE_QUIET_CYCLES - 10'd1)
                            ds_done_quiet_reached <= 1'b1;
                    end
                end else begin
                    ds_done_quiet_count <= 10'd0;
                    ds_done_quiet_reached <= 1'b0;
                end
            end else begin
                ds_done_quiet_count <= 10'd0;
                ds_done_quiet_reached <= 1'b0;
            end

            if (bridge_dma_active && target_dataslot_done_safe)
                bridge_dma_active <= 1'b0;
        end
    end
end

// Bridge SDRAM read and CRAM0 write handshake CDC (unchanged)
reg [31:0] bridge_addr_captured;
reg [31:0] bridge_wr_data_captured;
reg bridge_sdram_rd;
reg bridge_psram_wr;      // CRAM0 write (single-word handshake, low throughput OK)
reg [31:0] bridge_addr_ram_clk;
reg bridge_rd_done;
reg bridge_rd_done_sync1, bridge_rd_done_sync2;
reg [31:0] bridge_rd_data_captured;

// CRAM1 read: request FIFO (clk_74a→ram_controller) + register response
// The drain FSM reads PSRAM and writes bridge_rd_data_captured directly.
// The APF host is slow enough (~1.6µs per word over SPI) that the drain FSM
// always completes before the next bridge_rd.
wire        cram1_rd_req_fifo_full;

always @(posedge clk_74a) begin
    bridge_rd_done_sync1 <= bridge_rd_done;
    bridge_rd_done_sync2 <= bridge_rd_done_sync1;
    bridge_psram_wr_done_sync1 <= bridge_psram_wr_done;
    bridge_psram_wr_done_sync2 <= bridge_psram_wr_done_sync1;
    if (bridge_rd_done_sync2) bridge_sdram_rd <= 0;
    if (bridge_psram_wr_done_sync2) bridge_psram_wr <= 0;

    // CRAM0 write (bridge address 0x20xxxxxx) — single-word handshake
    if (!bridge_psram_wr && bridge_wr && bridge_addr[31:24] == 8'h20) begin
        bridge_psram_wr <= 1;
        bridge_addr_captured <= bridge_addr;
        bridge_wr_data_captured <= bridge_wr_data;
    end

    // SDRAM read (0x00-0x03) — single-word handshake
    if (!bridge_sdram_rd && bridge_rd && bridge_addr[31:26] == 6'b000000) begin
        bridge_sdram_rd <= 1;
        bridge_addr_captured <= bridge_addr;
    end
end

// CRAM1 read request FIFO: push on RISING EDGE of bridge_rd for 0x30xxxxxx.
// Level-sensitive push would fire every clk_74a cycle while bridge_rd is HIGH,
// flooding the request FIFO with duplicates and causing stale responses to
// pollute subsequent sequential reads during save readback.
reg prev_bridge_rd_for_cram1;
always @(posedge clk_74a)
    prev_bridge_rd_for_cram1 <= bridge_rd;

wire cram1_rd_req_push = ~prev_bridge_rd_for_cram1 && bridge_rd
                        && (bridge_addr[31:24] == 8'h30)
                        && !cram1_rd_req_fifo_full;
wire [21:0] cram1_rd_req_data = bridge_addr[23:2]; // 22-bit word address

// 4-stage synchronizer for SDRAM reads and CRAM0 writes
reg bridge_rd_sync1, bridge_rd_sync2, bridge_rd_sync3, bridge_rd_sync4;
reg bridge_psram_wr_sync1, bridge_psram_wr_sync2, bridge_psram_wr_sync3, bridge_psram_wr_sync4;
reg bridge_psram_wr_done, bridge_psram_wr_done_sync1, bridge_psram_wr_done_sync2;
reg [31:0] bridge_psram_addr_ram_clk;
reg [31:0] bridge_psram_wr_data_ram_clk;

reg [31:0] bridge_addr_sync1, bridge_addr_sync2;
reg [31:0] bridge_wr_data_sync1, bridge_wr_data_sync2;

always @(posedge clk_ram_controller) begin
    bridge_rd_sync1 <= bridge_sdram_rd;
    bridge_rd_sync2 <= bridge_rd_sync1;
    bridge_rd_sync3 <= bridge_rd_sync2;
    bridge_rd_sync4 <= bridge_rd_sync3;

    bridge_psram_wr_sync1 <= bridge_psram_wr;
    bridge_psram_wr_sync2 <= bridge_psram_wr_sync1;
    bridge_psram_wr_sync3 <= bridge_psram_wr_sync2;
    bridge_psram_wr_sync4 <= bridge_psram_wr_sync3;

    // Capture address/data on rising edge of bridge request
    if ((bridge_rd_sync2 && !bridge_rd_sync3) ||
        (bridge_psram_wr_sync2 && !bridge_psram_wr_sync3)) begin
        bridge_addr_sync1 <= bridge_addr_captured;
    end
    if (bridge_psram_wr_sync2 && !bridge_psram_wr_sync3) begin
        bridge_wr_data_sync1 <= bridge_wr_data_captured;
    end
    bridge_addr_sync2 <= bridge_addr_sync1;
    bridge_wr_data_sync2 <= bridge_wr_data_sync1;

    if (bridge_rd_sync3 && !bridge_rd_sync4) begin
        bridge_addr_ram_clk <= bridge_addr_sync2;
    end

    if (bridge_psram_wr_sync3 && !bridge_psram_wr_sync4) begin
        bridge_psram_addr_ram_clk <= bridge_addr_sync2;
        bridge_psram_wr_data_ram_clk <= bridge_wr_data_sync2;
    end

    // SDRAM read complete → capture data
    if (bridge_direct_rd_complete) begin
        bridge_rd_data_captured <= bridge_direct_rd_data;
        bridge_rd_done <= 1;
    end
    if (!bridge_rd_sync1) begin
        bridge_rd_done <= 0;
    end

    // (CRAM1 reads now use response FIFO — see below)
end

// Bridge CRAM0 write active signal (single-word handshake)
wire bridge_psram_wr_active = bridge_psram_wr_sync3 | bridge_psram_wr_sync4 | bridge_psram_wr_done | bridge_psram_write_pending;

reg bridge_psram_write_pending;
reg bridge_psram_write_started;

always @(posedge clk_ram_controller) begin
    if (bridge_psram_wr_sync4 && !bridge_psram_wr_done && !bridge_psram_write_pending) begin
        bridge_psram_write_pending <= 1;
        bridge_psram_write_started <= 0;
    end else if (bridge_psram_write_pending) begin
        if (!bridge_psram_write_started && psram_mux_busy) begin
            bridge_psram_write_started <= 1;
        end else if (bridge_psram_write_started && !psram_mux_busy) begin
            bridge_psram_write_pending <= 0;
            bridge_psram_write_started <= 0;
            bridge_psram_wr_done <= 1;
        end
    end

    if (!bridge_psram_wr_sync1) bridge_psram_wr_done <= 0;
end

// ============================================================
// Bridge CRAM1 Write Path: dcfifo (clk_74a -> clk_ram_controller)
// Mirrors the SDRAM write FIFO pattern for bulk nonvolatile loads.
// ============================================================
wire bridge_cram1_wr_detect = bridge_wr && (bridge_addr[31:24] == 8'h30);

// Skid buffer (4-entry) in clk_74a domain
localparam integer CRAM1_WR_SKID_DEPTH = 4;
reg [55:0] cram1_wr_skid_data [0:CRAM1_WR_SKID_DEPTH-1];
reg [1:0]  cram1_wr_skid_wrptr;
reg [1:0]  cram1_wr_skid_rdptr;
reg [2:0]  cram1_wr_skid_count;
wire       cram1_wr_skid_empty = (cram1_wr_skid_count == 0);
wire       cram1_wr_skid_nonempty_74a = !cram1_wr_skid_empty;
wire       cram1_wr_skid_pop = !cram1_wr_skid_empty && !cram1_wr_fifo_full;
wire [55:0] cram1_wr_skid_head =
            (cram1_wr_skid_rdptr == 2'd0) ? cram1_wr_skid_data[0] :
            (cram1_wr_skid_rdptr == 2'd1) ? cram1_wr_skid_data[1] :
            (cram1_wr_skid_rdptr == 2'd2) ? cram1_wr_skid_data[2] :
                                             cram1_wr_skid_data[3];
wire       cram1_wr_skid_push = bridge_cram1_wr_detect;
wire       cram1_wr_skid_has_space = (cram1_wr_skid_count != 3'd4);
wire       cram1_wr_skid_push_ok = cram1_wr_skid_push &&
                                   (cram1_wr_skid_has_space || cram1_wr_skid_pop);

wire       cram1_wr_fifo_full;
wire       cram1_wr_fifo_empty;
wire [55:0] cram1_wr_fifo_q;

// No reset_n_apf gate: the bridge writes save data BEFORE reset exit,
// so the skid buffer must accept writes even while the core is in reset.
always @(posedge clk_74a) begin
    if (cram1_wr_skid_pop)
        cram1_wr_skid_rdptr <= cram1_wr_skid_rdptr + 2'd1;

    if (cram1_wr_skid_push_ok) begin
        case (cram1_wr_skid_wrptr)
            2'd0: cram1_wr_skid_data[0] <= {bridge_addr[23:2], 2'b00, bridge_wr_data[31:0]};
            2'd1: cram1_wr_skid_data[1] <= {bridge_addr[23:2], 2'b00, bridge_wr_data[31:0]};
            2'd2: cram1_wr_skid_data[2] <= {bridge_addr[23:2], 2'b00, bridge_wr_data[31:0]};
            default: cram1_wr_skid_data[3] <= {bridge_addr[23:2], 2'b00, bridge_wr_data[31:0]};
        endcase
        cram1_wr_skid_wrptr <= cram1_wr_skid_wrptr + 2'd1;
    end

    case ({cram1_wr_skid_push_ok, cram1_wr_skid_pop})
        2'b10: cram1_wr_skid_count <= cram1_wr_skid_count + 3'd1;
        2'b01: cram1_wr_skid_count <= cram1_wr_skid_count - 3'd1;
        default: ;
    endcase
end

// Async FIFO: clk_74a → clk_ram_controller (512 entries, 56-bit: 24-bit addr + 32-bit data)
wire cram1_wr_fifo_drain;

dcfifo cram1_wr_fifo (
    .wrclk   (clk_74a),
    .wrreq   (cram1_wr_skid_pop),
    .data    (cram1_wr_skid_head),
    .wrfull  (cram1_wr_fifo_full),
    .rdclk   (clk_ram_controller),
    .rdreq   (cram1_wr_fifo_drain),
    .q       (cram1_wr_fifo_q),
    .rdempty (cram1_wr_fifo_empty),
    .aclr    (1'b0),
    .wrusedw (), .wrempty (), .rdfull (), .rdusedw ()
);
defparam cram1_wr_fifo.intended_device_family = "Cyclone V",
    cram1_wr_fifo.lpm_numwords  = 512,
    cram1_wr_fifo.lpm_showahead = "ON",
    cram1_wr_fifo.lpm_type      = "dcfifo",
    cram1_wr_fifo.lpm_width     = 56,
    cram1_wr_fifo.lpm_widthu    = 9,
    cram1_wr_fifo.overflow_checking  = "ON",
    cram1_wr_fifo.underflow_checking = "ON",
    cram1_wr_fifo.rdsync_delaypipe   = 5,
    cram1_wr_fifo.wrsync_delaypipe   = 5,
    cram1_wr_fifo.use_eab       = "ON";

// Synchronize skid-queue nonempty flag into RAM clock domain
reg [2:0] cram1_wr_skid_nonempty_sync;
always @(posedge clk_ram_controller) begin
    cram1_wr_skid_nonempty_sync <= {cram1_wr_skid_nonempty_sync[1:0], cram1_wr_skid_nonempty_74a};
end
wire cram1_wr_skid_nonempty = cram1_wr_skid_nonempty_sync[2];

// CRAM1 write drain FSM: pop from FIFO, write to psram1, wait for completion.
// Follows the same pattern as the CRAM0 bridge write handshake:
//   pending=1 → drive wr, wait for busy high → started=1 → wait for busy low → done
reg        cram1_wr_pending;
reg        cram1_wr_started;
reg [21:0] cram1_wr_addr_r;
reg [31:0] cram1_wr_data_r;

assign cram1_wr_fifo_drain = !cram1_wr_fifo_empty && !cram1_wr_pending;

always @(posedge clk_ram_controller) begin
    if (!cram1_wr_pending) begin
        // Pop next entry from FIFO
        if (!cram1_wr_fifo_empty) begin
            cram1_wr_addr_r <= cram1_wr_fifo_q[55:34];
            cram1_wr_data_r <= cram1_wr_fifo_q[31:0];
            cram1_wr_pending <= 1;
            cram1_wr_started <= 0;
        end
    end else begin
        // Drive wr=1 until psram1 accepts (busy goes high)
        if (!cram1_wr_started && psram1_busy) begin
            cram1_wr_started <= 1;
        end else if (cram1_wr_started && !psram1_busy) begin
            // Write complete
            cram1_wr_pending <= 0;
            cram1_wr_started <= 0;
        end
    end
end

wire cram1_bridge_wr_active = cram1_wr_pending | !cram1_wr_fifo_empty | cram1_wr_skid_nonempty;

// ============================================================
// Bridge CRAM1 Read Path: FIFO-based (no CDC race)
// Request FIFO (clk_74a → clk_ram_controller): 22-bit word addresses
// Response FIFO (clk_ram_controller → clk_74a): 32-bit data words
// Bridge reads response FIFO head (showahead); each bridge_rd pops it.
// ============================================================

// Request FIFO: clk_74a write side, clk_ram_controller read side
wire        cram1_rd_req_fifo_empty;
wire [21:0] cram1_rd_req_fifo_q;
wire        cram1_rd_req_drain;

dcfifo cram1_rd_req_fifo (
    .wrclk   (clk_74a),
    .wrreq   (cram1_rd_req_push),
    .data    (cram1_rd_req_data),
    .wrfull  (cram1_rd_req_fifo_full),
    .rdclk   (clk_ram_controller),
    .rdreq   (cram1_rd_req_drain),
    .q       (cram1_rd_req_fifo_q),
    .rdempty (cram1_rd_req_fifo_empty),
    .aclr    (1'b0)
);
defparam cram1_rd_req_fifo.intended_device_family = "Cyclone V",
    cram1_rd_req_fifo.lpm_numwords  = 64,
    cram1_rd_req_fifo.lpm_showahead = "ON",
    cram1_rd_req_fifo.lpm_type      = "dcfifo",
    cram1_rd_req_fifo.lpm_width     = 22,
    cram1_rd_req_fifo.lpm_widthu    = 6,
    cram1_rd_req_fifo.overflow_checking  = "ON",
    cram1_rd_req_fifo.underflow_checking = "ON",
    cram1_rd_req_fifo.rdsync_delaypipe   = 5,
    cram1_rd_req_fifo.wrsync_delaypipe   = 5,
    cram1_rd_req_fifo.use_eab       = "ON";

// Read drain FSM: pop address from request FIFO, read PSRAM, push result
// to response FIFO (clk_ram_controller → clk_74a).
reg        cram1_rd_pending;
reg        cram1_rd_started;
reg [21:0] cram1_rd_addr_r;
reg        cram1_rd_resp_push;  // push pulse for response FIFO
reg [31:0] cram1_rd_resp_wdata; // data to push

assign cram1_rd_req_drain = !cram1_rd_req_fifo_empty && !cram1_rd_pending;

always @(posedge clk_ram_controller) begin
    cram1_rd_resp_push <= 0;

    if (!cram1_rd_pending) begin
        if (!cram1_rd_req_fifo_empty) begin
            cram1_rd_addr_r <= cram1_rd_req_fifo_q;
            cram1_rd_pending <= 1;
            cram1_rd_started <= 0;
        end
    end else begin
        if (!cram1_rd_started && psram1_busy)
            cram1_rd_started <= 1;
        if (psram1_rdata_valid) begin
            cram1_rd_resp_wdata <= psram1_rdata;
            cram1_rd_resp_push <= 1;
            cram1_rd_pending <= 0;
            cram1_rd_started <= 0;
        end
    end
end

// Response FIFO: clk_ram_controller → clk_74a (32-bit data words)
wire        cram1_rd_resp_empty;
wire [31:0] cram1_rd_resp_q;
reg         cram1_rd_resp_pop;

dcfifo cram1_rd_resp_fifo (
    .wrclk   (clk_ram_controller),
    .wrreq   (cram1_rd_resp_push),
    .data    (cram1_rd_resp_wdata),
    .rdclk   (clk_74a),
    .rdreq   (cram1_rd_resp_pop),
    .q       (cram1_rd_resp_q),
    .rdempty (cram1_rd_resp_empty),
    .aclr    (1'b0),
    .wrfull  (), .wrempty (), .rdfull (), .rdusedw (), .wrusedw ()
);
defparam cram1_rd_resp_fifo.intended_device_family = "Cyclone V",
    cram1_rd_resp_fifo.lpm_numwords  = 4,
    cram1_rd_resp_fifo.lpm_showahead = "ON",
    cram1_rd_resp_fifo.lpm_type      = "dcfifo",
    cram1_rd_resp_fifo.lpm_width     = 32,
    cram1_rd_resp_fifo.lpm_widthu    = 2,
    cram1_rd_resp_fifo.overflow_checking  = "ON",
    cram1_rd_resp_fifo.underflow_checking = "ON",
    cram1_rd_resp_fifo.rdsync_delaypipe   = 5,
    cram1_rd_resp_fifo.wrsync_delaypipe   = 5,
    cram1_rd_resp_fifo.use_eab       = "OFF";

// APF side: pop response FIFO when data arrives, latch into bridge_rd_data
reg [31:0] cram1_rd_resp_data;
always @(posedge clk_74a) begin
    cram1_rd_resp_pop <= 0;
    if (!cram1_rd_resp_empty && !cram1_rd_resp_pop) begin
        cram1_rd_resp_pop <= 1;
    end
    if (cram1_rd_resp_pop) begin
        cram1_rd_resp_data <= cram1_rd_resp_q;
    end
end

wire bridge_cram1_rd_active_flag = cram1_rd_pending | !cram1_rd_req_fifo_empty;

// Combined CRAM1 bridge activity (write FIFO draining or read in progress)
wire bridge_cram1_active = cram1_bridge_wr_active | bridge_cram1_rd_active_flag;

// ============================================================
// PSRAM mux: Bank 0 (CRAM0) with bridge write priority,
//            Bank 1 (CRAM1) with bridge FIFO write / read priority
// ============================================================
wire cpu_on_bank0 = (cpu_psram_bank == 1'b0);

// CRAM0 mux: bridge writes have priority, CPU when idle and on bank 0
assign psram_mux_rd = (bridge_psram_wr_active || !cpu_on_bank0) ? 1'b0 : cpu_psram_rd;
assign psram_mux_wr = bridge_psram_write_pending ? 1'b1 : (cpu_on_bank0 ? cpu_psram_wr : 1'b0);
assign psram_mux_addr = bridge_psram_write_pending ? bridge_psram_addr_ram_clk[23:2] : cpu_psram_addr;
assign psram_mux_wdata = bridge_psram_write_pending ? bridge_psram_wr_data_ram_clk : cpu_psram_wdata;
assign psram_mux_wstrb = bridge_psram_write_pending ? 4'b1111 : cpu_psram_wstrb;

// CRAM1 mux: bridge write FIFO drain and read FIFO drain have priority over CPU
assign psram1_rd = (cram1_rd_pending && !cram1_wr_pending) ? 1'b1 :
                   (bridge_cram1_active || cpu_on_bank0) ? 1'b0 : cpu_psram_rd;
assign psram1_wr = cram1_wr_pending ? 1'b1 :
                   (bridge_cram1_active || cpu_on_bank0) ? 1'b0 : cpu_psram_wr;
assign psram1_addr = cram1_wr_pending ? cram1_wr_addr_r :
                     cram1_rd_pending ? cram1_rd_addr_r :
                     cpu_psram_addr;
assign psram1_wdata = cram1_wr_pending ? cram1_wr_data_r : cpu_psram_wdata;
assign psram1_wstrb = cram1_wr_pending ? 4'b1111 : cpu_psram_wstrb;

// Feedback to CPU: select based on bank
assign cpu_psram_rdata = cpu_on_bank0 ? psram_mux_rdata : psram1_rdata;
assign cpu_psram_busy = cpu_on_bank0 ? (bridge_psram_wr_active | psram_mux_busy) :
                                       (bridge_cram1_active | psram1_busy);
assign cpu_psram_rdata_valid = cpu_on_bank0 ? psram_mux_rdata_valid : psram1_rdata_valid;


//
// host/target command handler
//
    wire            reset_n_apf;
    wire    [31:0]  cmd_bridge_rd_data;

    wire reset_n = reset_n_apf;

    // Shutdown handshake CDC: bridge (clk_74a) ↔ CPU (clk_cpu)
    wire shutdown_pending_74a;  // from core_bridge_cmd (clk_74a domain)
    wire shutdown_ack_cpu;      // from axi_periph_slave (clk_cpu domain)

    // Sync shutdown_pending from clk_74a → clk_cpu
    wire shutdown_pending_cpu;
    synch_3 sync_shutdown_pending(shutdown_pending_74a, shutdown_pending_cpu, clk_cpu);

    // Sync shutdown_ack from clk_cpu → clk_74a
    wire shutdown_ack_74a;
    synch_3 sync_shutdown_ack(shutdown_ack_cpu, shutdown_ack_74a, clk_74a);

    wire            status_boot_done = pll_core_locked_s;
    wire            status_setup_done = pll_core_locked_s;
    wire            status_running = reset_n;

    wire            dataslot_requestread;
    wire    [15:0]  dataslot_requestread_id;
    wire            dataslot_requestread_ack = 1;
    wire            dataslot_requestread_ok = 1;

    wire            dataslot_requestwrite;
    wire    [15:0]  dataslot_requestwrite_id;
    wire    [31:0]  dataslot_requestwrite_size;
    wire            dataslot_requestwrite_ack = 1;
    wire            dataslot_requestwrite_ok = 1;

    wire            dataslot_update;
    wire    [15:0]  dataslot_update_id;
    wire    [31:0]  dataslot_update_size;

    wire            dataslot_allcomplete;

    wire     [31:0] rtc_epoch_seconds;
    wire     [31:0] rtc_date_bcd;
    wire     [31:0] rtc_time_bcd;
    wire            rtc_valid;

    wire            savestate_supported;
    wire    [31:0]  savestate_addr;
    wire    [31:0]  savestate_size;
    wire    [31:0]  savestate_maxloadsize;

    wire            savestate_start;
    wire            savestate_start_ack;
    wire            savestate_start_busy;
    wire            savestate_start_ok;
    wire            savestate_start_err;

    wire            savestate_load;
    wire            savestate_load_ack;
    wire            savestate_load_busy;
    wire            savestate_load_ok;
    wire            savestate_load_err;

    wire            osnotify_inmenu;

    // CPU-side signals (in clk_ram_controller domain)
    wire            cpu_target_dataslot_read;
    wire            cpu_target_dataslot_write;
    wire            cpu_target_dataslot_openfile;
    wire    [15:0]  cpu_target_dataslot_id;
    wire    [31:0]  cpu_target_dataslot_slotoffset;
    wire    [31:0]  cpu_target_dataslot_bridgeaddr;
    wire    [31:0]  cpu_target_dataslot_length;
    wire    [31:0]  cpu_target_buffer_param_struct;
    wire    [31:0]  cpu_target_buffer_resp_struct;

    // Bridge-side signals (in clk_74a domain)
    wire            target_dataslot_ack;
    wire            target_dataslot_done;
    wire    [2:0]   target_dataslot_err;

    wire            target_dataslot_read;
    wire            target_dataslot_write;
    wire            target_dataslot_openfile;
    wire            target_dataslot_getfile = 0;

    synch_3 sync_ds_read(cpu_target_dataslot_read, target_dataslot_read, clk_74a);
    synch_3 sync_ds_write(cpu_target_dataslot_write, target_dataslot_write, clk_74a);
    synch_3 sync_ds_openfile(cpu_target_dataslot_openfile, target_dataslot_openfile, clk_74a);

    reg [15:0]  cpu_ds_id_sync1, cpu_ds_id_sync2;
    reg [31:0]  cpu_ds_slotoffset_sync1, cpu_ds_slotoffset_sync2;
    reg [31:0]  cpu_ds_bridgeaddr_sync1, cpu_ds_bridgeaddr_sync2;
    reg [31:0]  cpu_ds_length_sync1, cpu_ds_length_sync2;
    reg [31:0]  cpu_ds_param_sync1, cpu_ds_param_sync2;
    reg [31:0]  cpu_ds_resp_sync1, cpu_ds_resp_sync2;

    reg target_dataslot_read_1, target_dataslot_write_1, target_dataslot_openfile_1;
    reg [15:0]  target_dataslot_id;
    reg [31:0]  target_dataslot_slotoffset;
    reg [31:0]  target_dataslot_bridgeaddr;
    reg [31:0]  target_dataslot_length;
    reg [31:0]  target_buffer_param_struct;
    reg [31:0]  target_buffer_resp_struct;

    always @(posedge clk_74a) begin
        cpu_ds_id_sync1 <= cpu_target_dataslot_id;
        cpu_ds_id_sync2 <= cpu_ds_id_sync1;
        cpu_ds_slotoffset_sync1 <= cpu_target_dataslot_slotoffset;
        cpu_ds_slotoffset_sync2 <= cpu_ds_slotoffset_sync1;
        cpu_ds_bridgeaddr_sync1 <= cpu_target_dataslot_bridgeaddr;
        cpu_ds_bridgeaddr_sync2 <= cpu_ds_bridgeaddr_sync1;
        cpu_ds_length_sync1 <= cpu_target_dataslot_length;
        cpu_ds_length_sync2 <= cpu_ds_length_sync1;
        cpu_ds_param_sync1 <= cpu_target_buffer_param_struct;
        cpu_ds_param_sync2 <= cpu_ds_param_sync1;
        cpu_ds_resp_sync1 <= cpu_target_buffer_resp_struct;
        cpu_ds_resp_sync2 <= cpu_ds_resp_sync1;

        target_dataslot_read_1 <= target_dataslot_read;
        target_dataslot_write_1 <= target_dataslot_write;
        target_dataslot_openfile_1 <= target_dataslot_openfile;

        if ((target_dataslot_read && !target_dataslot_read_1) ||
            (target_dataslot_write && !target_dataslot_write_1) ||
            (target_dataslot_openfile && !target_dataslot_openfile_1)) begin
            target_dataslot_id <= cpu_ds_id_sync2;
            target_dataslot_slotoffset <= cpu_ds_slotoffset_sync2;
            target_dataslot_bridgeaddr <= cpu_ds_bridgeaddr_sync2;
            target_dataslot_length <= cpu_ds_length_sync2;
            target_buffer_param_struct <= cpu_ds_param_sync2;
            target_buffer_resp_struct <= cpu_ds_resp_sync2;
        end
    end

    reg     [9:0]   datatable_addr;
    wire    [31:0]  datatable_q;
    reg             datatable_wren;
    reg     [31:0]  datatable_data;

// Write save slot size to datatable so the bridge knows how many bytes to
// read back at shutdown.  The bridge gets the DMA address from data.json,
// but reads the ACTUAL size from the datatable at runtime.
// Datatable layout: slot_index * 2 + 0 = address, slot_index * 2 + 1 = size.
// Our save slot ("Saves", id 5) is at array index 5 in data.json's data_slots[].
// NES/PCEngine CD cores both write size here — without it, bridge sees 0 and
// skips readback entirely (no .sav file created).
// Datatable entries for both nonvolatile slots.
// Alternates between writing save size and config size each clock cycle.
reg datatable_sel;
always @(posedge clk_74a) begin
    datatable_wren <= 1;
    datatable_sel  <= ~datatable_sel;
    if (!datatable_sel) begin
        datatable_addr <= 5 * 2 + 1;    // array index 5 (Saves), size entry
        datatable_data <= 32'h060000;   // 384KB = 6 × 64KB
    end else begin
        datatable_addr <= 6 * 2 + 1;    // array index 6 (Settings), size entry
        datatable_data <= 32'h001000;   // 4KB config
    end
end

core_bridge_cmd icb (

    .clk                ( clk_74a ),
    .reset_n            ( reset_n_apf ),

    .bridge_endian_little   ( bridge_endian_little ),
    .bridge_addr            ( bridge_addr ),
    .bridge_rd              ( bridge_rd ),
    .bridge_rd_data         ( cmd_bridge_rd_data ),
    .bridge_wr              ( bridge_wr ),
    .bridge_wr_data         ( bridge_wr_data ),

    .status_boot_done       ( status_boot_done ),
    .status_setup_done      ( status_setup_done ),
    .status_running         ( status_running ),

    .dataslot_requestread       ( dataslot_requestread ),
    .dataslot_requestread_id    ( dataslot_requestread_id ),
    .dataslot_requestread_ack   ( dataslot_requestread_ack ),
    .dataslot_requestread_ok    ( dataslot_requestread_ok ),

    .dataslot_requestwrite      ( dataslot_requestwrite ),
    .dataslot_requestwrite_id   ( dataslot_requestwrite_id ),
    .dataslot_requestwrite_size ( dataslot_requestwrite_size ),
    .dataslot_requestwrite_ack  ( dataslot_requestwrite_ack ),
    .dataslot_requestwrite_ok   ( dataslot_requestwrite_ok ),

    .dataslot_update            ( dataslot_update ),
    .dataslot_update_id         ( dataslot_update_id ),
    .dataslot_update_size       ( dataslot_update_size ),

    .dataslot_allcomplete   ( dataslot_allcomplete ),

    .rtc_epoch_seconds      ( rtc_epoch_seconds ),
    .rtc_date_bcd           ( rtc_date_bcd ),
    .rtc_time_bcd           ( rtc_time_bcd ),
    .rtc_valid              ( rtc_valid ),

    .savestate_supported    ( savestate_supported ),
    .savestate_addr         ( savestate_addr ),
    .savestate_size         ( savestate_size ),
    .savestate_maxloadsize  ( savestate_maxloadsize ),

    .savestate_start        ( savestate_start ),
    .savestate_start_ack    ( savestate_start_ack ),
    .savestate_start_busy   ( savestate_start_busy ),
    .savestate_start_ok     ( savestate_start_ok ),
    .savestate_start_err    ( savestate_start_err ),

    .savestate_load         ( savestate_load ),
    .savestate_load_ack     ( savestate_load_ack ),
    .savestate_load_busy    ( savestate_load_busy ),
    .savestate_load_ok      ( savestate_load_ok ),
    .savestate_load_err     ( savestate_load_err ),

    .osnotify_inmenu        ( osnotify_inmenu ),

    .shutdown_pending       ( shutdown_pending_74a ),
    .shutdown_ack_s         ( shutdown_ack_74a ),

    .target_dataslot_read       ( target_dataslot_read ),
    .target_dataslot_write      ( target_dataslot_write ),
    .target_dataslot_getfile    ( target_dataslot_getfile ),
    .target_dataslot_openfile   ( target_dataslot_openfile ),

    .target_dataslot_ack        ( target_dataslot_ack ),
    .target_dataslot_done       ( target_dataslot_done ),
    .target_dataslot_err        ( target_dataslot_err ),

    .target_dataslot_id         ( target_dataslot_id ),
    .target_dataslot_slotoffset ( target_dataslot_slotoffset ),
    .target_dataslot_bridgeaddr ( target_dataslot_bridgeaddr ),
    .target_dataslot_length     ( target_dataslot_length ),

    .target_buffer_param_struct ( target_buffer_param_struct ),
    .target_buffer_resp_struct  ( target_buffer_resp_struct ),

    .datatable_addr         ( datatable_addr ),
    .datatable_wren         ( datatable_wren ),
    .datatable_data         ( datatable_data ),
    .datatable_q            ( datatable_q )

);



////////////////////////////////////////////////////////////////////////////////////////



// video generation
assign video_rgb_clock = clk_core_12288;
assign video_rgb_clock_90 = clk_core_12288_90deg;
assign video_rgb = video_rgb_doom;
assign video_de = vidout_de;
assign video_skip = vidout_skip;
assign video_vs = vidout_vs;
assign video_hs = vidout_hs;

    localparam  VID_V_BPORCH = 'd16;
    localparam  VID_V_ACTIVE = 'd240;
    localparam  VID_V_TOTAL = 'd512;
    localparam  VID_H_BPORCH = 'd40;
    localparam  VID_H_ACTIVE = 'd320;
    localparam  VID_H_TOTAL = 'd400;

    reg [15:0]  frame_count;

    reg [9:0]   x_count;
    reg [9:0]   y_count;

    reg [23:0]  vidout_rgb;
    reg         vidout_de, vidout_de_1;
    reg         vidout_skip;
    reg         vidout_vs;
    reg         vidout_hs, vidout_hs_1;

    // CPU to terminal interface signals
    wire        term_mem_valid;
    wire [31:0] term_mem_addr;
    wire [31:0] term_mem_wdata;
    wire [3:0]  term_mem_wstrb;
    wire [31:0] term_mem_rdata;
    wire        term_mem_ready;

    // Display mode and framebuffer address from CPU
    wire display_mode;
    wire [24:0] fb_display_addr;

    // VexiiRiscv CPU system — AXI4 bus routing
    cpu_system cpu (
        .clk(clk_cpu),
        .reset_n(reset_n),
        // SDRAM AXI4 master interface
        .m_sdram_arvalid(cpu_m_sdram_arvalid),
        .m_sdram_arready(cpu_m_sdram_arready),
        .m_sdram_araddr(cpu_m_sdram_araddr),
        .m_sdram_arlen(cpu_m_sdram_arlen),
        .m_sdram_rvalid(cpu_m_sdram_rvalid),
        .m_sdram_rdata(cpu_m_sdram_rdata),
        .m_sdram_rresp(cpu_m_sdram_rresp),
        .m_sdram_rlast(cpu_m_sdram_rlast),
        .m_sdram_awvalid(cpu_m_sdram_awvalid),
        .m_sdram_awready(cpu_m_sdram_awready),
        .m_sdram_awaddr(cpu_m_sdram_awaddr),
        .m_sdram_awlen(cpu_m_sdram_awlen),
        .m_sdram_wvalid(cpu_m_sdram_wvalid),
        .m_sdram_wready(cpu_m_sdram_wready),
        .m_sdram_wdata(cpu_m_sdram_wdata),
        .m_sdram_wstrb(cpu_m_sdram_wstrb),
        .m_sdram_wlast(cpu_m_sdram_wlast),
        .m_sdram_bvalid(cpu_m_sdram_bvalid),
        .m_sdram_bresp(cpu_m_sdram_bresp),
        // PSRAM AXI4 master interface
        .m_psram_arvalid(cpu_m_psram_arvalid),
        .m_psram_arready(cpu_m_psram_arready),
        .m_psram_araddr(cpu_m_psram_araddr),
        .m_psram_arlen(cpu_m_psram_arlen),
        .m_psram_rvalid(cpu_m_psram_rvalid),
        .m_psram_rdata(cpu_m_psram_rdata),
        .m_psram_rresp(cpu_m_psram_rresp),
        .m_psram_rlast(cpu_m_psram_rlast),
        .m_psram_awvalid(cpu_m_psram_awvalid),
        .m_psram_awready(cpu_m_psram_awready),
        .m_psram_awaddr(cpu_m_psram_awaddr),
        .m_psram_awlen(cpu_m_psram_awlen),
        .m_psram_wvalid(cpu_m_psram_wvalid),
        .m_psram_wready(cpu_m_psram_wready),
        .m_psram_wdata(cpu_m_psram_wdata),
        .m_psram_wstrb(cpu_m_psram_wstrb),
        .m_psram_wlast(cpu_m_psram_wlast),
        .m_psram_bvalid(cpu_m_psram_bvalid),
        .m_psram_bresp(cpu_m_psram_bresp),
        // Local peripheral AXI4 master interface
        .m_local_arvalid(cpu_m_local_arvalid),
        .m_local_arready(cpu_m_local_arready),
        .m_local_araddr(cpu_m_local_araddr),
        .m_local_arlen(cpu_m_local_arlen),
        .m_local_rvalid(cpu_m_local_rvalid),
        .m_local_rdata(cpu_m_local_rdata),
        .m_local_rresp(cpu_m_local_rresp),
        .m_local_rlast(cpu_m_local_rlast),
        .m_local_awvalid(cpu_m_local_awvalid),
        .m_local_awready(cpu_m_local_awready),
        .m_local_awaddr(cpu_m_local_awaddr),
        .m_local_awlen(cpu_m_local_awlen),
        .m_local_wvalid(cpu_m_local_wvalid),
        .m_local_wready(cpu_m_local_wready),
        .m_local_wdata(cpu_m_local_wdata),
        .m_local_wstrb(cpu_m_local_wstrb),
        .m_local_wlast(cpu_m_local_wlast),
        .m_local_bvalid(cpu_m_local_bvalid),
        .m_local_bresp(cpu_m_local_bresp),
        // External interrupt
        .int_m_external(adma_irq)
    );

    // AXI4 peripheral slave
    axi_periph_slave periph (
        .clk(clk_cpu),
        .reset_n(reset_n),
        // AXI4 slave interface
        .s_axi_arvalid(cpu_m_local_arvalid),
        .s_axi_arready(cpu_m_local_arready),
        .s_axi_araddr(cpu_m_local_araddr),
        .s_axi_arlen(cpu_m_local_arlen),
        .s_axi_rvalid(cpu_m_local_rvalid),
        .s_axi_rready(1'b1),
        .s_axi_rdata(cpu_m_local_rdata),
        .s_axi_rresp(cpu_m_local_rresp),
        .s_axi_rlast(cpu_m_local_rlast),
        .s_axi_awvalid(cpu_m_local_awvalid),
        .s_axi_awready(cpu_m_local_awready),
        .s_axi_awaddr(cpu_m_local_awaddr),
        .s_axi_awlen(cpu_m_local_awlen),
        .s_axi_wvalid(cpu_m_local_wvalid),
        .s_axi_wready(cpu_m_local_wready),
        .s_axi_wdata(cpu_m_local_wdata),
        .s_axi_wstrb(cpu_m_local_wstrb),
        .s_axi_wlast(cpu_m_local_wlast),
        .s_axi_bvalid(cpu_m_local_bvalid),
        .s_axi_bready(1'b1),
        .s_axi_bresp(cpu_m_local_bresp),
        // CDC inputs
        .dataslot_allcomplete(dataslot_allcomplete && bridge_wr_idle),
        .vsync(vidout_vs),
        .cont1_key(p1_controls),
        .cont1_joy(p1_joypad),
        .cont1_trig(p1_trigger),
        .cont2_key(p2_controls),
        .cont2_joy(p2_joypad),
        .cont2_trig(p2_trigger),
        .target_dataslot_ack(target_dataslot_ack),
        .target_dataslot_done(target_dataslot_done_safe),
        .target_dataslot_err(target_dataslot_err),
        // Terminal interface
        .term_mem_valid(term_mem_valid),
        .term_mem_addr(term_mem_addr),
        .term_mem_wdata(term_mem_wdata),
        .term_mem_wstrb(term_mem_wstrb),
        .term_mem_rdata(term_mem_rdata),
        .term_mem_ready(term_mem_ready),
        // Display control
        .display_mode(display_mode),
        .fb_display_addr(fb_display_addr),
        // Palette write interface
        .pal_wr(cpu_pal_wr),
        .pal_addr(cpu_pal_addr),
        .pal_data(cpu_pal_data),
        // Target dataslot interface
        .target_dataslot_read(cpu_target_dataslot_read),
        .target_dataslot_write(cpu_target_dataslot_write),
        .target_dataslot_openfile(cpu_target_dataslot_openfile),
        .target_dataslot_id(cpu_target_dataslot_id),
        .target_dataslot_slotoffset(cpu_target_dataslot_slotoffset),
        .target_dataslot_bridgeaddr(cpu_target_dataslot_bridgeaddr),
        .target_dataslot_length(cpu_target_dataslot_length),
        .target_buffer_param_struct(cpu_target_buffer_param_struct),
        .target_buffer_resp_struct(cpu_target_buffer_resp_struct),
        // Audio output interface (CPU MMIO path)
        .audio_sample_wr(audio_cpu_sample_wr),
        .audio_sample_data(audio_cpu_sample_data),
        .audio_buf_level(audio_buf_level),
        // Audio DMA register interface
        .adma_reg_wr(adma_reg_wr),
        .adma_reg_addr(adma_reg_addr),
        .adma_reg_wdata(adma_reg_wdata),
        .adma_reg_rdata(adma_reg_rdata),
        // Link MMIO interface
        .link_reg_wr(link_reg_wr),
        .link_reg_rd(link_reg_rd),
        .link_reg_addr(link_reg_addr),
        .link_reg_wdata(link_reg_wdata),
        .link_reg_rdata(link_reg_rdata),
        // OPL2 hardware interface
        .opl_write_req(opl_write_req),
        .opl_write_addr(opl_write_addr),
        .opl_write_data(opl_write_data),
        .opl_ack(opl_ack),
        .game_id(game_id_sync2),
        .original_mode(original_mode_sync2),
        .run_mode(run_mode_sync2),
        .refresh_rate(refresh_rate_cpu_sync2),
        .vrr_v_total(vrr_v_total),
        .shutdown_pending(shutdown_pending_cpu),
        .shutdown_ack(shutdown_ack_cpu)
    );

    // Slave → io_sdram pulse adapter + bridge direct SDRAM reads
    reg sdram_accepted_r;
    reg sdram_cmd_forwarded;
    reg bridge_direct_rd_active;    // Bridge read issued, waiting for data
    reg bridge_direct_rd_complete;  // Pulse: bridge read data captured
    reg [31:0] bridge_direct_rd_data;

    always @(posedge clk_ram_controller) begin
        ram1_word_rd <= 0;
        ram1_word_wr <= 0;
        ram1_word_burst_len <= 4'd0;
        sdram_accepted_r <= 0;
        bridge_direct_rd_complete <= 0;

        if (!sdram_slave_rd && !sdram_slave_wr)
            sdram_cmd_forwarded <= 0;

        // Priority 1: AXI slave (CPU path)
        if (!ram1_word_busy && !sdram_cmd_forwarded &&
            (sdram_slave_rd || sdram_slave_wr)) begin
            ram1_word_rd <= sdram_slave_rd;
            ram1_word_wr <= sdram_slave_wr;
            ram1_word_addr <= sdram_slave_addr;
            ram1_word_data <= sdram_slave_wdata;
            ram1_word_wstrb <= sdram_slave_wstrb;
            ram1_word_burst_len <= sdram_slave_burst_len;
            sdram_accepted_r <= 1;
            sdram_cmd_forwarded <= 1;
        end
        // Priority 2: Bridge direct SDRAM read (bypasses AXI entirely)
        else if (!ram1_word_busy && !bridge_direct_rd_active &&
                 bridge_rd_sync4 && !bridge_rd_done) begin
            ram1_word_rd <= 1;
            ram1_word_addr <= bridge_addr_ram_clk[25:2];
            bridge_direct_rd_active <= 1;
        end

        // Capture bridge read result
        if (bridge_direct_rd_active && ram1_word_q_valid) begin
            bridge_direct_rd_data <= ram1_word_q;
            bridge_direct_rd_complete <= 1;
            bridge_direct_rd_active <= 0;
        end

        // Clear active when request goes away
        if (!bridge_rd_sync4)
            bridge_direct_rd_active <= 0;
    end

    // AXI4 bridge master
    axi_bridge_master bridge_axi_m (
        .clk(clk_cpu),
        .reset_n(reset_n),
        .fifo_q(bridge_wr_fifo_q),
        .fifo_empty(bridge_wr_fifo_empty),
        .fifo_rdreq(bridge_wr_fifo_drain),
        .bridge_rd_req(1'b0),           // Reads bypass AXI — handled directly
        .bridge_rd_addr(24'b0),
        .bridge_rd_data(),
        .bridge_rd_done(),
        .m_axi_arvalid(bridge_m_arvalid), .m_axi_arready(bridge_m_arready),
        .m_axi_araddr(bridge_m_araddr),   .m_axi_arlen(bridge_m_arlen),
        .m_axi_rvalid(bridge_m_rvalid),   .m_axi_rdata(bridge_m_rdata),
        .m_axi_rresp(bridge_m_rresp),     .m_axi_rlast(bridge_m_rlast),
        .m_axi_awvalid(bridge_m_awvalid), .m_axi_awready(bridge_m_awready),
        .m_axi_awaddr(bridge_m_awaddr),   .m_axi_awlen(bridge_m_awlen),
        .m_axi_wvalid(bridge_m_wvalid),   .m_axi_wready(bridge_m_wready),
        .m_axi_wdata(bridge_m_wdata),     .m_axi_wstrb(bridge_m_wstrb),
        .m_axi_wlast(bridge_m_wlast),
        .m_axi_bvalid(bridge_m_bvalid),   .m_axi_bresp(bridge_m_bresp),
        .idle(bridge_m_idle),
        .wr_idle(bridge_m_wr_idle)
    );

    // AXI4 SDRAM arbiter: M0 (tied off) > M1 (Audio DMA) > CPU(M2) > Bridge(M3)
    axi_sdram_arbiter sdram_arb (
        .clk(clk_cpu),
        .reset_n(reset_n),
        // M0: Tied off (was span rasterizer)
        .m0_arvalid(1'b0), .m0_arready(),
        .m0_araddr(32'b0),  .m0_arlen(8'b0),
        .m0_rvalid(),       .m0_rdata(),
        .m0_rresp(),        .m0_rlast(),
        .m0_awvalid(1'b0), .m0_awready(),
        .m0_awaddr(32'b0),  .m0_awlen(8'b0),
        .m0_wvalid(1'b0),  .m0_wready(),
        .m0_wdata(32'b0),   .m0_wstrb(4'b0),
        .m0_wlast(1'b0),
        .m0_bvalid(),       .m0_bresp(),
        // M1: Audio DMA (read-only from SDRAM)
        .m1_arvalid(adma_m_arvalid), .m1_arready(adma_m_arready),
        .m1_araddr(adma_m_araddr),   .m1_arlen(adma_m_arlen),
        .m1_rvalid(adma_m_rvalid),   .m1_rdata(adma_m_rdata),
        .m1_rresp(),                 .m1_rlast(adma_m_rlast),
        .m1_awvalid(1'b0), .m1_awready(),
        .m1_awaddr(32'b0),  .m1_awlen(8'b0),
        .m1_wvalid(1'b0),  .m1_wready(),
        .m1_wdata(32'b0),   .m1_wstrb(4'b0),
        .m1_wlast(1'b0),
        .m1_bvalid(),       .m1_bresp(),
        // M2: CPU
        .m2_arvalid(cpu_m_sdram_arvalid), .m2_arready(cpu_m_sdram_arready),
        .m2_araddr(cpu_m_sdram_araddr),   .m2_arlen(cpu_m_sdram_arlen),
        .m2_rvalid(cpu_m_sdram_rvalid),   .m2_rdata(cpu_m_sdram_rdata),
        .m2_rresp(cpu_m_sdram_rresp),     .m2_rlast(cpu_m_sdram_rlast),
        .m2_awvalid(cpu_m_sdram_awvalid), .m2_awready(cpu_m_sdram_awready),
        .m2_awaddr(cpu_m_sdram_awaddr),   .m2_awlen(cpu_m_sdram_awlen),
        .m2_wvalid(cpu_m_sdram_wvalid),   .m2_wready(cpu_m_sdram_wready),
        .m2_wdata(cpu_m_sdram_wdata),     .m2_wstrb(cpu_m_sdram_wstrb),
        .m2_wlast(cpu_m_sdram_wlast),
        .m2_bvalid(cpu_m_sdram_bvalid),   .m2_bresp(cpu_m_sdram_bresp),
        // M3: Bridge (lowest priority)
        .m3_arvalid(bridge_m_arvalid), .m3_arready(bridge_m_arready),
        .m3_araddr(bridge_m_araddr),   .m3_arlen(bridge_m_arlen),
        .m3_rvalid(bridge_m_rvalid),   .m3_rdata(bridge_m_rdata),
        .m3_rresp(bridge_m_rresp),     .m3_rlast(bridge_m_rlast),
        .m3_awvalid(bridge_m_awvalid), .m3_awready(bridge_m_awready),
        .m3_awaddr(bridge_m_awaddr),   .m3_awlen(bridge_m_awlen),
        .m3_wvalid(bridge_m_wvalid),   .m3_wready(bridge_m_wready),
        .m3_wdata(bridge_m_wdata),     .m3_wstrb(bridge_m_wstrb),
        .m3_wlast(bridge_m_wlast),
        .m3_bvalid(bridge_m_bvalid),   .m3_bresp(bridge_m_bresp),
        // Slave output (to axi_sdram_slave)
        .s_arvalid(arb_s_arvalid), .s_arready(arb_s_arready),
        .s_araddr(arb_s_araddr),   .s_arlen(arb_s_arlen),
        .s_rvalid(arb_s_rvalid),   .s_rdata(arb_s_rdata),
        .s_rresp(arb_s_rresp),     .s_rlast(arb_s_rlast),
        .s_awvalid(arb_s_awvalid), .s_awready(arb_s_awready),
        .s_awaddr(arb_s_awaddr),   .s_awlen(arb_s_awlen),
        .s_wvalid(arb_s_wvalid),   .s_wready(arb_s_wready),
        .s_wdata(arb_s_wdata),     .s_wstrb(arb_s_wstrb),
        .s_wlast(arb_s_wlast),
        .s_bvalid(arb_s_bvalid),   .s_bresp(arb_s_bresp)
    );

    // AXI4 SDRAM slave
    axi_sdram_slave sdram_axi_slave (
        .clk(clk_cpu),
        .reset_n(reset_n),
        .s_axi_arvalid(arb_s_arvalid),
        .s_axi_arready(arb_s_arready),
        .s_axi_araddr(arb_s_araddr),
        .s_axi_arlen(arb_s_arlen),
        .s_axi_rvalid(arb_s_rvalid),
        .s_axi_rready(1'b1),
        .s_axi_rdata(arb_s_rdata),
        .s_axi_rresp(arb_s_rresp),
        .s_axi_rlast(arb_s_rlast),
        .s_axi_awvalid(arb_s_awvalid),
        .s_axi_awready(arb_s_awready),
        .s_axi_awaddr(arb_s_awaddr),
        .s_axi_awlen(arb_s_awlen),
        .s_axi_wvalid(arb_s_wvalid),
        .s_axi_wready(arb_s_wready),
        .s_axi_wdata(arb_s_wdata),
        .s_axi_wstrb(arb_s_wstrb),
        .s_axi_wlast(arb_s_wlast),
        .s_axi_bvalid(arb_s_bvalid),
        .s_axi_bready(1'b1),
        .s_axi_bresp(arb_s_bresp),
        .sdram_rd(sdram_slave_rd),
        .sdram_wr(sdram_slave_wr),
        .sdram_addr(sdram_slave_addr),
        .sdram_wdata(sdram_slave_wdata),
        .sdram_wstrb(sdram_slave_wstrb),
        .sdram_burst_len(sdram_slave_burst_len),
        .sdram_rdata(ram1_word_q),
        .sdram_busy(ram1_word_busy),
        .sdram_accepted(sdram_accepted_r),
        .sdram_rdata_valid(ram1_word_q_valid)
    );

    // AXI4 PSRAM slave
    axi_psram_slave cpu_psram_axi (
        .clk(clk_cpu),
        .reset_n(reset_n),
        .s_axi_arvalid(cpu_m_psram_arvalid),
        .s_axi_arready(cpu_m_psram_arready),
        .s_axi_araddr(cpu_m_psram_araddr),
        .s_axi_arlen(cpu_m_psram_arlen),
        .s_axi_rvalid(cpu_m_psram_rvalid),
        .s_axi_rready(1'b1),
        .s_axi_rdata(cpu_m_psram_rdata),
        .s_axi_rresp(cpu_m_psram_rresp),
        .s_axi_rlast(cpu_m_psram_rlast),
        .s_axi_awvalid(cpu_m_psram_awvalid),
        .s_axi_awready(cpu_m_psram_awready),
        .s_axi_awaddr(cpu_m_psram_awaddr),
        .s_axi_awlen(cpu_m_psram_awlen),
        .s_axi_wvalid(cpu_m_psram_wvalid),
        .s_axi_wready(cpu_m_psram_wready),
        .s_axi_wdata(cpu_m_psram_wdata),
        .s_axi_wstrb(cpu_m_psram_wstrb),
        .s_axi_wlast(cpu_m_psram_wlast),
        .s_axi_bvalid(cpu_m_psram_bvalid),
        .s_axi_bready(1'b1),
        .s_axi_bresp(cpu_m_psram_bresp),
        .psram_rd(cpu_psram_rd),
        .psram_wr(cpu_psram_wr),
        .psram_addr(cpu_psram_addr),
        .psram_wdata(cpu_psram_wdata),
        .psram_wstrb(cpu_psram_wstrb),
        .psram_rdata(cpu_psram_rdata),
        .psram_busy(cpu_psram_busy),
        .psram_rdata_valid(cpu_psram_rdata_valid),
        .psram_bank(cpu_psram_bank)
    );

    // Terminal display (40x30 characters, 320x240 pixels)
    wire [23:0] terminal_pixel_color;

    text_terminal terminal (
        .clk(clk_core_12288),
        .clk_cpu(clk_cpu),
        .reset_n(reset_n),
        .pixel_x({1'b0,visible_x[9:1]}), // halve x resolution (zero-extend)
        .pixel_y(visible_y),
        .pixel_color(terminal_pixel_color),
        .mem_valid(term_mem_valid),
        .mem_addr(term_mem_addr),
        .mem_wdata(term_mem_wdata),
        .mem_wstrb(term_mem_wstrb),
        .mem_rdata(term_mem_rdata),
        .mem_ready(term_mem_ready)
    );

    // Line start signal for video scanout
    reg line_start;
    always @(posedge clk_core_12288) begin
        line_start <= (x_count == 0);
    end

    // Video scanout from SDRAM framebuffer (8-bit indexed with hardware palette)
    wire [23:0] framebuffer_pixel_color;

    // Palette write signals from CPU
    wire        cpu_pal_wr;
    wire [7:0]  cpu_pal_addr;
    wire [23:0] cpu_pal_data;

    // SDRAM burst interface signals for video scanout
    wire        video_burst_rd;
    wire [24:0] video_burst_addr;
    wire [10:0] video_burst_len;
    wire        video_burst_32bit;
    wire [31:0] video_burst_data;
    wire        video_burst_data_valid;
    wire        video_burst_data_done;
    
    video_CRT_scanout_indexed_BRAM  scanout (
        .clk_video(clk_core_12288),
        .reset_n(reset_n),
        .x_count(x_count),
        .y_count(y_count),
        .line_start(line_start),
        .pixel_color(framebuffer_pixel_color),
        .fb_base_addr(fb_display_addr),
        .clk_sdram(clk_ram_controller),
        .burst_rd(video_burst_rd),
        .burst_addr(video_burst_addr),
        .burst_len(video_burst_len),
        .burst_32bit(video_burst_32bit),
        .burst_data(video_burst_data),
        .burst_data_valid(video_burst_data_valid),
        .burst_data_done(video_burst_data_done),
        .pal_wr(cpu_pal_wr),
        .pal_addr(cpu_pal_addr),
        .pal_data(cpu_pal_data)
    );

        // ---  CRT 15.7kHz / variable Hz Parameters ---
    localparam CRT_V_SYNC   = 3;
    localparam CRT_V_BPORCH = 15;
    localparam CRT_V_FPORCH = 4;
    localparam CRT_V_ACTIVE = 240;
    localparam CRT_V_TOTAL_60 = CRT_V_SYNC + CRT_V_BPORCH + CRT_V_ACTIVE + CRT_V_FPORCH; // 262

    // Dynamic V_TOTAL for refresh rate control (applied at frame boundary)
    // Presets: 60 Hz=262, 55 Hz=286, 50 Hz=315
    // VRR: firmware writes V_TOTAL directly via MMIO
    reg [9:0] crt_v_total;

    wire [9:0] crt_v_total_preset =
        (refresh_rate_vid_sync2[1:0] == 2'd2) ? 10'd315 :  // 50 Hz
        (refresh_rate_vid_sync2[1:0] == 2'd1) ? 10'd286 :  // 55 Hz
                                                 10'd262;   // 60 Hz (default)

    // VRR mode (setting 3): use firmware-written V_TOTAL, hard clamp to safe range.
    // Pocket scaler accepts 42-60 Hz → V_TOTAL range [262, 375].
    wire vrr_active = (refresh_rate_vid_sync2[1:0] == 2'd3);
    wire [9:0] vrr_vt_safe = (vrr_vt_sync2 < 10'd262) ? 10'd262 :
                              (vrr_vt_sync2 > 10'd375) ? 10'd375 :
                              (vrr_vt_sync2 == 10'd0)  ? 10'd262 : vrr_vt_sync2;
    wire [9:0] crt_v_total_next = vrr_active ? vrr_vt_safe : crt_v_total_preset;
    localparam CRT_H_TOTAL  = CRT_H_SYNC + CRT_H_BPORCH + CRT_H_ACTIVE + CRT_H_FPORCH;
    localparam CRT_H_SYNC   = 58;
    localparam CRT_H_BPORCH = 62;
    localparam CRT_H_FPORCH = 20;
    localparam CRT_H_ACTIVE = 640;
    reg crt_hs, crt_vs, crt_de;
    reg crt_hblank, crt_vblank;

    wire [9:0]  visible_x = x_count - CRT_H_SYNC - CRT_H_BPORCH;
    wire [9:0]  visible_y = y_count - CRT_V_SYNC - CRT_V_BPORCH;

always @(posedge clk_core_12288 or negedge reset_n) begin

    if(~reset_n) begin

        x_count <= 0;
        y_count <= 0;
        crt_v_total <= CRT_V_TOTAL_60;

    end else begin
        vidout_de <= 0;
        vidout_skip <= 0;
        vidout_vs <= 0;
        vidout_hs <= 0;

        vidout_hs_1 <= vidout_hs;
        vidout_de_1 <= vidout_de;

        // x and y counters
        x_count <= x_count + 1'b1;
        if(x_count == CRT_H_TOTAL-1) begin
            x_count <= 0;

            y_count <= y_count + 1'b1;
            if(y_count == crt_v_total-1) begin
                y_count <= 0;
                crt_v_total <= crt_v_total_next; // latch new V_TOTAL at frame boundary
            end
        end

        // CRT Blank
        crt_hblank <= x_count < (CRT_H_SYNC + CRT_H_BPORCH) || (x_count >= CRT_H_SYNC + CRT_H_BPORCH + CRT_H_ACTIVE);
        crt_vblank <= y_count < (CRT_V_SYNC + CRT_V_BPORCH) || (y_count >= CRT_V_SYNC + CRT_V_BPORCH + CRT_V_ACTIVE);

        // Generate CRT sync
        // --- Generación de Syncs (Lógica Negativa) ---

        crt_hs <= (x_count >= 0) && (x_count < CRT_H_SYNC);
        crt_vs <= (y_count >= 0) && (y_count < CRT_V_SYNC);


        // Generate Pocket sync
        if(x_count == 0 && y_count == 0) begin
            // sync signal in back porch
            // new frame
            vidout_vs <= 1;
        end

        // we want HS to occur a bit after VS, not on the same cycle
        if(x_count == 3) begin
            // sync signal in back porch
            // new line
            vidout_hs <= 1;
        end

        // inactive screen areas are black
        vidout_rgb <= 24'h0;

        // generate active video, now accounts for CRT specific timings but making compatible with Analogue Pocket video also
        if(x_count >= CRT_H_SYNC + CRT_H_BPORCH  && x_count < CRT_H_SYNC + CRT_H_BPORCH + CRT_H_ACTIVE) begin

            if(y_count >= CRT_V_SYNC + CRT_V_BPORCH && y_count < CRT_V_SYNC + CRT_V_BPORCH + CRT_V_ACTIVE) begin
                // data enable. this is the active region of the line
                vidout_de <= 1;

                // Display mode: 0=terminal overlay, 1=framebuffer only
                if (display_mode) begin
                    // Framebuffer only mode
                    vidout_rgb <= framebuffer_pixel_color;
                end else begin
                    // Terminal overlay mode - white text overlays framebuffer
                    if (terminal_pixel_color == 24'hFFFFFF)
                        vidout_rgb <= terminal_pixel_color;
                    else
                        vidout_rgb <= framebuffer_pixel_color;
                end
            end
        end
    end
end

//
// Link MMIO peripheral
//
link_mmio #(
    .CLK_HZ(100000000),
    .SCK_HZ(256000),
    .POLL_HZ(3000),
    .FIFO_DEPTH(256)
) link0 (
    .clk(clk_cpu),
    .reset_n(reset_n),

    .reg_wr(link_reg_wr),
    .reg_rd(link_reg_rd),
    .reg_addr(link_reg_addr),
    .reg_wdata(link_reg_wdata),
    .reg_rdata(link_reg_rdata),

    .link_si_i(link_si_i),
    .link_so_o(link_so_out),
    .link_so_oe(link_so_oe),
    .link_sck_i(link_sck_i),
    .link_sck_o(link_sck_out),
    .link_sck_oe(link_sck_oe),
    .link_sd_i(link_sd_i),
    .link_sd_o(link_sd_out),
    .link_sd_oe(link_sd_oe)
);

//
// OPL3 hardware synthesizer (Greg Taylor's opl3_fpga, OPL2 compat mode)
//
opl3_wrapper opl3 (
    .clk            (clk_cpu),
    .clk_opl        (clk_core_12288),
    .reset_n        (reset_n),
    .opl_write_req  (opl_write_req),
    .opl_write_addr (opl_write_addr),
    .opl_write_data (opl_write_data),
    .opl_ack            (opl_ack),
    .opl_audio_out      (opl_audio_out),
    .opl_sample_toggle  (opl_sample_toggle)
);

//
// Audio DMA engine — reads pre-mixed samples from SDRAM, feeds ring buffer
//
audio_dma adma (
    .clk           (clk_cpu),
    .reset_n       (reset_n),

    // MMIO registers (from axi_periph_slave at 0x4F)
    .reg_wr        (adma_reg_wr),
    .reg_addr      (adma_reg_addr),
    .reg_wdata     (adma_reg_wdata),
    .reg_rdata     (adma_reg_rdata),

    // AXI4 read master (to SDRAM arbiter M1)
    .m_axi_arvalid (adma_m_arvalid),
    .m_axi_arready (adma_m_arready),
    .m_axi_araddr  (adma_m_araddr),
    .m_axi_arlen   (adma_m_arlen),
    .m_axi_rvalid  (adma_m_rvalid),
    .m_axi_rdata   (adma_m_rdata),
    .m_axi_rlast   (adma_m_rlast),

    // Ring buffer write + level
    .sample_wr     (adma_sample_wr),
    .sample_data   (adma_sample_data),
    .buf_level     (audio_buf_level),

    // Interrupt
    .irq           (adma_irq)
);

//
// Hardware audio upsampler (BRAM ring buffer + 11 kHz → 48 kHz interpolation)
// Single-clock (clk_sys), outputs 48 kHz samples to audio_output FIFO
//
audio_upsample audio_up (
    .clk          (clk_cpu),
    .reset_n      (reset_n),

    .sample_wr    (audio_sample_wr),
    .sample_data  (audio_sample_data),
    .buf_level    (audio_buf_level),

    .out_wr       (audio_up_wr),
    .out_data     (audio_up_data),
    .out_full     (audio_fifo_full)
);

//
// Audio output (dcfifo + I2S) with OPL2 mixing
//
audio_output audio_out (
    .clk_sys      (clk_cpu),
    .clk_audio    (clk_core_12288),
    .reset_n      (reset_n),

    .sample_wr    (audio_up_wr),
    .sample_data  (audio_up_data),
    .fifo_level   (audio_fifo_level),
    .fifo_full    (audio_fifo_full),

    .opl_audio_in     (opl_audio_out),
    .opl_toggle_in    (opl_sample_toggle),

    .audio_mclk   (audio_mclk),
    .audio_lrck   (audio_lrck),
    .audio_dac    (audio_dac)
);


///////////////////////////////////////////////


    wire    clk_core_12288;
    wire    clk_core_12288_90deg;
    wire    clk_core_49152;
    wire    clk_cpu;
    wire    clk_ram_controller;
    wire    clk_ram_chip;

    wire    pll_core_locked;
    wire    pll_ram_locked;
    wire    pll_locked_all = pll_core_locked & pll_ram_locked;
    wire    pll_core_locked_s;
synch_3 s01(pll_locked_all, pll_core_locked_s, clk_74a);

mf_pllbase mp1 (
    .refclk         ( clk_74a ),
    .rst            ( 0 ),

    .outclk_0       ( clk_core_12288 ),
    .outclk_1       ( clk_core_12288_90deg ),

    .outclk_2       ( clk_core_49152),

    .locked         ( pll_core_locked )
);

mf_pllram_133 mp_ram (
    .refclk         ( clk_74a ),
    .rst            ( 0 ),
    .outclk_0       ( clk_ram_controller ),
    .outclk_1       ( clk_ram_chip ),
    .locked         ( pll_ram_locked )
);

assign clk_cpu = clk_ram_controller;


// SDRAM controller
io_sdram isr0 (
    .controller_clk ( clk_ram_controller ),
    .chip_clk       ( clk_ram_chip ),
    .clk_90         ( clk_ram_chip ),
    .reset_n        ( 1'b1 ),

    .phy_cke        ( dram_cke ),
    .phy_clk        ( dram_clk ),
    .phy_cas        ( dram_cas_n ),
    .phy_ras        ( dram_ras_n ),
    .phy_we         ( dram_we_n ),
    .phy_ba         ( dram_ba ),
    .phy_a          ( dram_a ),
    .phy_dq         ( dram_dq ),
    .phy_dqm        ( dram_dqm ),

    // Burst interface - video scanout
    .burst_rd           ( video_burst_rd ),
    .burst_addr         ( video_burst_addr ),
    .burst_len          ( video_burst_len ),
    .burst_32bit        ( video_burst_32bit ),
    .burst_data         ( video_burst_data ),
    .burst_data_valid   ( video_burst_data_valid ),
    .burst_data_done    ( video_burst_data_done ),

    // Burst write interface - not used
    .burstwr        ( 1'b0 ),
    .burstwr_addr   ( 25'b0 ),
    .burstwr_ready  ( ),
    .burstwr_strobe ( 1'b0 ),
    .burstwr_data   ( 16'b0 ),
    .burstwr_done   ( 1'b0 ),

    // Word interface - CPU access via AXI
    .word_rd    ( ram1_word_rd ),
    .word_wr    ( ram1_word_wr ),
    .word_addr  ( ram1_word_addr ),
    .word_data  ( ram1_word_data ),
    .word_wstrb ( ram1_word_wstrb ),
    .word_burst_len ( ram1_word_burst_len ),
    .word_q     ( ram1_word_q ),
    .word_busy  ( ram1_word_busy ),
    .word_q_valid ( ram1_word_q_valid )

);



endmodule
