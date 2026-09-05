/************************************************************************

    wordPacker.v

    Two 16-bit sample words into one 32-bit transfer word
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

    Halves the write rate into the capture buffer for the same byte
    throughput as 16 bits at twice the word rate - see buffer.v's
    PacketWords for the other half of this arrangement, and the top level's
    Fx3DataWidth for why this exists at all: it is what the wide FX3 bus
    needs on the write side, and DomesdayDuplicator.v is the one place that
    can wire it in only when that bus is actually 32 bits wide.

    A module of its own rather than a generate block inline in the top
    level, specifically so it can be simulated: the top level instantiates
    IPpllGenerator, which needs Altera's altera_mf simulation library and so
    cannot be run under the free tools this project's test suite otherwise
    uses (see fpga/tests/run-sim.sh). Pulling the one piece of genuinely new
    logic this session added out from under that PLL is what lets it have a
    testbench at all - the same reasoning that put bufferMonitor.v in its
    own file.

    The first sample of a pair lands in the low half of the output word and
    the second in the high half. A host unpacking the wide bus has to agree
    with that order.

************************************************************************/

module wordPacker (
    input reset_n,
    input clock,

    // One assertion per 16-bit sample, at whatever rate the capture path
    // produces them.
    input        sample_enable,
    input [15:0] data_in,

    // One assertion per completed pair - once every second sample_enable -
    // with the packed word already valid on data_out that same edge.
    output        write_enable,
    output [31:0] data_out
);

    reg        pending_valid;
    reg [15:0] pending_word;
    reg        packed_write_enable;
    reg [31:0] packed_data;

    always @(posedge clock, negedge reset_n) begin
        if (!reset_n) begin
            pending_valid       <= 1'b0;
            pending_word        <= 16'd0;
            packed_write_enable <= 1'b0;
            packed_data         <= 32'd0;
        end else begin
            // One clock wide, like every other pulse in this design -
            // cleared every cycle and raised only by the sample that
            // completes a pair.
            packed_write_enable <= 1'b0;

            if (sample_enable) begin
                if (pending_valid) begin
                    packed_data         <= {data_in, pending_word};
                    packed_write_enable <= 1'b1;
                    pending_valid       <= 1'b0;
                end else begin
                    pending_word  <= data_in;
                    pending_valid <= 1'b1;
                end
            end
        end
    end

    assign write_enable = packed_write_enable;
    assign data_out     = packed_data;

endmodule
