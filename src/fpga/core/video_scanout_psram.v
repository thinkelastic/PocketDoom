//
// Video Scanout with PSRAM Framebuffer
// Reads RGB565 pixels from PSRAM using sequential 16-bit reads
// Uses a line buffer for clock domain crossing
//

`default_nettype none

module video_scanout_psram (
    // Video clock domain (12.288 MHz)
    input wire clk_video,
    input wire reset_n,

    // Video timing inputs (active high)
    input wire [9:0] x_count,
    input wire [9:0] y_count,
    input wire line_start,          // Pulses at start of each line (x_count == 0)

    // Pixel output (RGB888)
    output reg [23:0] pixel_color,

    // Framebuffer base address (22-bit PSRAM word address)
    // For double buffering, this switches between front/back buffer addresses
    input wire [21:0] fb_base_addr,

    // Enable signal - wait for framebuffer init before starting
    input wire enable,

    // PSRAM clock domain (48 MHz)
    input wire clk_psram,

    // PSRAM 16-bit interface (directly to psram.sv module)
    output reg         psram_read_en,
    output reg  [21:0] psram_addr,
    output reg         psram_bank_sel,
    input wire  [15:0] psram_data_out,
    input wire         psram_read_avail,
    input wire         psram_busy
);

    // Video timing parameters
    localparam VID_V_BPORCH = 16;
    localparam VID_V_ACTIVE = 240;
    localparam VID_H_BPORCH = 40;
    localparam VID_H_ACTIVE = 320;

    // Line buffer: 320 pixels x 16 bits = 640 bytes
    // Dual-port RAM: write from PSRAM clock, read from video clock
    reg [15:0] line_buffer [0:319];
    reg [8:0] write_ptr;    // Write pointer (0-319)

    // =========================================
    // Video clock domain - Line start detection
    // =========================================

    // Detect which line we need to fetch (current visible line)
    wire [9:0] fetch_line = y_count - VID_V_BPORCH;
    wire in_vactive = (y_count >= VID_V_BPORCH) && (y_count < VID_V_BPORCH + VID_V_ACTIVE);

    // Generate fetch request at end of line (before next visible line)
    reg fetch_request;
    reg fetch_request_ack_sync1, fetch_request_ack_sync2;
    reg [8:0] fetch_line_latched;

    // Sync enable signal to video clock domain
    reg enable_sync1, enable_sync2;

    always @(posedge clk_video or negedge reset_n) begin
        if (!reset_n) begin
            fetch_request <= 0;
            fetch_line_latched <= 0;
            fetch_request_ack_sync1 <= 0;
            fetch_request_ack_sync2 <= 0;
            enable_sync1 <= 0;
            enable_sync2 <= 0;
        end else begin
            // Sync enable from PSRAM domain
            enable_sync1 <= enable;
            enable_sync2 <= enable_sync1;

            // Sync ack from PSRAM domain
            fetch_request_ack_sync1 <= fetch_request_ack;
            fetch_request_ack_sync2 <= fetch_request_ack_sync1;

            // Clear request when ack received
            if (fetch_request_ack_sync2)
                fetch_request <= 0;

            // Issue fetch request at line start if in active region AND enabled
            if (line_start && in_vactive && !fetch_request && enable_sync2) begin
                fetch_request <= 1;
                fetch_line_latched <= fetch_line[8:0];
            end
        end
    end

    // =========================================
    // Video clock domain - Pixel output
    // =========================================

    wire [9:0] visible_x = x_count - VID_H_BPORCH;
    wire in_hactive = (x_count >= VID_H_BPORCH) && (x_count < VID_H_BPORCH + VID_H_ACTIVE);
    wire in_vactive_display = (y_count >= VID_V_BPORCH) && (y_count < VID_V_BPORCH + VID_V_ACTIVE);

    // Read from line buffer and convert RGB565 to RGB888
    wire [15:0] pixel_rgb565 = line_buffer[visible_x[8:0]];
    wire [4:0] r5 = pixel_rgb565[15:11];
    wire [5:0] g6 = pixel_rgb565[10:5];
    wire [4:0] b5 = pixel_rgb565[4:0];

    always @(posedge clk_video) begin
        if (in_hactive && in_vactive_display && enable_sync2) begin
            // RGB565 to RGB888: replicate MSBs into LSBs for proper scaling
            pixel_color <= {r5, r5[4:2], g6, g6[5:4], b5, b5[4:2]};
        end else begin
            pixel_color <= 24'h000000;
        end
    end

    // =========================================
    // PSRAM clock domain - Sequential read FSM
    // =========================================

    // Sync fetch request to PSRAM domain
    reg fetch_request_sync1, fetch_request_sync2;
    reg fetch_request_ack;
    reg [8:0] fetch_line_psram;
    reg [8:0] fetch_line_sync1, fetch_line_sync2;

    // FSM states
    localparam ST_IDLE = 3'd0;
    localparam ST_START_READ = 3'd1;
    localparam ST_WAIT_BUSY = 3'd2;
    localparam ST_WAIT_DATA = 3'd3;
    localparam ST_STORE = 3'd4;
    localparam ST_DONE = 3'd5;

    reg [2:0] state;

    always @(posedge clk_psram or negedge reset_n) begin
        if (!reset_n) begin
            state <= ST_IDLE;
            psram_read_en <= 0;
            psram_addr <= 0;
            psram_bank_sel <= 0;
            write_ptr <= 0;
            fetch_request_sync1 <= 0;
            fetch_request_sync2 <= 0;
            fetch_request_ack <= 0;
            fetch_line_psram <= 0;
            fetch_line_sync1 <= 0;
            fetch_line_sync2 <= 0;
        end else begin
            // Sync fetch request
            fetch_request_sync1 <= fetch_request;
            fetch_request_sync2 <= fetch_request_sync1;
            // Sync line index with request handshake to avoid CDC corruption
            fetch_line_sync1 <= fetch_line_latched;
            fetch_line_sync2 <= fetch_line_sync1;

            // Default: deassert read_en
            psram_read_en <= 0;

            case (state)
                ST_IDLE: begin
                    fetch_request_ack <= 0;

                    // Rising edge of fetch request
                    if (fetch_request_sync2 && !fetch_request_ack) begin
                        // Calculate PSRAM address for this line
                        // Each line is 320 pixels * 2 bytes = 640 bytes = 320 16-bit words
                        fetch_line_psram <= fetch_line_sync2;
                        // Base address + line * 320
                        // Using shift: line * 320 = line * 256 + line * 64
                        psram_addr <= fb_base_addr + {fetch_line_sync2, 8'b0} + {1'b0, fetch_line_sync2, 6'b0};
                        psram_bank_sel <= 0;  // Use bank 0
                        write_ptr <= 0;
                        state <= ST_START_READ;
                    end
                end

                ST_START_READ: begin
                    // Issue read request
                    if (!psram_busy) begin
                        psram_read_en <= 1;
                        state <= ST_WAIT_BUSY;
                    end
                end

                ST_WAIT_BUSY: begin
                    // Wait for PSRAM to start (busy goes high)
                    if (psram_busy) begin
                        state <= ST_WAIT_DATA;
                    end
                end

                ST_WAIT_DATA: begin
                    // Wait for read to complete (read_avail pulses)
                    if (psram_read_avail) begin
                        state <= ST_STORE;
                    end
                end

                ST_STORE: begin
                    // Store pixel in line buffer
                    line_buffer[write_ptr] <= psram_data_out;
                    write_ptr <= write_ptr + 1;

                    // Check if line complete
                    if (write_ptr == 319) begin
                        fetch_request_ack <= 1;
                        state <= ST_DONE;
                    end else begin
                        // Next address
                        psram_addr <= psram_addr + 1;
                        state <= ST_START_READ;
                    end
                end

                ST_DONE: begin
                    // Wait for fetch_request to clear before accepting new request
                    if (!fetch_request_sync2) begin
                        fetch_request_ack <= 0;
                        state <= ST_IDLE;
                    end
                end
            endcase
        end
    end

endmodule
