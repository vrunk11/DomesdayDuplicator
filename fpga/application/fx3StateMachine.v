/************************************************************************

    fx3StateMachine.v

    FX3 State-Machine module
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2018-2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

module fx3StateMachine #(
    // Words per packet - has to agree with buffer.v's PacketWords, which is
    // where the reasoning for the number lives. Two places that must agree
    // and cannot share a localparam, because this module has no visibility
    // into buffer.v's DataWidth of its own; the top level ties them together
    // by passing the same derived value to both.
    parameter integer PacketWords = 8192
) (
    input reset_n,
    input fx3_clock,
    input read_data,

    // High on the fx3_clock cycle a word may actually be counted. At
    // Fx3DataWidth == 16 this is tied high - fx3_clock and this module's own
    // clock are the same rate, so every cycle counts, as it always has. At
    // 32 bits the pin fx3_clock's name refers to is a divide-by-two of the
    // clock this module (and the FIFO) actually run on, so counting every
    // cycle here would count words twice as fast as the FX3 can really take
    // them - see the top-level comment where this input is driven.
    input transfer_enable,

    output fx3_is_reading
);

    localparam integer WordCounterBits = $clog2(PacketWords + 1);
    localparam [31:0] PacketWordsValue = PacketWords;
    localparam [WordCounterBits-1:0] LastWordIndex =
        PacketWordsValue[WordCounterBits-1:0] - 1'b1;

    // State machine logic ---------------------------------------------------

    // State machine state definitions (4-bit 0-15)
    reg [3:0] sm_current_state;
    reg [3:0] sm_next_state;

    // localparam rather than parameter: these are state encodings, not knobs an
    // instantiation should be able to override.
    localparam [3:0] StateWaitForRequest = 4'd01;
    localparam [3:0] StateSendPacket = 4'd02;

    // Set state to StateWaitForRequest on reset - or assign the next state
    always @(posedge fx3_clock, negedge reset_n) begin
        if (!reset_n) begin
            sm_current_state <= StateWaitForRequest;
        end else begin
            sm_current_state <= sm_next_state;
        end
    end

    // Ensure that the read_data signal is only read
    // on the FX3 clock edge
    reg read_data_flag;

    always @(posedge fx3_clock, negedge reset_n) begin
        if (!reset_n) begin
            read_data_flag <= 1'b0;
        end else begin
            read_data_flag <= read_data;
        end
    end

    // Counter for the StateSendPacket state
    // Here we should send PacketWords words to the FX3
    reg [WordCounterBits-1:0] word_counter;

    always @(posedge fx3_clock, negedge reset_n) begin
        if (!reset_n) begin
            word_counter = {WordCounterBits{1'b0}};
        end else begin
            if (sm_current_state == StateSendPacket && transfer_enable) begin
                word_counter = word_counter + 1'b1;
            end else if (sm_current_state != StateSendPacket) begin
                word_counter = {WordCounterBits{1'b0}};
            end
        end
    end

    // Generate fx3_is_reading flag
    assign fx3_is_reading = (sm_current_state == StateSendPacket) ? 1'b1 : 1'b0;

    // State machine transition logic
    always @(*) begin
        sm_next_state = sm_current_state;

        case (sm_current_state)

            // StateWaitForRequest (waits for the FX3 to request a packet)
            StateWaitForRequest: begin
                // Is the GPIF reading data?
                if (read_data_flag == 1'b1 && word_counter == {WordCounterBits{1'b0}}) begin
                    sm_next_state = StateSendPacket;
                end else begin
                    // GPIF not ready... wait
                    sm_next_state = StateWaitForRequest;
                end
            end

            // StateSendPacket (sends a packet of PacketWords words to the FX3)
            StateSendPacket: begin
                if (word_counter == LastWordIndex) begin
                    // Packet send, go back to waiting
                    sm_next_state = StateWaitForRequest;
                end else begin
                    // Continue sending packet
                    sm_next_state = StateSendPacket;
                end
            end

        endcase
    end


endmodule
