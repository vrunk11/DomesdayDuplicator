/************************************************************************

    dataGenerator.v

    Data generation module
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2018-2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

    Runs on the system clock and takes one sample per assertion of
    sample_enable, which the top level raises on the cycle that also takes the
    ADC clock high. That is the same instant this module used to capture on
    when it was clocked by the ADC clock directly: the sample it reads was
    launched by the previous ADC clock edge, a full 40 MHz period earlier, so
    it has the same time to settle as before.

    Everything downstream of the enable counts samples rather than cycles, so
    the ramp and the sequence number advance at the sampling rate and not at
    the system clock rate.

************************************************************************/

module dataGenerator (
    input       reset_n,
    input       clock,
    input       sample_enable,
    input [9:0] adc_databus,
    input       test_mode_flag,

    // Outputs
    output [15:0] data_out
);

    // Register to store ADC data values
    reg [9:0] adc_data;

    // Register to store test data values
    reg [9:0] test_data;

    // Samples carrying each sequence number, and how many sequence numbers
    // there are before the count wraps.
    //
    // 65535 and not 65536, and the odd number is the whole point. The host
    // detects loss by predicting the next sequence number and disagreeing with
    // what arrives, so the one gap it cannot see is a gap that is an exact
    // multiple of the whole period - all 63 blocks - because that leaves the
    // stream in the phase it would have been in anyway.
    //
    // USB 3 loses capture data a whole endpoint packet at a time: 1024 bytes,
    // which is 512 samples. With a block of 65536 the period is 63 * 65536
    // samples, and 8064 lost packets land exactly on it - so a hole of
    // 7.875 MiB, a tenth of a second of capture, would read as no hole at all.
    // An odd block length shares no factor of two with a packet, so the
    // smallest hole that is both a whole number of packets and a whole period
    // becomes 512 periods: just under 4 GiB, or 53 seconds of capture, which
    // every other thing the host counts would have noticed long before.
    //
    // The host tolerates both lengths - see sequence_validator.cpp - so a
    // board carrying the older gateware still captures.
    localparam [15:0] BlockSamples = 16'd65535;
    localparam [15:0] BlockLast = BlockSamples - 16'd1;
    localparam [15:0] BlockZero = 16'd0;
    localparam [15:0] BlockOne = 16'd1;

    localparam [5:0] SequenceLast = 6'd62;
    localparam [5:0] SequenceZero = 6'd0;
    localparam [5:0] SequenceOne = 6'd1;

    // Register to store the sequence number and the position within the block
    // of samples that number covers
    reg [ 5:0] sequence_count;
    reg [15:0] sequence_position;

    // The top 6 bits of the output are the sequence number
    assign data_out[15:10] = sequence_count;

    // If we are in test-mode use test data,
    // otherwise use the actual ADC data
    assign data_out[9:0]   = test_mode_flag ? test_data : adc_data;

    // Read the ADC data and increment the counters, once per sample
    //
    // Note: The test data is a repeating pattern of incrementing
    // values from 0 to 1020.
    //
    // The sequence number counts from 0 to 62 repeatedly, with each
    // number being attached to 65535 samples.
    always @(posedge clock, negedge reset_n) begin
        if (!reset_n) begin
            adc_data          <= 10'd0;
            test_data         <= 10'd0;
            sequence_count    <= SequenceZero;
            sequence_position <= BlockZero;
        end else if (sample_enable) begin
            // Read the ADC data
            adc_data <= adc_databus;

            // Test mode data generation
            if (test_data == 10'd1021 - 1) begin
                test_data <= 10'd0;
            end else begin
                test_data <= test_data + 10'd1;
            end

            // Sequence number generation. Two counters rather than the
            // single wide one this used to be: the top bits of a 22-bit
            // counter are only a sequence number when the block length is a
            // power of two, and it deliberately is not.
            if (sequence_position == BlockLast) begin
                sequence_position <= BlockZero;

                if (sequence_count == SequenceLast) begin
                    sequence_count <= SequenceZero;
                end else begin
                    sequence_count <= sequence_count + SequenceOne;
                end
            end else begin
                sequence_position <= sequence_position + BlockOne;
            end
        end
    end

endmodule
