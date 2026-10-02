/************************************************************************

    sample_format.h

    The device's sample layout on the wire
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <algorithm>
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

// The converter's own resolution.
inline constexpr int kConverterBits = 10;

// The largest bit shift: four bits, a digital gain of x16.
inline constexpr int kMaximumBitShift = 4;

// The most low bits a capture may drop: four, leaving six of the ten.
//
// Each bit dropped costs 6 dB of quantisation noise, and beyond two the sync
// tips, the dropout detection and the chroma in the FM sidebands start to be
// what is lost rather than fine detail — allowed, because it is the user's
// trade to make, and recorded in every file so that it can never be mistaken
// for a full capture.
inline constexpr int kMaximumLsbDrop = 4;

// Everything done to a converter code on its way into a capture — every
// writer applies exactly this, and every capture records exactly this. Each is
// the action taken, not what it leaves, which is also how the files record it.
//
//  - dc_offset: the board's declared DC offset, in whole converter codes,
//    taken out (board_setup.h).
//  - bit_shift: how many bits the signal is shifted up, 0 to 4 — a digital
//    gain of 2^bit_shift. It adds no information and costs FLAC nothing — the
//    bits it opens at the bottom are zero in every sample, which FLAC stores
//    for free — but a weak signal at full scale is easier to read in every
//    tool that displays one.
//  - lsb_drop: how many of the converter's low bits are dropped, 0 to 4.
//    Dropping bits is the one way to make a capture meaningfully smaller: the
//    low bits are mostly noise and noise does not compress, so each one
//    dropped saves nearly a bit a sample. Counted in the converter's bits
//    whatever the shift, so that a shift and a drop cannot cancel: a shift of
//    two with two bits dropped keeps eight bits of the signal, not ten with
//    two zeros removed.
//
// Declared, and applied, in that order, and the order is the point — see
// UnsaturatedConverted(). Values outside those ranges are clamped wherever
// they are used, so a settings file from elsewhere cannot ask for something no
// writer does.
struct SampleConversion {
  int32_t dc_offset = 0;
  int bit_shift = 0;
  int lsb_drop = 0;

  bool operator==(const SampleConversion& other) const = default;
};

// The bits a conversion shifts up by and the low bits it drops, as used.
inline constexpr int BitShift(const SampleConversion& conversion) {
  return std::clamp(conversion.bit_shift, 0, kMaximumBitShift);
}

inline constexpr int LsbDrop(const SampleConversion& conversion) {
  return std::clamp(conversion.lsb_drop, 0, kMaximumLsbDrop);
}

// A centred value rounded to a step of 2^step_bits, half to even.
//
// Rounded rather than truncated, because truncation moves every sample the same
// way and shifts the whole signal by half a step. And half to even rather than
// half up, for the same reason in miniature: with one bit dropped every odd
// code is exactly half way, and rounding all of them up would shift the signal
// by half a code. Both only hold for a value that is already centred, which is
// why the DC offset comes out first.
inline constexpr int32_t RoundToStep(int32_t centred, int step_bits) {
  if (step_bits <= 0) {
    return centred;
  }
  const int32_t step = int32_t{1} << step_bits;
  const int32_t half = step / 2;

  // Floor division: the shift is arithmetic on a negative value, which C++20
  // guarantees.
  int32_t steps = centred >> step_bits;
  const int32_t remainder = centred - (steps * step);
  if (remainder > half || (remainder == half && (steps % 2) != 0)) {
    ++steps;
  }
  return steps * step;
}

// The highest sample a conversion writes: one output step below 32768.
//
// Not INT16_MAX. Every sample a conversion produces is a whole multiple of its
// step — 64 for the converter's ten bits, more once bits are dropped or the
// signal shifted — and FLAC stores the zero bits under that step for nothing. A
// saturated sample of 32767 would have none, and every block of the file that
// held one would pay for the full sixteen.
inline constexpr int32_t ConvertedTopSample(
    const SampleConversion& conversion) {
  return (kSampleZeroOffset * kSampleScale) -
         (kSampleScale << (LsbDrop(conversion) + BitShift(conversion)));
}

// The converted sample before it is saturated, in 32 bits.
//
// The order is fixed, and each step is where it is for a reason:
//
//  1. Centring. The DC offset comes out of the converter's own code, before
//     anything else has touched it, so that what follows is centred on the
//     signal's real zero — the rounding below is only unbiased about zero.
//  2. Bit shift. Shifted up in 32 bits, where nothing can overflow, so a sample
//     the shift takes past full scale is still known exactly at the end rather
//     than having been clipped half way through.
//  3. Dropping LSBs, last. Rounded to the step that drops lsb_drop of the
//     converter's bits at this shift — 2^(lsb_drop + bit_shift) in the
//     shifted units — so the shift is never mistaken for resolution.
//
// Saturating to sixteen bits is not a step of its own but the end of the
// line: ToConvertedSigned16Bit() does it once, to the finished value.
inline constexpr int32_t UnsaturatedConverted(
    int32_t ten_bit_value, const SampleConversion& conversion) {
  const int32_t centred =
      ten_bit_value - kSampleZeroOffset - conversion.dc_offset;
  const int32_t shifted = centred * (int32_t{1} << BitShift(conversion));
  const int32_t rounded =
      RoundToStep(shifted, LsbDrop(conversion) + BitShift(conversion));
  return rounded * kSampleScale;
}

// A converter code as a capture holds it, after `conversion`.
//
// Saturated, never cast, because a cast wraps: a sample just past the bottom
// would come out near the top, a full-scale spike in the other direction in
// the middle of the signal.
inline constexpr int16_t ToConvertedSigned16Bit(
    int32_t ten_bit_value, const SampleConversion& conversion) {
  const int32_t scaled = UnsaturatedConverted(ten_bit_value, conversion);
  if (scaled < INT16_MIN) {
    return INT16_MIN;
  }
  const int32_t top = ConvertedTopSample(conversion);
  if (scaled > top) {
    return static_cast<int16_t>(top);
  }
  return static_cast<int16_t>(scaled);
}

// The same, with a board's declared DC offset taken out and nothing else.
//
// The offset is in whole converter codes, so the result is still a code times
// 64 and its six low bits are still zero — which is what FLAC stores for
// nothing, and what a fractional correction would have made it store.
//
// The ten bits already fill the sixteen exactly, so shifting them by any
// offset at all pushes the codes nearest one end of the converter out of
// range. Those are saturated to the ends of the range the converter itself
// spans, -32768 and 32704. With an offset measured on the board it is
// correcting this cannot happen on its own — see DcOffsetSaturates().
inline constexpr int16_t ToCorrectedSigned16Bit(int32_t ten_bit_value,
                                                int32_t dc_offset) {
  return ToConvertedSigned16Bit(ten_bit_value, SampleConversion{dc_offset});
}

// Bytes per sample once converted to signed 16-bit. The same as on the wire,
// which is a coincidence worth naming rather than relying on: the wire word
// carries a 10-bit value in 16 bits, and the converted sample carries that
// value scaled into a signed 16-bit one.
inline constexpr size_t kSigned16BytesPerSample = 2;

// Convert wire words, sequence markers already stripped, to the signed 16-bit
// little-endian samples a raw capture holds, after `conversion`. What RawSink
// writes to a file and PipeWriter writes to standard output — and the same
// conversion FlacWriter applies — so that no two of them can disagree about a
// single sample.
//
// Byte by byte in and byte by byte out, so this is correct on a big-endian host
// and makes no alignment assumption about either buffer. `out` must hold
// kSigned16BytesPerSample * sample_count bytes.
inline void WireToSigned16LittleEndian(const uint8_t* wire_data,
                                       size_t sample_count,
                                       const SampleConversion& conversion,
                                       uint8_t* out) {
  for (size_t index = 0; index < sample_count; ++index) {
    const uint8_t* const in = wire_data + (index * kBytesPerSample);
    const auto ten_bit_value = static_cast<uint16_t>(
        static_cast<uint16_t>(in[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(in[1]) << 8));
    const auto sample = static_cast<uint16_t>(ToConvertedSigned16Bit(
        static_cast<int32_t>(ten_bit_value), conversion));
    uint8_t* const written = out + (index * kSigned16BytesPerSample);
    written[0] = static_cast<uint8_t>(sample);
    written[1] = static_cast<uint8_t>(sample >> 8);
  }
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

// Whether a sample that neither the converter nor the DC offset clipped is
// taken out of range by the bit shift — the shift clipping it.
//
// Counted apart from DcOffsetSaturates() because it says something different
// and asks for something different: a saturating offset says the declaration
// belongs to another board, and a clipping shift says the shift is too large
// for this signal. A sample the offset already lost is the offset's, so that
// each is counted once and by the cause that acted first.
inline constexpr bool BitShiftClips(uint16_t ten_bit_value,
                                    const SampleConversion& conversion) {
  if (BitShift(conversion) == 0) {
    return false;
  }
  const auto value = static_cast<int32_t>(ten_bit_value);
  if (value <= kMinimumSampleValue || value >= kMaximumSampleValue ||
      DcOffsetSaturates(ten_bit_value, conversion.dc_offset)) {
    return false;
  }
  const int32_t scaled = UnsaturatedConverted(value, conversion);
  return scaled < INT16_MIN || scaled > ConvertedTopSample(conversion);
}

// The converted sample back in the converter's 0..1023 domain, for a display
// that draws codes: what a capture holds, in the units a scope is drawn in.
inline constexpr int32_t ConvertedTenBitCode(
    int32_t ten_bit_value, const SampleConversion& conversion) {
  return (ToConvertedSigned16Bit(ten_bit_value, conversion) / kSampleScale) +
         kSampleZeroOffset;
}

// The inverse, for reading a capture back into the 10-bit domain the device's
// test pattern counts in.
inline constexpr int32_t ToTenBit(int16_t signed_value) {
  return (signed_value / kSampleScale) + kSampleZeroOffset;
}

}  // namespace ddd::capture
