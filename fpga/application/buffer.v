/************************************************************************

    buffer.v

    Data buffer module
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2018-2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

    Sits between the sampling side, which produces one word every second
    system-clock cycle, and the FX3, which takes them away a packet at a time
    and can be stalled for a long time by the USB 3 host. The buffer is what
    lets the FX3 fall behind and catch up without a sample being lost.

    One FIFO, not the ping-pong pair this module used to be. The pair existed
    because a dual-clock FIFO cannot report an occupancy that is exact - the
    read side sees the write side's word count through a synchroniser chain,
    several cycles stale - so "is a whole packet ready" had to be answered by
    filling one buffer completely and swapping. With a single clock the count
    is exact on the cycle, so the question is a comparison and the second
    buffer has nothing to do.

    Two consequences worth stating, because both are improvements rather than
    translations:

    An overflow now drops the samples that do not fit. The old module reacted
    by asynchronously clearing the whole 8192-word buffer the FX3 had not
    finished with, so a stall that cost one sample threw away up to 8192 that
    had already been captured. The sequence numbers dataGenerator stamps into
    the stream let the host see the gap either way, so the only difference is
    how much is lost.

    The error flag is held for a fixed number of cycles so the FX3 cannot miss
    it, which is what the old module intended - but its hold counter was never
    cleared, so only the *first* overflow of a session was held. Every one
    after it raised the flag for a single cycle. That is fixed here.

************************************************************************/

module buffer #(
    // 16 for the two-byte GPIF word this board has always used; 32 for the
    // wide bus that halves the word rate for the same byte throughput. Every
    // size below is derived from this so that widening it cannot silently
    // leave one of them at the old width - see the note at PacketWords.
    parameter integer DataWidth = 16
) (
    input reset_n,
    input clock,

    // One write per DataWidth/16 samples: at 16 bits, one assertion per
    // sample; at 32, one every second sample, because that is what packing
    // two samples into one wide word means for how often there is a whole
    // one to write.
    input                  write_enable,
    input [DataWidth-1:0] data_in,

    // High for each cycle the FX3 is taking a word off the databus
    input                  is_reading,
    output [DataWidth-1:0] data_out,

    output reg data_available,
    output reg buffer_error,

    // The back-pressure instrument. telemetry_latch samples it, and everything
    // else about it leaves this module read-only - see bufferMonitor.v.
    input          telemetry_latch,
    output [127:0] telemetry,
    output [ 47:0] telemetry_geometry
);

    // The packet size, in words, held constant in bytes rather than in words
    // as DataWidth changes - 16 KiB either way, which is the number the FX3's
    // DMA buffer, the count in fx3StateMachine, and one USB 3 bulk endpoint
    // buffer all still have to agree on. 8192 words at 16 bits, 4096 at 32:
    // the wide bus halves the word rate for the same byte throughput, and
    // this is the other half of making that true - the packet still holds
    // the same 16 KiB, just as fewer, wider words.
    localparam integer PacketWords = 131072 / DataWidth;

    // Twice the packet size. The headroom above the threshold is what a USB
    // stall is paid for out of: 16 KiB of headroom at 40 MSPS x 16 bits is
    // 205 us of grace, the same as the old pair of buffers gave, in the same
    // total memory - and the same 16 KiB of headroom at a wider word is the
    // same grace in time, because it is still the same number of bytes.
    localparam integer FifoDepth = 2 * PacketWords;

    // Three quarters of the depth, which is half the headroom above the packet
    // threshold. Occupancy at or above this is what the instrument counts time
    // against: the FIFO reaching half is ordinary, and reaching this means half
    // of what a stall is paid out of has already been spent.
    localparam integer NearFullWords = (3 * PacketWords) / 2;

    localparam integer UsedBits = $clog2(FifoDepth + 1);
    localparam integer PacketBits = $clog2(PacketWords + 1);

    // Sized constants, made by part-selecting a 32-bit value for the reason
    // given at the head of fifo.v
    localparam [31:0] PacketValue = PacketWords;
    localparam [UsedBits-1:0] PacketThreshold = PacketValue[UsedBits-1:0];
    localparam [PacketBits-1:0] PacketCount = PacketValue[PacketBits-1:0];
    localparam [PacketBits-1:0] PacketZero = {PacketBits{1'b0}};
    localparam [PacketBits-1:0] PacketOne = {{(PacketBits - 1) {1'b0}}, 1'b1};

    // How long the error flag is held, in system clock cycles. 2000 cycles at
    // 80 MHz is 25 us, which is what 1000 cycles of the old 40 MHz write clock
    // came to - the FX3 samples this pin per packet, so the flag has to
    // outlast a packet's worth of indifference.
    localparam [11:0] ErrorHoldCycles = 12'd2000;
    localparam [11:0] ErrorHoldZero = 12'd0;
    localparam [11:0] ErrorHoldOne = 12'd1;

    wire [UsedBits-1:0] used_words;
    wire                fifo_full;

    // A write that arrives with the FIFO full is the overflow. The FIFO
    // discards it - that is its stated contract, so gating the request here as
    // well would be a second copy of the same decision - and this is only
    // what raises the flag about it.
    wire                overflow = write_enable && fifo_full;

    fifo #(
        .DataWidth(DataWidth),
        .Depth    (FifoDepth)
    ) fifo_0 (
        .reset_n      (reset_n),
        .clock        (clock),
        .write_request(write_enable),
        .data_in      (data_in),
        .read_request (is_reading),
        .data_out     (data_out),
        .full         (fifo_full),
        .used_words   (used_words)
    );

    // The back-pressure instrument ------------------------------------------
    //
    // Given the same signals this module reasons about, and given them as
    // inputs only. It cannot affect any of them, which is what makes it safe to
    // read a running capture with.

    bufferMonitor #(
        .FifoDepth    (FifoDepth),
        .PacketWords  (PacketWords),
        .NearFullWords(NearFullWords)
    ) buffer_monitor_0 (
        .reset_n     (reset_n),
        .clock       (clock),
        .used_words  (used_words),
        .write_enable(write_enable),
        .overflow    (overflow),
        .is_reading  (is_reading),
        .latch       (telemetry_latch),
        .telemetry   (telemetry),
        .geometry    (telemetry_geometry)
    );

    // Packet availability ---------------------------------------------------
    //
    // data_available states that a whole packet can be read without the FX3
    // ever having to wait, so it is raised only once a whole packet is queued
    // and then held for the length of that packet. Holding it is what the
    // ping-pong pair did - it set the flag when a buffer filled and cleared it
    // when that buffer emptied - and the GPIF II state machine on the other
    // side of the pin was designed against that. Dropping the flag the moment
    // the occupancy fell back below a packet would be a truthful signal and a
    // different contract.
    reg [PacketBits-1:0] packet_remaining;

    always @(posedge clock, negedge reset_n) begin
        if (!reset_n) begin
            data_available   <= 1'b0;
            packet_remaining <= PacketZero;
        end else if (!data_available) begin
            if (used_words >= PacketThreshold) begin
                data_available   <= 1'b1;
                packet_remaining <= PacketCount;
            end
        end else if (is_reading) begin
            if (packet_remaining == PacketOne) begin
                data_available   <= 1'b0;
                packet_remaining <= PacketZero;
            end else begin
                packet_remaining <= packet_remaining - PacketOne;
            end
        end
    end

    // The overflow flag -----------------------------------------------------

    reg [11:0] error_hold;

    always @(posedge clock, negedge reset_n) begin
        if (!reset_n) begin
            buffer_error <= 1'b0;
            error_hold   <= ErrorHoldZero;
        end else if (overflow) begin
            // Restarting the count on every overflow, rather than only on the
            // first, is the fix described in the header
            buffer_error <= 1'b1;
            error_hold   <= ErrorHoldZero;
        end else if (buffer_error) begin
            if (error_hold >= ErrorHoldCycles) begin
                buffer_error <= 1'b0;
                error_hold   <= ErrorHoldZero;
            end else begin
                error_hold <= error_hold + ErrorHoldOne;
            end
        end
    end

endmodule
