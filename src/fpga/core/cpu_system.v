//
// VexiiRiscv CPU System — AXI4 bus routing
// - VexiiRiscv RISC-V CPU with 3-bus architecture:
//   FetchL1Axi4 (I-cache, read-only)
//   LsuL1Axi4   (D-cache, read+write)
//   LsuPlugin IO (uncached, single-beat cmd/rsp)
// - Per-bus address decode → {SDRAM, PSRAM, Local} AXI4 masters
//
// All peripheral/local logic (BRAM, colormap, system registers, CDC, terminal,
// DMA/Span/ATM/Audio/Link dispatch) lives in axi_periph_slave.v.
//

`default_nettype none

module cpu_system (
    input wire clk,           // CPU clock (100 MHz)
    input wire reset_n,

    // SDRAM AXI4 master interface (to axi_sdram_slave via core_top)
    output reg         m_sdram_arvalid,
    input  wire        m_sdram_arready,
    output reg  [31:0] m_sdram_araddr,
    output reg  [7:0]  m_sdram_arlen,

    input  wire        m_sdram_rvalid,
    input  wire [31:0] m_sdram_rdata,
    input  wire [1:0]  m_sdram_rresp,
    input  wire        m_sdram_rlast,

    output reg         m_sdram_awvalid,
    input  wire        m_sdram_awready,
    output reg  [31:0] m_sdram_awaddr,
    output reg  [7:0]  m_sdram_awlen,

    output reg         m_sdram_wvalid,
    input  wire        m_sdram_wready,
    output reg  [31:0] m_sdram_wdata,
    output reg  [3:0]  m_sdram_wstrb,
    output reg         m_sdram_wlast,

    input  wire        m_sdram_bvalid,
    input  wire [1:0]  m_sdram_bresp,

    // PSRAM AXI4 master interface (to axi_psram_slave via core_top)
    output reg         m_psram_arvalid,
    input  wire        m_psram_arready,
    output reg  [31:0] m_psram_araddr,
    output reg  [7:0]  m_psram_arlen,

    input  wire        m_psram_rvalid,
    input  wire [31:0] m_psram_rdata,
    input  wire [1:0]  m_psram_rresp,
    input  wire        m_psram_rlast,

    output reg         m_psram_awvalid,
    input  wire        m_psram_awready,
    output reg  [31:0] m_psram_awaddr,
    output reg  [7:0]  m_psram_awlen,

    output reg         m_psram_wvalid,
    input  wire        m_psram_wready,
    output reg  [31:0] m_psram_wdata,
    output reg  [3:0]  m_psram_wstrb,
    output reg         m_psram_wlast,

    input  wire        m_psram_bvalid,
    input  wire [1:0]  m_psram_bresp,

    // Local peripheral AXI4 master interface (to axi_periph_slave via core_top)
    output reg         m_local_arvalid,
    input  wire        m_local_arready,
    output reg  [31:0] m_local_araddr,
    output reg  [7:0]  m_local_arlen,

    input  wire        m_local_rvalid,
    input  wire [31:0] m_local_rdata,
    input  wire [1:0]  m_local_rresp,
    input  wire        m_local_rlast,

    output reg         m_local_awvalid,
    input  wire        m_local_awready,
    output reg  [31:0] m_local_awaddr,
    output reg  [7:0]  m_local_awlen,

    output reg         m_local_wvalid,
    input  wire        m_local_wready,
    output reg  [31:0] m_local_wdata,
    output reg  [3:0]  m_local_wstrb,
    output reg         m_local_wlast,

    input  wire        m_local_bvalid,
    input  wire [1:0]  m_local_bresp,

    // External interrupt (active high)
    input  wire        int_m_external
);

// ============================================
// VexiiRiscv AXI4 signals
// ============================================

// Active-high reset for VexiiRiscv
wire reset = ~reset_n;

// FetchL1Axi4 (I-cache, read-only): AR + R channels
wire        fetch_ar_valid;
reg         fetch_ar_ready;
wire [31:0] fetch_ar_addr;
wire [0:0]  fetch_ar_id;
wire [7:0]  fetch_ar_len;

reg         fetch_r_valid;
wire        fetch_r_ready;
reg  [31:0] fetch_r_data;
reg  [0:0]  fetch_r_id;
wire [1:0]  fetch_r_resp = 2'b00;
reg         fetch_r_last;

// LsuL1Axi4 (D-cache, full AXI4): AW + W + B + AR + R channels
wire        lsu_aw_valid;
reg         lsu_aw_ready;
wire [31:0] lsu_aw_addr;
wire [0:0]  lsu_aw_id;
wire [7:0]  lsu_aw_len;

wire        lsu_w_valid;
reg         lsu_w_ready;
wire [31:0] lsu_w_data;
wire [3:0]  lsu_w_strb;
wire        lsu_w_last;

reg         lsu_b_valid;
wire        lsu_b_ready;
reg  [0:0]  lsu_b_id;
wire [1:0]  lsu_b_resp = 2'b00;

wire        lsu_ar_valid;
reg         lsu_ar_ready;
wire [31:0] lsu_ar_addr;
wire [0:0]  lsu_ar_id;
wire [7:0]  lsu_ar_len;

reg         lsu_r_valid;
wire        lsu_r_ready;
reg  [31:0] lsu_r_data;
reg  [0:0]  lsu_r_id;
wire [1:0]  lsu_r_resp = 2'b00;
reg         lsu_r_last;

// LsuPlugin IO bus (simple cmd/rsp, uncached data)
wire        io_cmd_valid;
reg         io_cmd_ready;
wire        io_cmd_write;
wire [31:0] io_cmd_addr;
wire [31:0] io_cmd_data;
wire [3:0]  io_cmd_mask;

reg         io_rsp_valid;
reg         io_rsp_error;
reg  [31:0] io_rsp_data;

// 64-bit rdtime counter for PrivilegedPlugin
reg [63:0] rdtime_counter;
always @(posedge clk or posedge reset) begin
    if (reset)
        rdtime_counter <= 64'd0;
    else
        rdtime_counter <= rdtime_counter + 64'd1;
end

// ============================================
// VexiiRiscv CPU instantiation
// ============================================
VexiiRiscv cpu (
    .clk(clk),
    .reset(reset),

    .PrivilegedPlugin_logic_rdtime(rdtime_counter),
    .PrivilegedPlugin_logic_harts_0_int_m_timer(1'b0),
    .PrivilegedPlugin_logic_harts_0_int_m_software(1'b0),
    .PrivilegedPlugin_logic_harts_0_int_m_external(int_m_external),

    // LsuL1Axi4 (D-cache)
    .LsuL1Axi4Plugin_logic_axi_aw_valid(lsu_aw_valid),
    .LsuL1Axi4Plugin_logic_axi_aw_ready(lsu_aw_ready),
    .LsuL1Axi4Plugin_logic_axi_aw_payload_addr(lsu_aw_addr),
    .LsuL1Axi4Plugin_logic_axi_aw_payload_id(lsu_aw_id),
    .LsuL1Axi4Plugin_logic_axi_aw_payload_len(lsu_aw_len),
    .LsuL1Axi4Plugin_logic_axi_aw_payload_size(),
    .LsuL1Axi4Plugin_logic_axi_aw_payload_burst(),
    .LsuL1Axi4Plugin_logic_axi_aw_payload_cache(),
    .LsuL1Axi4Plugin_logic_axi_aw_payload_prot(),

    .LsuL1Axi4Plugin_logic_axi_w_valid(lsu_w_valid),
    .LsuL1Axi4Plugin_logic_axi_w_ready(lsu_w_ready),
    .LsuL1Axi4Plugin_logic_axi_w_payload_data(lsu_w_data),
    .LsuL1Axi4Plugin_logic_axi_w_payload_strb(lsu_w_strb),
    .LsuL1Axi4Plugin_logic_axi_w_payload_last(lsu_w_last),

    .LsuL1Axi4Plugin_logic_axi_b_valid(lsu_b_valid),
    .LsuL1Axi4Plugin_logic_axi_b_ready(lsu_b_ready),
    .LsuL1Axi4Plugin_logic_axi_b_payload_id(lsu_b_id),
    .LsuL1Axi4Plugin_logic_axi_b_payload_resp(lsu_b_resp),

    .LsuL1Axi4Plugin_logic_axi_ar_valid(lsu_ar_valid),
    .LsuL1Axi4Plugin_logic_axi_ar_ready(lsu_ar_ready),
    .LsuL1Axi4Plugin_logic_axi_ar_payload_addr(lsu_ar_addr),
    .LsuL1Axi4Plugin_logic_axi_ar_payload_id(lsu_ar_id),
    .LsuL1Axi4Plugin_logic_axi_ar_payload_len(lsu_ar_len),
    .LsuL1Axi4Plugin_logic_axi_ar_payload_size(),
    .LsuL1Axi4Plugin_logic_axi_ar_payload_burst(),
    .LsuL1Axi4Plugin_logic_axi_ar_payload_cache(),
    .LsuL1Axi4Plugin_logic_axi_ar_payload_prot(),

    .LsuL1Axi4Plugin_logic_axi_r_valid(lsu_r_valid),
    .LsuL1Axi4Plugin_logic_axi_r_ready(lsu_r_ready),
    .LsuL1Axi4Plugin_logic_axi_r_payload_data(lsu_r_data),
    .LsuL1Axi4Plugin_logic_axi_r_payload_id(lsu_r_id),
    .LsuL1Axi4Plugin_logic_axi_r_payload_resp(lsu_r_resp),
    .LsuL1Axi4Plugin_logic_axi_r_payload_last(lsu_r_last),

    // FetchL1Axi4 (I-cache, read-only)
    .FetchL1Axi4Plugin_logic_axi_ar_valid(fetch_ar_valid),
    .FetchL1Axi4Plugin_logic_axi_ar_ready(fetch_ar_ready),
    .FetchL1Axi4Plugin_logic_axi_ar_payload_addr(fetch_ar_addr),
    .FetchL1Axi4Plugin_logic_axi_ar_payload_id(fetch_ar_id),
    .FetchL1Axi4Plugin_logic_axi_ar_payload_len(fetch_ar_len),
    .FetchL1Axi4Plugin_logic_axi_ar_payload_size(),
    .FetchL1Axi4Plugin_logic_axi_ar_payload_burst(),
    .FetchL1Axi4Plugin_logic_axi_ar_payload_cache(),
    .FetchL1Axi4Plugin_logic_axi_ar_payload_prot(),

    .FetchL1Axi4Plugin_logic_axi_r_valid(fetch_r_valid),
    .FetchL1Axi4Plugin_logic_axi_r_ready(fetch_r_ready),
    .FetchL1Axi4Plugin_logic_axi_r_payload_data(fetch_r_data),
    .FetchL1Axi4Plugin_logic_axi_r_payload_id(fetch_r_id),
    .FetchL1Axi4Plugin_logic_axi_r_payload_resp(fetch_r_resp),
    .FetchL1Axi4Plugin_logic_axi_r_payload_last(fetch_r_last),

    // LsuPlugin IO bus (uncached data)
    .LsuPlugin_logic_bus_cmd_valid(io_cmd_valid),
    .LsuPlugin_logic_bus_cmd_ready(io_cmd_ready),
    .LsuPlugin_logic_bus_cmd_payload_write(io_cmd_write),
    .LsuPlugin_logic_bus_cmd_payload_address(io_cmd_addr),
    .LsuPlugin_logic_bus_cmd_payload_data(io_cmd_data),
    .LsuPlugin_logic_bus_cmd_payload_size(),
    .LsuPlugin_logic_bus_cmd_payload_mask(io_cmd_mask),
    .LsuPlugin_logic_bus_cmd_payload_io(),
    .LsuPlugin_logic_bus_cmd_payload_fromHart(),
    .LsuPlugin_logic_bus_cmd_payload_uopId(),
    .LsuPlugin_logic_bus_rsp_valid(io_rsp_valid),
    .LsuPlugin_logic_bus_rsp_payload_error(io_rsp_error),
    .LsuPlugin_logic_bus_rsp_payload_data(io_rsp_data)
);

// ============================================
// Request arbitration
// ============================================
localparam BUS_NONE  = 2'd0;
localparam BUS_FETCH = 2'd1;
localparam BUS_LSU   = 2'd2;
localparam BUS_IO    = 2'd3;

reg last_grant_lsu;

wire fetch_req = fetch_ar_valid;
wire lsu_rd_req = lsu_ar_valid;
wire lsu_wr_req = lsu_aw_valid;
wire lsu_req = lsu_rd_req | lsu_wr_req;

// Priority: LSU > Fetch with round-robin, IO lowest
wire lsu_grant = lsu_req & (~fetch_req | ~last_grant_lsu);
wire fetch_grant = fetch_req & ~lsu_grant;
wire lsu_rd_grant = lsu_grant & lsu_rd_req;
wire lsu_wr_grant = lsu_grant & ~lsu_rd_req;
wire io_grant = io_cmd_valid & ~lsu_grant & ~fetch_grant;

// ============================================
// Memory access FSM
// ============================================
localparam FSM_IDLE       = 3'd0;
localparam FSM_MEM_AR     = 3'd1;
localparam FSM_MEM_R      = 3'd2;
localparam FSM_MEM_AW     = 3'd3;
localparam FSM_MEM_W      = 3'd4;
localparam FSM_MEM_B      = 3'd5;
localparam FSM_WRITE_NEXT = 3'd6;

reg [2:0] fsm_state;

// Latched request fields
reg [31:0] req_addr_r;
reg [31:0] req_wdata_r;
reg [3:0]  req_wstrb_r;
reg [0:0]  req_id_r;       // AXI ID echo-back (refill-count=2)
reg [1:0]  active_bus;
reg        is_write_r;

// Burst tracking
reg [7:0]  burst_len_r;
reg [7:0]  burst_count;

// Memory target for AXI4 forwarding (3-way)
localparam TGT_SDRAM = 2'd0;
localparam TGT_PSRAM = 2'd1;
localparam TGT_LOCAL = 2'd2;
reg [1:0] target_mem;

// Whether this beat is the last of a burst
wire beat_is_last = (burst_count == burst_len_r);

// AXI4 master target mux (3-way)
wire mem_arready = (target_mem == TGT_SDRAM) ? m_sdram_arready :
                   (target_mem == TGT_PSRAM) ? m_psram_arready :
                                               m_local_arready;
wire mem_rvalid  = (target_mem == TGT_SDRAM) ? m_sdram_rvalid :
                   (target_mem == TGT_PSRAM) ? m_psram_rvalid :
                                               m_local_rvalid;
wire [31:0] mem_rdata = (target_mem == TGT_SDRAM) ? m_sdram_rdata :
                        (target_mem == TGT_PSRAM) ? m_psram_rdata :
                                                    m_local_rdata;
wire mem_rlast   = (target_mem == TGT_SDRAM) ? m_sdram_rlast :
                   (target_mem == TGT_PSRAM) ? m_psram_rlast :
                                               m_local_rlast;
wire mem_awready = (target_mem == TGT_SDRAM) ? m_sdram_awready :
                   (target_mem == TGT_PSRAM) ? m_psram_awready :
                                               m_local_awready;
wire mem_wready  = (target_mem == TGT_SDRAM) ? m_sdram_wready :
                   (target_mem == TGT_PSRAM) ? m_psram_wready :
                                               m_local_wready;
wire mem_bvalid  = (target_mem == TGT_SDRAM) ? m_sdram_bvalid :
                   (target_mem == TGT_PSRAM) ? m_psram_bvalid :
                                               m_local_bvalid;

// ============================================
// Main FSM
// ============================================
always @(posedge clk or posedge reset) begin
    if (reset) begin
        fsm_state <= FSM_IDLE;
        active_bus <= BUS_NONE;
        is_write_r <= 0;
        req_addr_r <= 0;
        req_wdata_r <= 0;
        req_wstrb_r <= 0;
        req_id_r <= 0;
        burst_len_r <= 0;
        burst_count <= 0;
        last_grant_lsu <= 0;

        target_mem <= TGT_SDRAM;

        fetch_ar_ready <= 0;
        fetch_r_valid <= 0;
        fetch_r_data <= 0;
        fetch_r_id <= 0;
        fetch_r_last <= 0;

        lsu_aw_ready <= 0;
        lsu_w_ready <= 0;
        lsu_ar_ready <= 0;
        lsu_r_valid <= 0;
        lsu_r_data <= 0;
        lsu_r_id <= 0;
        lsu_r_last <= 0;
        lsu_b_valid <= 0;
        lsu_b_id <= 0;

        io_cmd_ready <= 0;
        io_rsp_valid <= 0;
        io_rsp_error <= 0;
        io_rsp_data <= 0;

        m_sdram_arvalid <= 0;
        m_sdram_araddr <= 0;
        m_sdram_arlen <= 0;
        m_sdram_awvalid <= 0;
        m_sdram_awaddr <= 0;
        m_sdram_awlen <= 0;
        m_sdram_wvalid <= 0;
        m_sdram_wdata <= 0;
        m_sdram_wstrb <= 0;
        m_sdram_wlast <= 0;

        m_psram_arvalid <= 0;
        m_psram_araddr <= 0;
        m_psram_arlen <= 0;
        m_psram_awvalid <= 0;
        m_psram_awaddr <= 0;
        m_psram_awlen <= 0;
        m_psram_wvalid <= 0;
        m_psram_wdata <= 0;
        m_psram_wstrb <= 0;
        m_psram_wlast <= 0;

        m_local_arvalid <= 0;
        m_local_araddr <= 0;
        m_local_arlen <= 0;
        m_local_awvalid <= 0;
        m_local_awaddr <= 0;
        m_local_awlen <= 0;
        m_local_wvalid <= 0;
        m_local_wdata <= 0;
        m_local_wstrb <= 0;
        m_local_wlast <= 0;
    end else begin
        // Defaults: deassert single-cycle pulses
        fetch_ar_ready <= 0;
        fetch_r_valid <= 0;
        lsu_aw_ready <= 0;
        lsu_w_ready <= 0;
        lsu_ar_ready <= 0;
        lsu_r_valid <= 0;
        lsu_b_valid <= 0;
        io_cmd_ready <= 0;
        io_rsp_valid <= 0;

        case (fsm_state)

        // ============================================
        // IDLE: Accept new AXI4 request, decode target
        // ============================================
        FSM_IDLE: begin
            // Deassert AXI4 master valids
            m_sdram_arvalid <= 0;
            m_sdram_awvalid <= 0;
            m_sdram_wvalid <= 0;
            m_psram_arvalid <= 0;
            m_psram_awvalid <= 0;
            m_psram_wvalid <= 0;
            m_local_arvalid <= 0;
            m_local_awvalid <= 0;
            m_local_wvalid <= 0;

            if (lsu_rd_grant) begin
                // Accept LsuL1 read (AR channel)
                lsu_ar_ready <= 1;
                active_bus <= BUS_LSU;
                is_write_r <= 0;
                req_addr_r <= lsu_ar_addr;
                req_id_r <= lsu_ar_id;
                burst_len_r <= lsu_ar_len;
                burst_count <= 0;
                last_grant_lsu <= 1;

                // Issue AR to target slave
                fsm_state <= FSM_MEM_AR;
                if (lsu_ar_addr[31:26] == 6'b000100 || lsu_ar_addr[31:26] == 6'b010100) begin
                    target_mem <= TGT_SDRAM;
                    m_sdram_arvalid <= 1;
                    m_sdram_araddr <= lsu_ar_addr;
                    m_sdram_arlen <= lsu_ar_len;
                end else if (lsu_ar_addr[31:27] == 5'b00110) begin
                    target_mem <= TGT_PSRAM;
                    m_psram_arvalid <= 1;
                    m_psram_araddr <= lsu_ar_addr;
                    m_psram_arlen <= lsu_ar_len;
                end else begin
                    target_mem <= TGT_LOCAL;
                    m_local_arvalid <= 1;
                    m_local_araddr <= lsu_ar_addr;
                    m_local_arlen <= lsu_ar_len;
                end

            end else if (lsu_wr_grant) begin
                // Accept LsuL1 write address (AW channel)
                lsu_aw_ready <= 1;
                active_bus <= BUS_LSU;
                is_write_r <= 1;
                req_addr_r <= lsu_aw_addr;
                req_id_r <= lsu_aw_id;
                burst_len_r <= lsu_aw_len;
                burst_count <= 0;
                last_grant_lsu <= 1;

                // Determine target
                if (lsu_aw_addr[31:26] == 6'b000100 || lsu_aw_addr[31:26] == 6'b010100)
                    target_mem <= TGT_SDRAM;
                else if (lsu_aw_addr[31:27] == 5'b00110)
                    target_mem <= TGT_PSRAM;
                else
                    target_mem <= TGT_LOCAL;

                // Also accept W if valid on same cycle
                if (lsu_w_valid) begin
                    lsu_w_ready <= 1;
                    req_wdata_r <= lsu_w_data;
                    req_wstrb_r <= lsu_w_strb;

                    // Issue AW to target slave
                    fsm_state <= FSM_MEM_AW;
                    if (lsu_aw_addr[31:26] == 6'b000100 || lsu_aw_addr[31:26] == 6'b010100) begin
                        m_sdram_awvalid <= 1;
                        m_sdram_awaddr <= lsu_aw_addr;
                        m_sdram_awlen <= lsu_aw_len;
                    end else if (lsu_aw_addr[31:27] == 5'b00110) begin
                        m_psram_awvalid <= 1;
                        m_psram_awaddr <= lsu_aw_addr;
                        m_psram_awlen <= lsu_aw_len;
                    end else begin
                        m_local_awvalid <= 1;
                        m_local_awaddr <= lsu_aw_addr;
                        m_local_awlen <= lsu_aw_len;
                    end
                end else begin
                    // W not ready yet - wait for it
                    fsm_state <= FSM_WRITE_NEXT;
                end

            end else if (fetch_grant) begin
                // Accept FetchL1 read (AR channel)
                fetch_ar_ready <= 1;
                active_bus <= BUS_FETCH;
                is_write_r <= 0;
                req_addr_r <= fetch_ar_addr;
                req_id_r <= fetch_ar_id;
                burst_len_r <= fetch_ar_len;
                burst_count <= 0;
                last_grant_lsu <= 0;

                // Issue AR to target slave
                fsm_state <= FSM_MEM_AR;
                if (fetch_ar_addr[31:26] == 6'b000100 || fetch_ar_addr[31:26] == 6'b010100) begin
                    target_mem <= TGT_SDRAM;
                    m_sdram_arvalid <= 1;
                    m_sdram_araddr <= fetch_ar_addr;
                    m_sdram_arlen <= fetch_ar_len;
                end else if (fetch_ar_addr[31:27] == 5'b00110) begin
                    target_mem <= TGT_PSRAM;
                    m_psram_arvalid <= 1;
                    m_psram_araddr <= fetch_ar_addr;
                    m_psram_arlen <= fetch_ar_len;
                end else begin
                    target_mem <= TGT_LOCAL;
                    m_local_arvalid <= 1;
                    m_local_araddr <= fetch_ar_addr;
                    m_local_arlen <= fetch_ar_len;
                end

            end else if (io_grant) begin
                // Accept IO bus command (single-beat)
                io_cmd_ready <= 1;
                active_bus <= BUS_IO;
                burst_len_r <= 0;  // IO is always single-beat
                burst_count <= 0;

                // Address decode for IO bus:
                // 0x50-0x53 (uncached SDRAM alias) → SDRAM target
                // 0x30-0x37 (uncached PSRAM/CRAM1) → PSRAM target
                // Everything else → Local target
                if (io_cmd_addr[31:26] == 6'b010100) begin
                    target_mem <= TGT_SDRAM;
                end else if (io_cmd_addr[31:27] == 5'b00110) begin
                    target_mem <= TGT_PSRAM;
                end else begin
                    target_mem <= TGT_LOCAL;
                end

                if (io_cmd_write) begin
                    // IO write: issue AW + W simultaneously
                    is_write_r <= 1;
                    req_addr_r <= io_cmd_addr;
                    req_wdata_r <= io_cmd_data;
                    req_wstrb_r <= io_cmd_mask;

                    fsm_state <= FSM_MEM_AW;
                    if (io_cmd_addr[31:26] == 6'b010100) begin
                        m_sdram_awvalid <= 1;
                        m_sdram_awaddr <= io_cmd_addr;
                        m_sdram_awlen <= 0;
                    end else if (io_cmd_addr[31:27] == 5'b00110) begin
                        m_psram_awvalid <= 1;
                        m_psram_awaddr <= io_cmd_addr;
                        m_psram_awlen <= 0;
                    end else begin
                        m_local_awvalid <= 1;
                        m_local_awaddr <= io_cmd_addr;
                        m_local_awlen <= 0;
                    end
                end else begin
                    // IO read: issue AR
                    is_write_r <= 0;
                    req_addr_r <= io_cmd_addr;

                    fsm_state <= FSM_MEM_AR;
                    if (io_cmd_addr[31:26] == 6'b010100) begin
                        m_sdram_arvalid <= 1;
                        m_sdram_araddr <= io_cmd_addr;
                        m_sdram_arlen <= 0;
                    end else if (io_cmd_addr[31:27] == 5'b00110) begin
                        m_psram_arvalid <= 1;
                        m_psram_araddr <= io_cmd_addr;
                        m_psram_arlen <= 0;
                    end else begin
                        m_local_arvalid <= 1;
                        m_local_araddr <= io_cmd_addr;
                        m_local_arlen <= 0;
                    end
                end
            end
        end

        // ============================================
        // MEM_AR: Wait for target arready
        // ============================================
        FSM_MEM_AR: begin
            if (mem_arready) begin
                m_sdram_arvalid <= 0;
                m_psram_arvalid <= 0;
                m_local_arvalid <= 0;
                fsm_state <= FSM_MEM_R;
            end
        end

        // ============================================
        // MEM_R: Forward R beats to requester
        // ============================================
        FSM_MEM_R: begin
            if (mem_rvalid) begin
                if (active_bus == BUS_FETCH) begin
                    fetch_r_valid <= 1;
                    fetch_r_data <= mem_rdata;
                    fetch_r_id <= req_id_r;
                    fetch_r_last <= beat_is_last;
                end else if (active_bus == BUS_LSU) begin
                    lsu_r_valid <= 1;
                    lsu_r_data <= mem_rdata;
                    lsu_r_id <= req_id_r;
                    lsu_r_last <= beat_is_last;
                end else begin
                    // BUS_IO: single-beat read response
                    io_rsp_valid <= 1;
                    io_rsp_data <= mem_rdata;
                    io_rsp_error <= 0;
                end
                burst_count <= burst_count + 1;
                if (beat_is_last)
                    fsm_state <= FSM_IDLE;
            end
        end

        // ============================================
        // MEM_AW: Wait for target awready, then send W
        // ============================================
        FSM_MEM_AW: begin
            if (mem_awready) begin
                m_sdram_awvalid <= 0;
                m_psram_awvalid <= 0;
                m_local_awvalid <= 0;
                // Send W beat
                fsm_state <= FSM_MEM_W;
                if (target_mem == TGT_SDRAM) begin
                    m_sdram_wvalid <= 1;
                    m_sdram_wdata <= req_wdata_r;
                    m_sdram_wstrb <= req_wstrb_r;
                    m_sdram_wlast <= beat_is_last;
                end else if (target_mem == TGT_PSRAM) begin
                    m_psram_wvalid <= 1;
                    m_psram_wdata <= req_wdata_r;
                    m_psram_wstrb <= req_wstrb_r;
                    m_psram_wlast <= beat_is_last;
                end else begin
                    m_local_wvalid <= 1;
                    m_local_wdata <= req_wdata_r;
                    m_local_wstrb <= req_wstrb_r;
                    m_local_wlast <= beat_is_last;
                end
            end
        end

        // ============================================
        // MEM_W: Wait for target wready
        // ============================================
        FSM_MEM_W: begin
            if (mem_wready) begin
                m_sdram_wvalid <= 0;
                m_psram_wvalid <= 0;
                m_local_wvalid <= 0;
                burst_count <= burst_count + 1;
                if (beat_is_last) begin
                    fsm_state <= FSM_MEM_B;
                end else begin
                    req_addr_r <= req_addr_r + 32'd4;
                    fsm_state <= FSM_WRITE_NEXT;
                end
            end
        end

        // ============================================
        // MEM_B: Wait for target bvalid, forward response
        // ============================================
        FSM_MEM_B: begin
            if (mem_bvalid) begin
                if (active_bus == BUS_IO) begin
                    // IO write complete
                    io_rsp_valid <= 1;
                    io_rsp_data <= 0;
                    io_rsp_error <= 0;
                end else begin
                    // LSU write complete
                    lsu_b_valid <= 1;
                    lsu_b_id <= req_id_r;
                end
                fsm_state <= FSM_IDLE;
            end
        end

        // ============================================
        // WRITE_NEXT: Accept next W beat from VexiiRiscv, forward to target
        // ============================================
        FSM_WRITE_NEXT: begin
            if (lsu_w_valid) begin
                lsu_w_ready <= 1;
                req_wdata_r <= lsu_w_data;
                req_wstrb_r <= lsu_w_strb;

                // Issue AW if we haven't yet (first beat case where W wasn't ready)
                // or send W beat for subsequent beats
                if (burst_count == 0 && !m_sdram_awvalid && !m_psram_awvalid && !m_local_awvalid) begin
                    // First beat: we accepted AW from VexiiRiscv but haven't issued AW to target yet
                    fsm_state <= FSM_MEM_AW;
                    if (target_mem == TGT_SDRAM) begin
                        m_sdram_awvalid <= 1;
                        m_sdram_awaddr <= req_addr_r;
                        m_sdram_awlen <= burst_len_r;
                    end else if (target_mem == TGT_PSRAM) begin
                        m_psram_awvalid <= 1;
                        m_psram_awaddr <= req_addr_r;
                        m_psram_awlen <= burst_len_r;
                    end else begin
                        m_local_awvalid <= 1;
                        m_local_awaddr <= req_addr_r;
                        m_local_awlen <= burst_len_r;
                    end
                end else begin
                    // Subsequent beat: send W to target
                    fsm_state <= FSM_MEM_W;
                    if (target_mem == TGT_SDRAM) begin
                        m_sdram_wvalid <= 1;
                        m_sdram_wdata <= lsu_w_data;
                        m_sdram_wstrb <= lsu_w_strb;
                        m_sdram_wlast <= (burst_count == burst_len_r);
                    end else if (target_mem == TGT_PSRAM) begin
                        m_psram_wvalid <= 1;
                        m_psram_wdata <= lsu_w_data;
                        m_psram_wstrb <= lsu_w_strb;
                        m_psram_wlast <= (burst_count == burst_len_r);
                    end else begin
                        m_local_wvalid <= 1;
                        m_local_wdata <= lsu_w_data;
                        m_local_wstrb <= lsu_w_strb;
                        m_local_wlast <= (burst_count == burst_len_r);
                    end
                end
            end
        end

        default: fsm_state <= FSM_IDLE;

        endcase
    end
end

endmodule
