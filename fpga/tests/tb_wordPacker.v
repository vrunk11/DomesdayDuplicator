/************************************************************************

    tb_wordPacker.v

    Testbench for the 16-to-32-bit sample packer (T3)
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

    This is the one piece of this session's clock-architecture work that can
    be simulated at all: everything else new sits in DomesdayDuplicator.v,
    which instantiates IPpllGenerator and so cannot be (see the note in
    fpga/tests/run-sim.sh). Pulling the packer out into its own module was
    specifically so this file could exist.

    What matters here is pairing and ordering, not timing - Quartus and
    TimeQuest already speak to whether the signals are electrically sound.
    A wrong pairing would not show up as a timing violation at all; it would
    show up as a capture with every sample interleaved with its neighbour,
    which is exactly the kind of silent corruption this project's test
    culture exists to catch before hardware does.

************************************************************************/

`timescale 1ns / 1ps

module tb_wordPacker;

    reg         reset_n;
    reg         clock;
    reg         sample_enable;
    reg  [15:0] data_in;

    wire        write_enable;
    wire [31:0] data_out;

    integer     errors;
    integer     i;

    wordPacker dut (
        .reset_n      (reset_n),
        .clock        (clock),
        .sample_enable(sample_enable),
        .data_in      (data_in),
        .write_enable (write_enable),
        .data_out     (data_out)
    );

    // 150 MHz system clock — 6.667 ns period
    initial begin
        clock = 1'b0;
    end
    always begin
        #3.333 clock = ~clock;
    end

    task check;
        input [31:0] got;
        input [31:0] want;
        input [255:0] what;
        begin
            if (got !== want) begin
                $display("FAIL: %0s: got %h, expected %h (t=%0t)", what, got, want, $time);
                errors = errors + 1;
            end
        end
    endtask

    // Presents one sample for one clock, on sample_enable, the way
    // capture_enable actually pulses in the real design - a single-cycle
    // assertion, not held.
    task present_sample;
        input [15:0] value;
        begin
            data_in       = value;
            sample_enable = 1'b1;
            @(posedge clock);
            #1;
            sample_enable = 1'b0;
        end
    endtask

    initial begin
        errors        = 0;
        reset_n       = 1'b0;
        sample_enable = 1'b0;
        data_in       = 16'd0;

        @(posedge clock);
        #1 reset_n = 1'b1;

        // --- Reset values ---
        check(write_enable, 1'b0, "write_enable is low after reset");
        check(data_out, 32'd0, "data_out is zero after reset");

        // --- First sample of a pair produces no write ---
        //
        // The whole point of this module is to wait for a second sample
        // before it has anything to hand the buffer; a write here would be
        // half a pair reaching the FIFO as a whole word.
        present_sample(16'hAAAA);
        check(write_enable, 1'b0, "no write after only the first sample of a pair");

        // --- The second sample completes the pair ---
        //
        // First sample in the low half, second in the high half - this
        // order is the wire contract with whatever unpacks the wide bus on
        // the other end, and getting it backwards here would still compile,
        // still close timing, and still be wrong.
        present_sample(16'h1234);
        check(write_enable, 1'b1, "write_enable pulses on the second sample of a pair");
        check(data_out, 32'h1234AAAA, "first sample low, second sample high");

        // --- The pulse is exactly one clock wide ---
        @(posedge clock);
        #1;
        check(write_enable, 1'b0, "write_enable clears the cycle after the pulse");

        // --- Several pairs in a row, each with the same spacing the real
        // decimator gives sample_enable (one sample every second
        // system_clock cycle) - checks the pending/complete alternation
        // does not drift over many pairs, not just the first one. ---
        for (i = 0; i < 8; i = i + 1) begin
            present_sample(16'h0100 + i);
            check(write_enable, 1'b0, "no write after an odd-numbered sample");
            @(posedge clock);
            #1;

            present_sample(16'h0200 + i);
            check(write_enable, 1'b1, "write pulses after an even-numbered sample");
            check(data_out, {16'h0200 + i[15:0], 16'h0100 + i[15:0]},
                  "pair contents in order, repeated pairs");
            @(posedge clock);
            #1;
            check(write_enable, 1'b0, "write_enable clears again");
        end

        // --- Reset mid-pair discards the half received so far ---
        //
        // A pending first sample must not survive into a sample taken after
        // reset and be paired with it - that would silently join two
        // samples that were never adjacent.
        present_sample(16'hFFFF);
        check(write_enable, 1'b0, "first sample of a pair pending before reset");

        reset_n = 1'b0;
        @(posedge clock);
        #1 reset_n = 1'b1;

        present_sample(16'h5678);
        check(write_enable, 1'b0,
              "the post-reset sample is a fresh first sample, not a completed pair");

        present_sample(16'h9ABC);
        check(write_enable, 1'b1, "the next sample completes a genuine post-reset pair");
        check(data_out, 32'h9ABC5678,
              "post-reset pairing does not include the pre-reset half sample");

        if (errors == 0) begin
            $display("tb_wordPacker: PASS");
        end else begin
            $display("tb_wordPacker: FAIL (%0d errors)", errors);
        end

        if (errors != 0) begin
            $fatal(1, "tb_wordPacker failed");
        end
        $finish;
    end

endmodule
