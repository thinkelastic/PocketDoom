//
// Video Scanout with 8-bit Indexed Color and Hardware Palette (PSRAM-backed)
// Reads 8-bit palette indices from PSRAM via 32-bit word interface.
//

`default_nettype none

module video_scanout_indexed_psram (
    // Video clock domain (12.288 MHz)
    input wire clk_video,
    input wire reset_n,
    input wire enable,              // 1 = fetch/display framebuffer, 0 = idle

    // Video timing inputs (active high)
    input wire [9:0] x_count,
    input wire [9:0] y_count,
    input wire line_start,          // Pulses at start of each line (x_count == 0)

    // Pixel output (RGB888)
    output reg [23:0] pixel_color,

    // Framebuffer base address (25-bit byte>>1 addressing used by cpu_system)
    input wire [24:0] fb_base_addr,

    // Memory clock domain (66 MHz)
    input wire clk_mem,

    // PSRAM 32-bit word read interface
    output reg         psram_rd,
    output reg  [21:0] psram_addr,
    input wire  [31:0] psram_q,
    input wire         psram_busy,
    input wire         psram_q_valid,
    output reg         psram_active, // Asserted while a line fetch is in-flight

    // Palette write interface (from CPU)
    input wire        pal_wr,
    input wire [7:0]  pal_addr,
    input wire [23:0] pal_data
);

    // Video timing parameters
    localparam VID_V_BPORCH = 16;
    localparam VID_V_ACTIVE = 240;
    localparam VID_H_BPORCH = 40;
    localparam VID_H_ACTIVE = 320;

    // One line = 320 x 8-bit indices = 80 x 32-bit words
    localparam LINE_WORDS_32 = 7'd80;

    // Line buffer: 320 x 8-bit palette indices stored as 160 x 16-bit words
    reg [15:0] line_buffer [0:159];
    reg [7:0] write_ptr;

    // Palette RAM: 256 entries x 24-bit RGB
    reg [23:0] palette [0:255];

    // Palette writes in memory domain
    always @(posedge clk_mem) begin
        if (pal_wr) begin
            palette[pal_addr] <= pal_data;
        end
    end

    // =========================================
    // Video clock domain - request generation
    // =========================================
    wire [9:0] fetch_line = y_count - VID_V_BPORCH;
    wire in_vactive = (y_count >= VID_V_BPORCH) && (y_count < VID_V_BPORCH + VID_V_ACTIVE);

    reg fetch_request;
    reg fetch_request_ack_sync1, fetch_request_ack_sync2;
    reg [8:0] fetch_line_latched;

    always @(posedge clk_video or negedge reset_n) begin
        if (!reset_n) begin
            fetch_request <= 1'b0;
            fetch_line_latched <= 9'd0;
            fetch_request_ack_sync1 <= 1'b0;
            fetch_request_ack_sync2 <= 1'b0;
        end else begin
            fetch_request_ack_sync1 <= fetch_request_ack;
            fetch_request_ack_sync2 <= fetch_request_ack_sync1;

            if (fetch_request_ack_sync2) begin
                fetch_request <= 1'b0;
            end

            if (line_start && enable && in_vactive && !fetch_request) begin
                fetch_request <= 1'b1;
                fetch_line_latched <= fetch_line[8:0];
            end
        end
    end

    // =========================================
    // Video clock domain - pixel output
    // =========================================
    wire [9:0] visible_x = x_count - VID_H_BPORCH;
    wire in_hactive = (x_count >= VID_H_BPORCH) && (x_count < VID_H_BPORCH + VID_H_ACTIVE);
    wire in_vactive_display = (y_count >= VID_V_BPORCH) && (y_count < VID_V_BPORCH + VID_V_ACTIVE);

    wire [7:0] word_idx = visible_x[8:1];
    wire [15:0] pixel_word = line_buffer[word_idx];
    wire [7:0] palette_index = visible_x[0] ? pixel_word[15:8] : pixel_word[7:0];

    reg [23:0] palette_rgb;
    always @(posedge clk_video) begin
        palette_rgb <= palette[palette_index];
    end

    always @(posedge clk_video) begin
        if (enable && in_hactive && in_vactive_display) begin
            pixel_color <= palette_rgb;
        end else begin
            pixel_color <= 24'h000000;
        end
    end

    // =========================================
    // Memory clock domain - line fetch FSM
    // =========================================
    reg fetch_request_sync1, fetch_request_sync2;
    reg fetch_request_ack;
    reg [8:0] fetch_line_sync1, fetch_line_sync2;

    reg [1:0] state;
    reg [6:0] word_index;
    reg       word_outstanding;
    reg [21:0] line_base_addr32;
    wire [21:0] line_offset32 = {7'd0, fetch_line_sync2, 6'b0} + {9'd0, fetch_line_sync2, 4'b0};

    localparam ST_IDLE = 2'd0;
    localparam ST_FETCH = 2'd1;
    localparam ST_WAIT_CLEAR = 2'd2;

    always @(posedge clk_mem or negedge reset_n) begin
        if (!reset_n) begin
            fetch_request_sync1 <= 1'b0;
            fetch_request_sync2 <= 1'b0;
            fetch_request_ack <= 1'b0;
            fetch_line_sync1 <= 9'd0;
            fetch_line_sync2 <= 9'd0;
            state <= ST_IDLE;
            psram_rd <= 1'b0;
            psram_addr <= 22'd0;
            psram_active <= 1'b0;
            word_index <= 7'd0;
            write_ptr <= 8'd0;
            word_outstanding <= 1'b0;
            line_base_addr32 <= 22'd0;
        end else begin
            fetch_request_sync1 <= fetch_request;
            fetch_request_sync2 <= fetch_request_sync1;
            fetch_line_sync1 <= fetch_line_latched;
            fetch_line_sync2 <= fetch_line_sync1;
            psram_rd <= 1'b0;

            case (state)
                ST_IDLE: begin
                    fetch_request_ack <= 1'b0;
                    psram_active <= 1'b0;
                    word_outstanding <= 1'b0;

                    if (fetch_request_sync2) begin
                        // fb_base_addr is byte>>1 addressing (16-bit words). Convert to 32-bit words:
                        // base32 = (fb_base_addr >> 1) + line * 80
                        line_base_addr32 <= fb_base_addr[22:1] + line_offset32;
                        word_index <= 7'd0;
                        write_ptr <= 8'd0;
                        psram_active <= 1'b1;
                        state <= ST_FETCH;
                    end
                end

                ST_FETCH: begin
                    // Issue next read whenever no command is outstanding.
                    if (!word_outstanding && !psram_busy) begin
                        psram_rd <= 1'b1;
                        psram_addr <= line_base_addr32 + {15'd0, word_index};
                        word_outstanding <= 1'b1;
                    end

                    // Capture read response.
                    if (word_outstanding && psram_q_valid) begin
                        line_buffer[write_ptr] <= psram_q[15:0];
                        line_buffer[write_ptr + 1'b1] <= psram_q[31:16];
                        write_ptr <= write_ptr + 8'd2;
                        word_outstanding <= 1'b0;

                        if (word_index == (LINE_WORDS_32 - 1'b1)) begin
                            fetch_request_ack <= 1'b1;
                            psram_active <= 1'b0;
                            state <= ST_WAIT_CLEAR;
                        end else begin
                            word_index <= word_index + 7'd1;
                        end
                    end
                end

                ST_WAIT_CLEAR: begin
                    if (!fetch_request_sync2) begin
                        fetch_request_ack <= 1'b0;
                        state <= ST_IDLE;
                    end
                end

                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule
