/************************************************************************

    tb_dataGenerator.v

    Testbench for the data generation module (T3)
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2018-2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

    This is the simulation counterpart of step 4 of the capture-integrity
    procedure in TESTING.md. That procedure asserts, on silicon and through a
    real USB capture, that the test data is an unbroken ramp with a monotonic
    sequence number. Everything it checks originates here, so a defect in this
    module is the one class of capture fault that can be caught before the
    bitstream is built.

************************************************************************/

`timescale 1ns / 1ps

module tb_dataGenerator;

    reg            reset_n;
    reg            clock;
    reg     [ 9:0] adc_databus;
    reg            test_mode_flag;
    wire    [15:0] data_out;

    integer        errors;
    integer        i;
    integer        expected;

    // The ramp is 0..1020 inclusive, so it repeats every 1021 samples. This is
    // the number the GUI's analyser and dddutil both assume; if it changes here
    // it must change there too.
    localparam integer RAMP_LENGTH = 1021;

    // The sequence number advances once every 65535 samples and wraps after 63
    // of them. The block length is odd on purpose - see dataGenerator.v - and
    // that is exactly why it is worth simulating: a length that is not a power
    // of two cannot be the top bits of a wider counter, so the wrap is real
    // arithmetic rather than a bit slice that cannot get it wrong.
    localparam integer SAMPLES_PER_SEQUENCE = 65535;
    localparam integer SEQUENCE_COUNT = 63;

    // 80 MHz system clock — 12.5 ns period
    initial begin
        clock = 1'b0;
    end
    always begin
        #6.25 clock = ~clock;
    end

    // The sampling clock and its enable, built exactly as the top level builds
    // them: a free-running divide-by-two whose output is the ADC's 40 MHz
    // clock, and an enable that is high on the cycle which takes that clock
    // high. Reproducing the real relationship rather than simply toggling an
    // enable is the point — it is what makes this testbench cover the phasing
    // as well as the counting.
    reg adc_clock_divider;
    initial begin
        adc_clock_divider = 1'b0;
    end
    always @(posedge clock) begin
        adc_clock_divider <= ~adc_clock_divider;
    end

    wire sample_enable = ~adc_clock_divider;

    dataGenerator dut (
        .reset_n       (reset_n),
        .clock         (clock),
        .sample_enable (sample_enable),
        .adc_databus   (adc_databus),
        .test_mode_flag(test_mode_flag),
        .data_out      (data_out)
    );

    // Advance to, and through, the next edge at which the DUT takes a sample.
    // Every loop below counts samples, not clock cycles, so this is what a
    // "clock edge" in the original testbench meant.
    task sample_tick;
        begin
            while (sample_enable !== 1'b1) begin
                @(posedge clock);
                #1;
            end
            @(posedge clock);
            #1;
        end
    endtask

    task check;
        input [31:0] got;
        input [31:0] want;
        input [255:0] what;
        begin
            if (got !== want) begin
                $display("FAIL: %0s: got %0d, expected %0d (t=%0t)", what, got, want, $time);
                errors = errors + 1;
            end
        end
    endtask

    initial begin
        errors         = 0;
        adc_databus    = 10'd0;
        test_mode_flag = 1'b1;
        reset_n        = 1'b0;

        // Hold reset across a clock edge, then release it away from one
        @(posedge clock);
        #1 reset_n = 1'b1;

        // --- Reset state -------------------------------------------------
        // Every register clears to zero, so the first sample of a capture is
        // sample 0 of the ramp with sequence number 0.
        check(data_out[9:0], 0, "test data after reset");
        check(data_out[15:10], 0, "sequence number after reset");

        // --- The ramp ----------------------------------------------------
        // Walk three full periods. Two would prove it wraps; three proves the
        // wrap leaves the counter in a state that wraps again the same way.
        for (i = 1; i <= RAMP_LENGTH * 3; i = i + 1) begin
            sample_tick;
            expected = i % RAMP_LENGTH;
            check(data_out[9:0], expected, "test data ramp");
        end

        // The ramp must reach 1020 and must never reach 1021. The loop above
        // covers both, but assert the endpoint explicitly so a failure names it.
        check(RAMP_LENGTH - 1, 1020, "ramp endpoint");

        // --- ADC passthrough ---------------------------------------------
        // Out of test mode the low ten bits are the ADC bus, registered on the
        // clock edge — so the value appears one cycle after it is presented.
        test_mode_flag = 1'b0;
        adc_databus    = 10'd682;  // 0b1010101010: every bit exercised
        sample_tick;
        check(data_out[9:0], 682, "ADC passthrough (alternating bits)");

        adc_databus = 10'd341;  // 0b0101010101: the complement
        sample_tick;
        check(data_out[9:0], 341, "ADC passthrough (complement)");

        adc_databus = 10'd1023;
        sample_tick;
        check(data_out[9:0], 1023, "ADC passthrough (full scale)");

        // Test mode must ignore the ADC bus entirely: the ramp continues from
        // where it left off rather than restarting or picking up ADC values.
        test_mode_flag = 1'b1;
        sample_tick;
        if (data_out[9:0] === 1023) begin
            $display("FAIL: test mode is passing ADC data through (t=%0t)", $time);
            errors = errors + 1;
        end

        // --- Sequence number ---------------------------------------------
        // This is the field the capture-integrity procedure counts breaks in.
        // Restart from reset so the sample count is known exactly.
        reset_n = 1'b0;
        @(posedge clock);
        #1 reset_n = 1'b1;
        check(data_out[15:10], 0, "sequence number after second reset");

        // One short of the boundary the field is still 0; one sample later it is 1.
        for (i = 1; i < SAMPLES_PER_SEQUENCE; i = i + 1) begin
            sample_tick;
        end
        check(data_out[15:10], 0, "sequence number just before the first boundary");

        sample_tick;
        check(data_out[15:10], 1, "sequence number at the first boundary");

        for (i = 1; i <= SAMPLES_PER_SEQUENCE; i = i + 1) begin
            sample_tick;
        end
        check(data_out[15:10], 2, "sequence number at the second boundary");

        // Run out the remaining sequence numbers and check the wrap. Both
        // counters have to roll together for this to land: the block counter
        // back to zero on its 65535th sample and the sequence number from 62
        // to 0 rather than on to 63. Worth the simulation time, because a
        // wrap that lands on 63 would put a value in the field that the host
        // never expects and only a full period of samples reaches it.
        for (i = 1; i <= SAMPLES_PER_SEQUENCE * (SEQUENCE_COUNT - 2); i = i + 1) begin
            sample_tick;
        end
        check(data_out[15:10], 0, "sequence number wrap after 63 sequences");

        sample_tick;
        check(data_out[9:0], (SAMPLES_PER_SEQUENCE * SEQUENCE_COUNT + 1) % RAMP_LENGTH,
              "ramp is continuous across the sequence wrap");

        if (errors == 0) begin
            $display("tb_dataGenerator: PASS");
        end else begin
            $display("tb_dataGenerator: FAIL (%0d errors)", errors);
        end

        if (errors != 0) begin
            $fatal(1, "tb_dataGenerator failed");
        end
        $finish;
    end

endmodule
