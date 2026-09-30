/************************************************************************

    sample_format.h

    The device's sample layout on the wire
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

// Every fact about what arrives from the device, in one place.
//
// The device delivers one 16-bit little-endian word per sample:
//
//     bits  9..0   the 10-bit unsigned sample value, 0..1023
//     bits 15..10  a 6-bit sequence counter
//
// The sequence counter increments once every 65,535 samples and wraps at 62,
// so it repeats every 63 * 65,535 samples. It is the only evidence a capture is
// bit-perfect: a host that stalls loses whole USB packets, and the gap shows up
// as a counter that skipped. Nothing else in the stream would reveal it.
//
// The one gap that cannot show up is a gap of exactly a whole number of
// periods, because that leaves the stream in the phase it would have been in
// anyway. The block length is odd so that no whole number of USB packets can
// ever be one: a SuperSpeed bulk packet is 1,024 bytes, 512 samples, and the
// smallest loss that is both a whole number of packets and a whole period is
// 512 periods — just under 4 GiB, or 53 seconds of capture. Gateware built
// before this was understood used a block of 65,536, where the same figure was
// one period: 7.875 MiB, or a tenth of a second. See issue #186.
//
// These constants are deliberately a component-local copy rather than shared
// with the gateware or the firmware (AGENTS.md §2). They describe a wire
// protocol, not shared code: if the gateware ever changes them, the three
// definitions must be able to disagree for as long as it takes to migrate.
namespace ddd::capture {

// Bytes per sample on the wire
inline constexpr size_t kBytesPerSample = 2;

// The device's real sampling rate
inline constexpr uint32_t kSampleRateHz = 40'000'000;

// 80 MB/s, continuously, for as long as a capture runs. Stated as a constant
// because almost every buffer-sizing decision in the engine is derived from it.
inline constexpr uint64_t kWireBytesPerSecond =
    static_cast<uint64_t>(kSampleRateHz) * kBytesPerSample;

// Sample value limits. A sample sitting at either end is clipped, and the count
// of those is what tells a user their RF gain is wrong.
inline constexpr uint16_t kMinimumSampleValue = 0;
inline constexpr uint16_t kMaximumSampleValue = 1023;

// The 10-bit value that represents zero once the signal is centred
inline constexpr int32_t kSampleZeroOffset = 512;

// Scale between the 10-bit and 16-bit representations
inline constexpr int32_t kSampleScale = 64;

// Mask selecting the sample value out of a wire word
inline constexpr uint16_t kSampleValueMask = 0x03FF;

// The sequence counter occupies the top six bits of the word. It is expressed
// here in terms of the high byte as well, because the hot loop reads bytes: it
// never assembles the 16-bit word just to extract the counter.
inline constexpr int kSequenceCounterShift = 10;
inline constexpr uint8_t kSequenceCounterHighByteShift = 2;
inline constexpr uint8_t kSampleValueHighByteMask = 0x03;

// Sequence counter values run 0..62 inclusive — 63 distinct values, not 64.
inline constexpr uint32_t kSequenceCounterValues = 63;

// Samples carrying each sequence counter value, as the gateware in this tree
// emits them.
inline constexpr uint32_t kSamplesPerSequenceCounter = 65'535;

// What gateware predating the odd block length emits. The host accepts either,
// because a board is updated when its owner updates it and a capture from an
// older one is still a capture worth proving. Both are what the validator
// measures against; nothing else is a block length this project has shipped.
inline constexpr uint32_t kLegacySamplesPerSequenceCounter = 65'536;

// The longest run of one counter value any gateware produces. What a validator
// searching for its first counter change has to look through before concluding
// there are no markers at all.
inline constexpr uint32_t kMaximumSamplesPerSequenceCounter =
    kLegacySamplesPerSequenceCounter;

// Is this a run length one of the shipped gatewares produces? The validator
// learns the block length from the stream rather than being told it, and this
// is the only thing standing between "learned it" and "believed the first
// number it saw".
inline constexpr bool IsKnownSamplesPerSequenceCounter(uint32_t samples) {
  return samples == kSamplesPerSequenceCounter ||
         samples == kLegacySamplesPerSequenceCounter;
}

// Extract the 10-bit sample value from a little-endian wire word.
inline constexpr uint16_t SampleValueFromWord(uint16_t word) {
  return static_cast<uint16_t>(word & kSampleValueMask);
}

// Extract the 6-bit sequence counter from a little-endian wire word.
inline constexpr uint8_t SequenceCounterFromWord(uint16_t word) {
  return static_cast<uint8_t>(word >> kSequenceCounterShift);
}

// Build a wire word from a sample value and a sequence counter. Used by the
// synthetic source and by every test that needs to fabricate device data.
inline constexpr uint16_t MakeWireWord(uint16_t sample_value,
                                       uint8_t sequence_counter) {
  return static_cast<uint16_t>(
      (sample_value & kSampleValueMask) |
      static_cast<uint16_t>(static_cast<uint16_t>(sequence_counter)
                            << kSequenceCounterShift));
}

// Convert a 10-bit unsigned sample to the signed 16-bit representation
// ld-decode calls the DdD 16-bit format.
//
// This is not a local convention: lddecode/lds.py unpacks captures into exactly
// this before handing them to flac, which is why a FLAC file written here needs
// no format negotiation with the decode toolchain.
inline constexpr int16_t ToSigned16Bit(int32_t ten_bit_value) {
  return static_cast<int16_t>((ten_bit_value - kSampleZeroOffset) *
                              kSampleScale);
}

// The same, with a board's declared DC offset taken out (board_setup.h).
//
// The offset is in whole converter codes, so the result is still a code times
// 64 and its six low bits are still zero — which is what FLAC stores for
// nothing, and what a fractional correction would have made it store.
//
// The ten bits already fill the sixteen exactly, so shifting them by any
// offset at all pushes the codes nearest one end of the converter out of
// range. Those are saturated to the end of the 16-bit range rather than cast,
// because a cast wraps: a sample just past the bottom would come out near the
// top, a full-scale spike in the other direction in the middle of the signal.
// With an offset measured on the board it is correcting this cannot happen on
// its own — see DcOffsetSaturates().
inline constexpr int16_t ToCorrectedSigned16Bit(int32_t ten_bit_value,
                                                int32_t dc_offset) {
  const int32_t scaled =
      (ten_bit_value - kSampleZeroOffset - dc_offset) * kSampleScale;
  if (scaled < INT16_MIN) {
    return INT16_MIN;
  }
  if (scaled > INT16_MAX) {
    return INT16_MAX;
  }
  return static_cast<int16_t>(scaled);
}

// Whether a sample the converter did *not* clip is pushed out of range by the
// DC offset correction — the correction clipping it rather than the ADC.
//
// Codes 0 and 1023 are excluded because they are already clipped, by the
// converter, and are counted as such; this is the count of the ones only the
// correction lost. On a board whose offset was measured with nothing connected
// it is zero: an AC-coupled signal is symmetric about its mean, which is the
// offset, so it cannot reach the far end of the corrected range without
// clipping the converter at the near end first. A non-zero count therefore
// says the declared offset does not belong to this board.
inline constexpr bool DcOffsetSaturates(uint16_t ten_bit_value,
                                        int32_t dc_offset) {
  const auto value = static_cast<int32_t>(ten_bit_value);
  if (value <= kMinimumSampleValue || value >= kMaximumSampleValue) {
    return false;
  }
  const int32_t corrected = value - kSampleZeroOffset - dc_offset;
  return corrected < -kSampleZeroOffset || corrected >= kSampleZeroOffset;
}

// The inverse, for reading a capture back into the 10-bit domain the device's
// test pattern counts in.
inline constexpr int32_t ToTenBit(int16_t signed_value) {
  return (signed_value / kSampleScale) + kSampleZeroOffset;
}

}  // namespace ddd::capture
