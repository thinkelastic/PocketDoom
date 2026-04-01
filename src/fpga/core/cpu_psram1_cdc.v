//
// CPU ↔ PSRAM1 Clock Domain Crossing Adapter
//
// Bridges the CPU (clk_cpu, 100 MHz) to PSRAM1 (clk_74a, 74.25 MHz)
// using a req/ack toggle handshake.  Data is latched and stable across
// the handshake boundary — no metastability risk beyond the 2-FF syncs.
//
// Latency: ~8 sync cycles + ~14 PSRAM cycles ≈ 22 cycles per access.
// Only used for save data — not performance-critical.
//

`default_nettype none

module cpu_psram1_cdc (
    // CPU side (clk_cpu domain)
    input  wire        clk_cpu,
    input  wire        cpu_rd,
    input  wire        cpu_wr,
    input  wire [21:0] cpu_addr,
    input  wire [31:0] cpu_wdata,
    input  wire [3:0]  cpu_wstrb,
    output reg  [31:0] cpu_rdata,
    output reg         cpu_busy,
    output reg         cpu_rdata_valid,

    // PSRAM1 side (clk_74a domain)
    input  wire        clk_74a,
    output reg         psram_rd,
    output reg         psram_wr,
    output reg  [21:0] psram_addr,
    output reg  [31:0] psram_wdata,
    output reg  [3:0]  psram_wstrb,
    input  wire [31:0] psram_rdata,
    input  wire        psram_busy,
    input  wire        psram_rdata_valid,

    // Bridge activity (clk_74a domain) — hold off CPU when bridge is active
    input  wire        bridge_active
);

// ============================================
// Latched request data (written in clk_cpu, read in clk_74a)
// Safe: stable whenever req_toggle != ack_toggle (handshake invariant)
// ============================================
reg [21:0] lat_addr;
reg [31:0] lat_wdata;
reg [3:0]  lat_wstrb;
reg        lat_is_write;

// ============================================
// Latched response data (written in clk_74a, read in clk_cpu)
// Safe: stable whenever ack_toggle has been toggled
// ============================================
reg [31:0] lat_rdata;

// ============================================
// Toggle handshake signals
// ============================================
reg req_toggle;   // clk_cpu domain: toggled when new request issued
reg ack_toggle;   // clk_74a domain: toggled when request completed

// 2-FF synchronizers
reg [1:0] req_sync;  // req_toggle synced into clk_74a
reg [1:0] ack_sync;  // ack_toggle synced into clk_cpu

always @(posedge clk_74a)
    req_sync <= {req_sync[0], req_toggle};

always @(posedge clk_cpu)
    ack_sync <= {ack_sync[0], ack_toggle};

wire req_seen  = req_sync[1];   // clk_74a: latest synced req_toggle
wire ack_seen  = ack_sync[1];   // clk_cpu: latest synced ack_toggle

// ============================================
// CPU side (clk_cpu): issue requests, wait for ack
// ============================================
reg cpu_waiting;  // request in flight

always @(posedge clk_cpu) begin
    cpu_rdata_valid <= 1'b0;  // default: one-cycle pulse

    if (!cpu_waiting) begin
        cpu_busy <= 1'b0;
        if (cpu_rd || cpu_wr) begin
            // Latch request
            lat_addr     <= cpu_addr;
            lat_wdata    <= cpu_wdata;
            lat_wstrb    <= cpu_wstrb;
            lat_is_write <= cpu_wr;
            // Toggle request
            req_toggle   <= ~req_toggle;
            cpu_busy     <= 1'b1;
            cpu_waiting  <= 1'b1;
        end
    end else begin
        cpu_busy <= 1'b1;
        // Wait for ack_toggle to match req_toggle (synced)
        if (ack_seen == req_toggle) begin
            // Response arrived
            if (!lat_is_write) begin
                cpu_rdata       <= lat_rdata;
                cpu_rdata_valid <= 1'b1;
            end
            cpu_waiting <= 1'b0;
            cpu_busy    <= 1'b0;
        end
    end
end

// ============================================
// PSRAM1 side (clk_74a): execute requests
// ============================================
localparam P_IDLE    = 2'd0;
localparam P_ISSUE   = 2'd1;
localparam P_WAIT    = 2'd2;
localparam P_DONE    = 2'd3;

reg [1:0] p_state;
reg       p_started;

always @(posedge clk_74a) begin
    psram_rd <= 1'b0;  // default: deassert
    psram_wr <= 1'b0;

    case (p_state)

    P_IDLE: begin
        // New request? req_toggle (synced) differs from ack_toggle
        if (req_seen != ack_toggle && !bridge_active && !psram_busy) begin
            psram_addr  <= lat_addr;
            psram_wdata <= lat_wdata;
            psram_wstrb <= lat_wstrb;
            if (lat_is_write)
                psram_wr <= 1'b1;
            else
                psram_rd <= 1'b1;
            p_started <= 1'b0;
            p_state   <= P_WAIT;
        end
    end

    P_WAIT: begin
        // Wait for psram_controller handshake: busy rises then falls (write),
        // or rdata_valid pulses (read)
        if (!p_started && psram_busy)
            p_started <= 1'b1;

        if (lat_is_write) begin
            if (p_started && !psram_busy)
                p_state <= P_DONE;
        end else begin
            if (psram_rdata_valid) begin
                lat_rdata <= psram_rdata;
                p_state   <= P_DONE;
            end
        end
    end

    P_DONE: begin
        // Toggle ack to signal CPU
        ack_toggle <= ~ack_toggle;
        p_state    <= P_IDLE;
    end

    endcase
end

endmodule
