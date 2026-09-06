/************************************************************************

    DomesdayDuplicator.v

    Top-level module
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2018-2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

`include "version.vh"

module DomesdayDuplicator #(
    // 16 for the word width every board shipped with until now; 32 to use
    // the wide half of the bus the hardware has always had wired (see the
    // note below) and halve the FX3 transfer rate for the same byte
    // throughput. A build for a board that still needs 16 bits overrides
    // this alone - buffer.v, fx3StateMachine.v and the pin mapping below all
    // derive from it rather than each carrying their own copy of the choice.
    //
    // Only a value this file's own gateware understands is safe here: 32
    // needs the FX3's GPIF II reconfigured to match, which is a firmware
    // change this parameter cannot make by itself. Do not set this to 32
    // without that firmware alongside it.
    parameter integer Fx3DataWidth = 32
) (
    input         CLOCK_50,
    inout  [33:0] GPIO0,
    inout  [33:0] GPIO1,
    output [ 7:0] LED
);

    // FX3 Hardware mapping begins ------------------------------------------------

    // Generic pin-mapping for FX3 (DomDupBoard revisions 2_0 to 3_0)
    wire [Fx3DataWidth-1:0] fx3_databus;  // 32-bit databus (16 or 32 bits used)
    wire [            12:0] fx3_control;  // 13-bit control bus
    wire                    fx3_clock;  // FX3 GPIF Clock

    // The lower 16 bits of the data bus physical mapping (output only).
    // Present at every width - the upper half, when it exists, is mapped
    // separately below, on GPIO0 rather than GPIO1, because that is where
    // the board's remaining 16 traces to the FX3 run.
    assign GPIO1[32] = fx3_databus[00];
    assign GPIO1[30] = fx3_databus[01];
    assign GPIO1[28] = fx3_databus[02];
    assign GPIO1[26] = fx3_databus[03];
    assign GPIO1[24] = fx3_databus[04];
    assign GPIO1[22] = fx3_databus[05];
    assign GPIO1[20] = fx3_databus[06];
    assign GPIO1[18] = fx3_databus[07];
    assign GPIO1[16] = fx3_databus[08];
    assign GPIO1[14] = fx3_databus[09];
    assign GPIO1[12] = fx3_databus[10];
    assign GPIO1[10] = fx3_databus[11];
    assign GPIO1[08] = fx3_databus[12];
    assign GPIO1[06] = fx3_databus[13];
    assign GPIO1[04] = fx3_databus[14];
    assign GPIO1[02] = fx3_databus[15];

    // The upper 16 bits of the data bus, on the board's other 16 traces to
    // the FX3 - GPIO0 rather than GPIO1, because GPIO1 only ever carried 16.
    // Driven only at Fx3DataWidth == 32; tri-stated otherwise, exactly as
    // every board has left them since the pins were first wired, because at
    // 16 bits nothing on the other end expects them to be anything else.
    generate
        if (Fx3DataWidth == 32) begin : gen_wide_databus
            assign GPIO0[02] = fx3_databus[16];
            assign GPIO0[03] = fx3_databus[17];
            assign GPIO0[04] = fx3_databus[18];
            assign GPIO0[05] = fx3_databus[19];
            assign GPIO0[06] = fx3_databus[20];
            assign GPIO0[07] = fx3_databus[21];
            assign GPIO0[12] = fx3_databus[22];
            assign GPIO0[13] = fx3_databus[23];
            assign GPIO0[14] = fx3_databus[24];
            assign GPIO0[15] = fx3_databus[25];
            assign GPIO0[16] = fx3_databus[26];
            assign GPIO0[17] = fx3_databus[27];
            assign GPIO0[18] = fx3_databus[28];
            assign GPIO0[19] = fx3_databus[29];
            assign GPIO0[20] = fx3_databus[30];
            assign GPIO0[21] = fx3_databus[31];
        end else begin : gen_narrow_databus
            assign GPIO0[02] = 1'bZ;
            assign GPIO0[03] = 1'bZ;
            assign GPIO0[04] = 1'bZ;
            assign GPIO0[05] = 1'bZ;
            assign GPIO0[06] = 1'bZ;
            assign GPIO0[07] = 1'bZ;
            assign GPIO0[12] = 1'bZ;
            assign GPIO0[13] = 1'bZ;
            assign GPIO0[14] = 1'bZ;
            assign GPIO0[15] = 1'bZ;
            assign GPIO0[16] = 1'bZ;
            assign GPIO0[17] = 1'bZ;
            assign GPIO0[18] = 1'bZ;
            assign GPIO0[19] = 1'bZ;
            assign GPIO0[20] = 1'bZ;
            assign GPIO0[21] = 1'bZ;
        end
    endgenerate

    // FX3 Clock physical mapping
    assign GPIO1[31]       = fx3_clock;  // FX3 GPIO_16

    // 13-bit control bus physical mapping (outputs)
    assign GPIO1[27]       = fx3_control[00];  // FX3 CTL_00 GPIO_17 (output)
    assign GPIO1[21]       = fx3_control[03];  // FX3 CTL_03 GPIO_20 (output)
    assign GPIO1[19]       = fx3_control[04];  // FX3 CTL_04 GPIO_21 (output)
    assign GPIO1[13]       = fx3_control[07];  // FX3 CTL_07 GPIO_24 (output)
    assign GPIO1[05]       = fx3_control[11];  // FX3 CTL_11 GPIO_28 (output)
    assign GPIO1[03]       = fx3_control[12];  // FX3 CTL_12 GPIO_29 (output)

    // 13-bit control bus physical mapping (inputs)
    assign fx3_control[01] = GPIO1[25];  // FX3 CTL_01 GPIO_18
    assign fx3_control[02] = GPIO1[23];  // FX3 CTL_02 GPIO_19
    assign fx3_control[05] = GPIO1[17];  // FX3 CTL_05 GPIO_22
    assign fx3_control[06] = GPIO1[15];  // FX3 CTL_06 GPIO_23
    assign fx3_control[08] = GPIO1[11];  // FX3 CTL_08 GPIO_25
    assign fx3_control[09] = GPIO1[09];  // FX3 CTL_09 GPIO_26
    assign fx3_control[10] = GPIO1[07];  // FX3 CTL_10 GPIO_27

    // High-Z the unused GPIO0 pins
    assign GPIO0[0]        = 1'bZ;
    assign GPIO0[1]        = 1'bZ;
    assign GPIO0[8]        = 1'bZ;
    assign GPIO0[9]        = 1'bZ;
    assign GPIO0[10]       = 1'bZ;
    assign GPIO0[11]       = 1'bZ;
    assign GPIO0[22]       = fx3_range_select;  // RSEL on the ADS828
    assign GPIO0[23]       = 1'bZ;
    assign GPIO0[24]       = 1'bZ;
    assign GPIO0[25]       = 1'bZ;
    assign GPIO0[26]       = 1'bZ;
    assign GPIO0[27]       = 1'bZ;
    assign GPIO0[28]       = 1'bZ;
    assign GPIO0[29]       = 1'bZ;
    assign GPIO0[30]       = 1'bZ;
    assign GPIO0[31]       = 1'bZ;
    assign GPIO0[32]       = 1'bZ;

    // High-Z the unused GPIO1 pins
    assign GPIO1[0]        = 1'bZ;
    assign GPIO1[1]        = 1'bZ;
    assign GPIO1[7]        = 1'bZ;
    assign GPIO1[9]        = 1'bZ;
    assign GPIO1[11]       = 1'bZ;
    assign GPIO1[15]       = 1'bZ;
    assign GPIO1[17]       = 1'bZ;
    assign GPIO1[23]       = 1'bZ;
    assign GPIO1[25]       = 1'bZ;
    assign GPIO1[29]       = 1'bZ;
    assign GPIO1[33]       = 1'bZ;

    // FX3 Signal mapping:
    //
    // Signal            GPIO     CTL     Direction Description
    //
    // CLK               GPIO16   PCLK    Output    - Data clock
    // Databus           GPIO0:15         Output    - Databus
    // data_available    GPIO_17  CTL_00  Output    - FPGA signals if data is available for reading
    // reset_n           GPIO_27  CTL_10  Input     - FX3 signals (not) reset condition
    // collect_data      GPIO_19  CTL_02  Input     - Unused
    // read_data         GPIO_18  CTL_01  Input     - FX3 signals it is reading from the databus
    //
    // input0            GPIO_20  CTL_03  Output    - Buffer error flag from FPGA
    // input1            GPIO_21  CTL_04  Output    - Unused
    // input2            GPIO_28  CTL_11  Output    - Unused
    // input3            GPIO_29  CTL_12  Output    - Unused
    //
    // spi_clock         GPIO_22  CTL_05  Input     - SPI clock from the FX3
    // spi_mosi          GPIO_23  CTL_06  Input     - SPI data from the FX3
    // spi_miso          GPIO_24  CTL_07  Output    - SPI data to the FX3
    // spi_chip_select_n GPIO_25  CTL_08  Input     - SPI chip select from the FX3 (active low)
    // (reserved)        GPIO_26  CTL_09  Input     - Unused; wired and held for a future signal

    // The four SPI lines replace what were five one-bit configuration signals, of
    // which only test mode was ever used. They reach the register bank in
    // spiRegisters, which is where test mode and the status LEDs now live; the
    // contract is the "FPGA register interface" page of the documentation site.

    // Wire definitions for FX3 GPIO mapping
    wire       fx3_reset_n;
    wire       fx3_data_available;
    wire       fx3_read_data;
    wire       fx3_buffer_error;
    wire       fx3_spi_clock;
    wire       fx3_spi_mosi;
    wire       fx3_spi_miso;
    wire       fx3_spi_chip_select_n;
    wire       fx3_test_mode;
    wire       fx3_range_select;
    wire [7:0] fx3_decimation;
    wire [7:0] fx3_pll_preset_unused;

    // Signal outputs to FX3
    assign fx3_control[00]       = fx3_data_available;
    assign fx3_control[03]       = fx3_buffer_error;
    assign fx3_control[07]       = fx3_spi_miso;

    // These are currently unused, but must have a defined value
    assign fx3_control[04]       = 1'b0;
    assign fx3_control[11]       = 1'b0;
    assign fx3_control[12]       = 1'b0;

    // Signal inputs from FX3
    assign fx3_reset_n           = fx3_control[10];
    //assign fx3_unused = fx3_control[02];
    assign fx3_read_data         = fx3_control[01];

    // Signal inputs from FX3 (SPI register interface)
    assign fx3_spi_clock         = fx3_control[05];
    assign fx3_spi_mosi          = fx3_control[06];
    assign fx3_spi_chip_select_n = fx3_control[08];

    // FX3 Hardware mapping ends --------------------------------------------------


    // ADC Hardware mapping begins ------------------------------------------------

    wire [9:0] adc_databus;

    // 10-bit databus from ADC
    assign adc_databus[0] = GPIO0[32];
    assign adc_databus[1] = GPIO0[31];
    assign adc_databus[2] = GPIO0[30];
    assign adc_databus[3] = GPIO0[29];
    assign adc_databus[4] = GPIO0[28];
    assign adc_databus[5] = GPIO0[27];
    assign adc_databus[6] = GPIO0[26];
    assign adc_databus[7] = GPIO0[25];
    assign adc_databus[8] = GPIO0[24];
    assign adc_databus[9] = GPIO0[23];

    // ADC clock output - a divide-by-two of the system clock, generated below
    wire adc_clock;
    assign GPIO0[33] = adc_clock;

    // ADC Hardware mapping ends --------------------------------------------------


    // Application logic begins ---------------------------------------------------


    // PLL clock generation
    //
    // One 80 MHz system clock from the 50 MHz physical clock, and the whole
    // design runs from it. There is no second clock domain: the sampling rate
    // is set by decimating this clock rather than by a clock of its own, which
    // is what lets the FX3 drain faster than the ADC fills without the two
    // sides having to be synchronised to each other.
    //
    // 80 MHz because it is the lowest multiple of the 40 MSPS sampling rate
    // that leaves the FX3 room to catch up. The GPIF II interface is specified
    // to 100 MHz, so this is inside it; the ADC needs a uniform clock, so the
    // system clock has to be an exact multiple of the sampling rate, which
    // 60 MHz was not.
    wire system_clock;

    IPpllGenerator pll_generator_0 (
        // Inputs
        .inclk0(CLOCK_50),

        // Outputs
        .c0(system_clock)  // 80 MHz system clock
    );

    // ADC sampling clock and the sampling instant
    //
    // A divide-by-two of the system clock, free-running and with no reset:
    // the ADC is a pipelined converter whose analogue behaviour was
    // characterised with a clock that is always present, and stopping it
    // whenever the host closes the device would be a change to the front end
    // rather than to the logic. The initial value is the power-up state, and
    // which phase it powers up in does not matter.
    //
    // sample_enable is high on the system clock edge that takes adc_clock
    // high, so the design captures the ADC bus at the same instant it did when
    // this module was clocked by the ADC clock directly - one full 40 MHz
    // period after the sample was launched.
    reg adc_clock_divider;

    initial begin
        adc_clock_divider = 1'b0;
    end

    always @(posedge system_clock) begin
        adc_clock_divider <= ~adc_clock_divider;
    end

    assign adc_clock = adc_clock_divider;

    // The FX3's GPIF II is a synchronous slave and this pin is the clock it
    // runs from.
    //
    // At Fx3DataWidth == 32, this is adc_clock itself: system_clock = 2 x
    // adc_clock still holds (it is what gives the decimation filter's
    // pipeline its slack, see halfBandDecimator.v), but the interface no
    // longer needs to be faster than the sampling rate to give the FX3 room
    // to catch up - the wide bus buys that margin instead, by halving the
    // word rate for the same byte throughput. Driving the interface from the
    // undivided system clock here would put 150 MHz on a pin specified for a
    // small fraction of that.
    //
    // At 16 bits this is unchanged from what this design has always done:
    // the undivided system clock, twice the sampling rate, which is where
    // the FX3's headroom to catch up has always come from on that bus width.
    generate
        if (Fx3DataWidth == 32) begin : gen_fx3_clock_divided
            assign fx3_clock = adc_clock_divider;
        end else begin : gen_fx3_clock_undivided
            assign fx3_clock = system_clock;
        end
    endgenerate

    wire       sample_enable = ~adc_clock_divider;

    // Reset synchroniser
    //
    // fx3_reset_n is driven by the FX3 and is asynchronous to the system
    // clock. Asserting asynchronously is what the design has always done and
    // is what makes a reset work with no clock; releasing it synchronously is
    // new, and is what stops two registers coming out of reset on different
    // cycles because they resolved the same asynchronous edge differently.
    reg  [1:0] reset_n_sync;

    always @(posedge system_clock, negedge fx3_reset_n) begin
        if (!fx3_reset_n) begin
            reset_n_sync <= 2'b00;
        end else begin
            reset_n_sync <= {reset_n_sync[0], 1'b1};
        end
    end

    wire reset_n = reset_n_sync[1];

    // ADC capture delay ------------------------------------------------------
    //
    // sample_enable fires one system_clock cycle after adc_clock's rising
    // edge - one ADC period after the sample was launched, per the comment
    // above adc_clock_divider. At 75 MHz that period alone (13.33 ns) is
    // shorter than the real settling time of the ADC plus its output buffer
    // (measured against the ADS828 and SN74LVTH541 datasheets: about 15.5 ns
    // worst case) - so capturing there reads the bus before it has finished
    // settling, not after.
    //
    // The ADS828's own data-hold spec (t1) guarantees the *previous* value
    // stays valid well past that same edge - the real valid window for a
    // given sample sits later than a single ADC period, not earlier - so the
    // fix is to wait one system_clock cycle longer before capturing, not to
    // touch adc_clock itself. Delaying adc_clock would only shrink the gap
    // between launch and capture; only delaying the capture widens it.
    //
    // This is a plain one-cycle register delay of sample_enable, not a
    // fractional PLL phase shift: it is derived from sample_enable and
    // nothing else, so it cannot drift out of the fixed one-cycle
    // relationship the way two independently-generated counters could.
    reg  sample_enable_delayed;

    always @(posedge system_clock, negedge reset_n) begin
        if (!reset_n) begin
            sample_enable_delayed <= 1'b0;
        end else begin
            sample_enable_delayed <= sample_enable;
        end
    end

    wire        fx3_is_reading;
    wire [15:0] data_generator_out;

    // Sample rate
    //
    // The register holds the decimation factor rather than a flag, so that a
    // host reading it back gets a positive statement of what the capture path
    // is doing rather than an echo of what it asked for. One, two and four
    // are the factors this gateware implements; the bank normalises anything
    // else to one. Four is two half-band stages in series rather than a
    // filter of its own - the first stage's decision to decimate covers both
    // the 2:1 and 4:1 cases, and the second stage only joins in for 4:1.
    wire        fx3_decimate_stage1 = (fx3_decimation == 8'h02) || (fx3_decimation == 8'h04);
    wire        fx3_decimate_stage2 = (fx3_decimation == 8'h04);

    wire [ 9:0] capture_sample_stage1;
    wire        capture_enable_stage1;
    wire [ 9:0] capture_sample;
    wire        capture_enable;

    // Anti-aliased decimation, for tape capture
    //
    // In front of the data generator rather than behind it, which is the
    // arrangement that keeps the sequence counter honest: the counter is
    // attached to the samples that survive, so a decimated capture carries an
    // unbroken count and the host's integrity check works on it unchanged. A
    // decimator placed after the generator would drop samples' sequence
    // numbers and every capture would read as damaged.
    //
    // It filters the ADC and nothing else. The test pattern is generated
    // downstream at the rate the decimator sets, so a test capture is an
    // unbroken ramp at whichever rate is selected and the integrity oracle
    // covers every decimated path as well as the full-rate one.
    //
    // Stage 1: undecimated to 2:1.
    halfBandDecimator half_band_decimator_0 (
        // Inputs
        .reset_n      (reset_n),                // Not reset
        .clock        (system_clock),           // 80 MHz system clock
        .sample_enable(sample_enable_delayed),  // 1 = a sample arrives on this edge
        .data_in      (adc_databus),            // 10-bit ADC databus
        .decimate     (fx3_decimate_stage1),    // 1 = filter and halve the rate

        // Outputs
        .data_out     (capture_sample_stage1),  // 10-bit filtered sample
        .output_enable(capture_enable_stage1)   // 1 = a sample worth keeping
    );

    // Stage 2: 2:1 to 4:1. Its own sample_enable is stage 1's output_enable,
    // so it only ever considers a sample stage 1 kept - at 2:1 it is asked to
    // bypass and this stage is a second pass-through, at 4:1 it halves the
    // rate again on top of stage 1's half.
    halfBandDecimator half_band_decimator_1 (
        // Inputs
        .reset_n      (reset_n),                // Not reset
        .clock        (system_clock),           // 80 MHz system clock
        .sample_enable(capture_enable_stage1),  // 1 = stage 1 kept a sample this edge
        .data_in      (capture_sample_stage1),  // 10-bit sample from stage 1
        .decimate     (fx3_decimate_stage2),    // 1 = filter and halve the rate again

        // Outputs
        .data_out     (capture_sample),  // 10-bit filtered sample
        .output_enable(capture_enable)   // 1 = a sample worth keeping
    );

    // Generate 16-bit data either from the ADC or the test data generator
    dataGenerator data_generator_0 (
        // Inputs
        .reset_n       (reset_n),         // Not reset
        .clock         (system_clock),    // 80 MHz system clock
        .sample_enable (capture_enable),  // 1 = take a sample on this edge
        .adc_databus   (capture_sample),  // 10-bit sample from the decimator
        .test_mode_flag(fx3_test_mode),   // 1 = Test mode on

        // Outputs
        .data_out(data_generator_out)  // 16-bit data out
    );

    // Word packer, active only at Fx3DataWidth == 32 ------------------------
    //
    // Combines two successive 16-bit sample words into one wide transfer
    // word, halving the write rate into the buffer for the same byte
    // throughput as 16 bits at twice the word rate - see buffer.v's
    // PacketWords for the other half of this arrangement. The first sample
    // of a pair lands in the low half and the second in the high half; a
    // host unpacking the wide bus has to agree with that order.
    //
    // At 16 bits this is a wire, not a register: write_enable and data_in
    // reach the buffer exactly as they did before this parameter existed.
    //
    // The packer itself lives in wordPacker.v, not inline here, specifically
    // so it can be simulated - see that file's header for why this top level
    // cannot be.
    wire                    buffer_write_enable;
    wire [Fx3DataWidth-1:0] buffer_data_in;

    generate
        if (Fx3DataWidth == 32) begin : gen_word_packer
            wordPacker word_packer_0 (
                .reset_n      (reset_n),
                .clock        (system_clock),
                .sample_enable(capture_enable),
                .data_in      (data_generator_out),

                .write_enable(buffer_write_enable),
                .data_out    (buffer_data_in)
            );
        end else begin : gen_no_packer
            assign buffer_write_enable = capture_enable;
            assign buffer_data_in      = data_generator_out;
        end
    endgenerate

    // Read-side pacing, at Fx3DataWidth == 32 -------------------------------
    //
    // fx3_clock, the pin, is adc_clock_divider - half the rate everything in
    // this module (including fx3StateMachine and the FIFO read port) is
    // actually clocked at. Without this, the FIFO would give up a word, and
    // fx3StateMachine would count one sent, on every system_clock edge - twice
    // the rate the FX3 can really take them off the pin at. This is the read
    // side's equivalent of sample_enable, and at 32 bits it is sample_enable:
    // both sides only need "once every second system_clock cycle", and reusing
    // one register rather than building a second is one fewer thing that could
    // drift out of step with it.
    //
    // At 16 bits fx3_clock is the undivided system clock - the rate
    // fx3StateMachine and the FIFO already assume - so this is tied high and
    // changes nothing.
    wire fx3_transfer_enable;

    generate
        if (Fx3DataWidth == 32) begin : gen_fx3_transfer_enable_gated
            assign fx3_transfer_enable = sample_enable;
        end else begin : gen_fx3_transfer_enable_always
            assign fx3_transfer_enable = 1'b1;
        end
    endgenerate

    // The capture buffer's back-pressure instrument, on its way to the register
    // bank. The latch pulse comes back the other way and is the only thing the
    // host can do to the buffer at all.
    wire [127:0] buffer_telemetry;
    wire [ 47:0] buffer_telemetry_geometry;
    wire         buffer_telemetry_latch;

    // Words per packet at this bus width, held constant in bytes - see
    // buffer.v's PacketWords, which computes the same number from the same
    // reasoning and cannot share this one because the two modules have no
    // other link between them.
    localparam integer Fx3PacketWords = 131072 / Fx3DataWidth;

    // What the FIFO actually sees as a read this edge: fx3StateMachine says
    // a packet is in flight for the whole packet's duration, and this is what
    // narrows that down to the edges a word may really leave the FIFO on -
    // see the fx3_transfer_enable comment above for why that is not every
    // edge at Fx3DataWidth == 32.
    wire buffer_is_reading = fx3_is_reading && fx3_transfer_enable;

    // FIFO buffer
    buffer #(
        .DataWidth(Fx3DataWidth)
    ) buffer_0 (
        // Inputs
        .reset_n        (reset_n),                // Not reset
        .clock          (system_clock),           // 80 MHz system clock
        .write_enable   (buffer_write_enable),    // 1 = a word is written this edge
        .data_in        (buffer_data_in),         // Fx3DataWidth-bit data bus input
        .is_reading     (buffer_is_reading),      // 1 = a word may be taken off the FIFO this edge
        .telemetry_latch(buffer_telemetry_latch), // 1 = sample the instrument

        // Outputs
        .data_out          (fx3_databus),               // Fx3DataWidth-bit data output
        .data_available    (fx3_data_available),        // Set if a whole packet is queued
        .buffer_error      (fx3_buffer_error),          // Set if a sample had to be dropped
        .telemetry         (buffer_telemetry),          // The instrument's shadow bank
        .telemetry_geometry(buffer_telemetry_geometry)  // and the constants to read it by
    );

    // FX3 GPIF state-machine logic
    fx3StateMachine #(
        .PacketWords(Fx3PacketWords)
    ) fx3_state_machine_0 (
        // Inputs
        .reset_n        (reset_n),             // Not reset
        .fx3_clock      (system_clock),        // 80 MHz system clock
        .read_data      (fx3_read_data),       // FX3 is about to start sampling the databus
        .transfer_enable(fx3_transfer_enable), // 1 = this edge may count as a word sent

        // Output
        .fx3_is_reading(fx3_is_reading)  // Flag to indicate FX3 is sampling the databus
    );

    // SPI register bank
    //
    // The build stamp comes from version.vh, which fpga/generate-version.sh
    // writes into the build directory. The copy committed beside the sources
    // reports no commit, which is the honest answer for a lint or simulation run
    // and for anyone who compiles without running the generator first.
    wire        window_write;
    wire [ 1:0] window_address;
    wire [ 7:0] window_write_data;
    wire [31:0] window_read_data;
    wire        transaction_decoded;

    spiRegisters #(
        .CommitText(`GATEWARE_COMMIT_TEXT),
        .BuildFlags(`GATEWARE_BUILD_FLAGS),

        // This is the capture gateware, which is what a host reads out of
        // IMAGE_ROLE to know it is not looking at a unit in recovery
        .ImageRole(8'h01),

        // and the image that has a capture buffer to report on
        .TelemetryPresent(1'b1),

        // and the only image with a sample stream to decimate
        .DecimationPresent(1'b1),

        // This board's ADS828 converts up to 75 MHz. A build for a board
        // carrying a slower part in the same family overrides this alone -
        // everything else in this file is shared between them.
        .MaxAdcRateMHz(8'd75)

        // PllPresetPresent is left at its default (off): the register exists
        // in the map but there is no ALTPLL_RECONFIG controller behind it in
        // this build yet, so a write to 0x15 has nothing to act on.
    ) spi_registers_0 (
        // Inputs
        .reset_n           (reset_n),
        .clock             (system_clock),
        .spi_clock         (fx3_spi_clock),
        .spi_mosi          (fx3_spi_mosi),
        .spi_chip_select_n (fx3_spi_chip_select_n),
        .window_read_data  (window_read_data),
        .diagnostics       (remote_update_diagnostics),
        .telemetry         (buffer_telemetry),
        .telemetry_geometry(buffer_telemetry_geometry),

        // Outputs
        .spi_miso           (fx3_spi_miso),
        .test_mode          (fx3_test_mode),          // 1 = test data generator selected
        .range_select       (fx3_range_select),       // 1 = 2Vpp, 0 = 1Vpp on the ADS828
        .decimation         (fx3_decimation),         // Samples kept out of every n
        .pll_preset         (fx3_pll_preset_unused),  // Not acted on until 3 lands
        .leds               (LED),                    // Driven by the FX3, for status
        .window_write       (window_write),
        .window_address     (window_address),
        .window_write_data  (window_write_data),
        .transaction_decoded(transaction_decoded),
        .telemetry_latch    (buffer_telemetry_latch)
    );

    // Flash bridge and reconfiguration control
    //
    // The capture gateware carries these so that a gateware update is done from
    // the running application image rather than from the recovery one: the
    // factory image is for when something has gone wrong, and an update is not
    // that. Everything here is inert until the FX3 unlocks it.
    wire [7:0] bridge_unlock_read;
    wire [7:0] bridge_control_read;
    wire [7:0] bridge_data_read;
    wire [7:0] reconfiguration_read;

    wire       flash_clock;
    wire       flash_chip_select_n;
    wire       flash_data_out;
    wire       flash_data_in;
    wire       flash_drive;

    assign window_read_data = {
        reconfiguration_read, bridge_data_read, bridge_control_read, bridge_unlock_read
    };

    flashBridge flash_bridge_0 (
        // Inputs
        .reset_n          (reset_n),
        .clock            (system_clock),
        .window_write     (window_write),
        .window_address   (window_address),
        .window_write_data(window_write_data),
        .flash_data_in    (flash_data_in),

        // Outputs
        .unlock_read        (bridge_unlock_read),
        .control_read       (bridge_control_read),
        .data_read          (bridge_data_read),
        .flash_clock        (flash_clock),
        .flash_chip_select_n(flash_chip_select_n),
        .flash_data_out     (flash_data_out),
        .flash_drive        (flash_drive)
    );

    asmiBlock asmi_block_0 (
        // Inputs
        .dclk           (flash_clock),
        .chip_select_n  (flash_chip_select_n),
        .serial_data_out(flash_data_out),
        .output_enable  (flash_drive),

        // Output
        .serial_data_in(flash_data_in)
    );

    // The watchdog the factory image armed before it handed over is tickled by
    // transaction_decoded, so this image proves its fabric is alive rather than
    // merely proving it configured - which the configuration CRC already did.
    // A host reconfiguration request through this block returns the device to
    // the factory image, which then makes the boot decision again.
    // BENCH DIAGNOSTIC. The remote update block's account of itself,
    // carried to the register bank and presented read-only at 0x30.
    wire [63:0] remote_update_diagnostics;

    remoteUpdate remote_update_0 (
        // Inputs
        .reset_n            (reset_n),
        .clock              (system_clock),
        .window_write       (window_write && (window_address == 2'd3)),
        .window_write_data  (window_write_data),
        .transaction_decoded(transaction_decoded),
        .arm_request        (1'b0),
        .boot_address       (24'd0),
        .reconfigure_request(1'b0),

        // Outputs
        .control_read(reconfiguration_read),
        .diagnostics (remote_update_diagnostics)
    );

endmodule
