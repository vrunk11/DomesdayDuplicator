/************************************************************************

    test_sample_format.cpp

    T1 tests for the device's sample layout and the capture file naming
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <gtest/gtest.h>

#include <vector>

#include "capture_format.h"
#include "sample_format.h"
#include "wire_protocol.h"

namespace ddd::capture {
namespace {

TEST(SampleFormatTest, TheWireRateIsEightyMegabytesPerSecond) {
  // The number the whole design is built around. If this ever changes, every
  // buffer-sizing decision in the engine has to be revisited, so it is asserted
  // rather than left as an assumption in a comment.
  EXPECT_EQ(kWireBytesPerSecond, 80'000'000U);
}

TEST(SampleFormatTest, AWordSplitsIntoASampleAndACounter) {
  const uint16_t word = MakeWireWord(0x2AB, 37);

  EXPECT_EQ(SampleValueFromWord(word), 0x2AB);
  EXPECT_EQ(SequenceCounterFromWord(word), 37);
}

TEST(SampleFormatTest, TheCounterCannotReachIntoTheSampleValue) {
  // The two fields share a 16-bit word, so the check that matters is that the
  // largest legal value of each leaves the other alone.
  const uint16_t word = MakeWireWord(kMaximumSampleValue, 62);

  EXPECT_EQ(SampleValueFromWord(word), kMaximumSampleValue);
  EXPECT_EQ(SequenceCounterFromWord(word), 62);
}

TEST(SampleFormatTest, TheHighByteConstantsAgreeWithTheWordConstants) {
  // The hot loop reads bytes rather than assembling words, so it uses a second
  // set of shifts and masks. They have to describe the same layout, and this is
  // what stops the two drifting apart.
  for (uint16_t value = 0; value <= kMaximumSampleValue; value += 7) {
    for (uint8_t counter = 0; counter < kSequenceCounterValues; counter += 5) {
      const uint16_t word = MakeWireWord(value, counter);
      const uint8_t high_byte = static_cast<uint8_t>(word >> 8);

      EXPECT_EQ(high_byte >> kSequenceCounterHighByteShift, counter);
      EXPECT_EQ(
          static_cast<uint16_t>(
              (word & 0xFF) | static_cast<uint16_t>(
                                  (high_byte & kSampleValueHighByteMask) << 8)),
          value);
    }
  }
}

TEST(SampleFormatTest, NoWholeNumberOfUsbPacketsIsAWholeCounterPeriod) {
  // The counter proves a capture is bit-perfect by predicting itself, so the
  // one hole it cannot see is a hole of exactly a whole number of periods: the
  // stream comes back in the phase it would have been in anyway.
  //
  // USB 3 loses capture data a whole endpoint packet at a time, and every size
  // a transfer can be — the 1,024-byte packet, the FX3's 16 KiB DMA buffer, the
  // host's 2 MiB slot — is a power of two. An odd period therefore shares no
  // factor with any of them, and this is that statement in one line.
  constexpr uint64_t kPeriodSamples =
      uint64_t{kSequenceCounterValues} * kSamplesPerSequenceCounter;
  EXPECT_EQ(kPeriodSamples % 2, 1U) << "the counter period must be odd";

  // A SuperSpeed bulk packet is 1,024 bytes: 512 samples, and the smallest
  // unit any of this is lost in. The first loss that is both a whole number of
  // them and a whole number of periods is therefore 512 periods, which is 52
  // seconds of capture — a hole every other thing the engine counts would have
  // noticed long before.
  constexpr uint64_t kPacketSamples = 512;
  constexpr uint64_t kSmallestBlindLossBytes =
      kPacketSamples * kPeriodSamples * kBytesPerSample;
  EXPECT_GT(kSmallestBlindLossBytes / kWireBytesPerSecond, 50U);

  // And the reason this is asserted rather than assumed. Gateware built before
  // issue #186 used a block of 65,536, which made the period 8,064 whole
  // packets: a 7.875 MiB hole, a tenth of a second, read as no hole at all.
  constexpr uint64_t kLegacyPeriodSamples =
      uint64_t{kSequenceCounterValues} * kLegacySamplesPerSequenceCounter;
  EXPECT_EQ(kLegacyPeriodSamples % kPacketSamples, 0U);
}

TEST(SampleFormatTest, BothShippedBlockLengthsAreRecognisedAndNothingElseIs) {
  // The validator learns the block length from the stream rather than being
  // told it, and this is the list it learns from.
  EXPECT_TRUE(IsKnownSamplesPerSequenceCounter(kSamplesPerSequenceCounter));
  EXPECT_TRUE(
      IsKnownSamplesPerSequenceCounter(kLegacySamplesPerSequenceCounter));
  EXPECT_FALSE(IsKnownSamplesPerSequenceCounter(0));
  EXPECT_FALSE(
      IsKnownSamplesPerSequenceCounter(kSamplesPerSequenceCounter - 1));
  EXPECT_FALSE(
      IsKnownSamplesPerSequenceCounter(kLegacySamplesPerSequenceCounter + 1));

  // The search for the first counter change has to span the longer of them, or
  // a legacy stream whose buffer opened just after a boundary would be called
  // markerless.
  EXPECT_GE(kMaximumSamplesPerSequenceCounter, kSamplesPerSequenceCounter);
  EXPECT_GE(kMaximumSamplesPerSequenceCounter,
            kLegacySamplesPerSequenceCounter);
}

TEST(SampleFormatTest, TheScalingIsTheOneLdDecodeExpects) {
  // ld-decode's lds.py calls (value - 512) * 64 "the DdD 16-bit format". These
  // three points pin it: the bottom of the range, the midpoint, and the top.
  EXPECT_EQ(ToSigned16Bit(0), -32768);
  EXPECT_EQ(ToSigned16Bit(512), 0);
  EXPECT_EQ(ToSigned16Bit(1023), 32704);
}

TEST(SampleFormatTest, ScalingRoundTripsThroughEveryTenBitValue) {
  for (int32_t value = 0; value <= kMaximumSampleValue; ++value) {
    EXPECT_EQ(ToTenBit(ToSigned16Bit(value)), value) << "value " << value;
  }
}

// With no offset declared, the corrected conversion is the ordinary one for
// every code — a board nobody has measured writes exactly what it always did.
TEST(SampleFormatTest, AZeroOffsetCorrectsNothing) {
  for (int32_t value = 0; value <= kMaximumSampleValue; ++value) {
    EXPECT_EQ(ToCorrectedSigned16Bit(value, 0), ToSigned16Bit(value))
        << "value " << value;
    EXPECT_FALSE(DcOffsetSaturates(static_cast<uint16_t>(value), 0))
        << "value " << value;
  }
}

// A signal sitting 20 codes high is moved down by 20 codes: its mean lands on
// zero, and the result is still a whole code times 64.
TEST(SampleFormatTest, TheOffsetIsTakenOutInWholeCodes) {
  EXPECT_EQ(ToCorrectedSigned16Bit(532, 20), 0);
  EXPECT_EQ(ToCorrectedSigned16Bit(492, -20), 0);
  EXPECT_EQ(ToCorrectedSigned16Bit(600, 20), (600 - 512 - 20) * 64);
  for (int32_t value = 0; value <= kMaximumSampleValue; ++value) {
    EXPECT_EQ(ToCorrectedSigned16Bit(value, 7) % 64, 0) << "value " << value;
  }
}

// The codes pushed past either end of the 16-bit range are held at that end.
// A cast would wrap them to the far end instead: a full-scale spike the wrong
// way in the middle of the signal.
TEST(SampleFormatTest, ACodePushedOutOfRangeSaturatesAndNeverWraps) {
  EXPECT_EQ(ToCorrectedSigned16Bit(0, 3), INT16_MIN);
  EXPECT_EQ(ToCorrectedSigned16Bit(2, 3), INT16_MIN);
  EXPECT_EQ(ToCorrectedSigned16Bit(3, 3), INT16_MIN);
  EXPECT_EQ(ToCorrectedSigned16Bit(4, 3), INT16_MIN + 64);

  EXPECT_EQ(ToCorrectedSigned16Bit(1023, -3), 511 * 64);
  EXPECT_EQ(ToCorrectedSigned16Bit(1021, -3), 511 * 64);
  EXPECT_EQ(ToCorrectedSigned16Bit(1020, -3), 511 * 64);

  // Monotonic across the whole range, for both signs: nothing folds back.
  for (const int32_t offset : {-40, -1, 1, 40}) {
    for (int32_t value = 1; value <= kMaximumSampleValue; ++value) {
      EXPECT_GE(ToCorrectedSigned16Bit(value, offset),
                ToCorrectedSigned16Bit(value - 1, offset))
          << "offset " << offset << " value " << value;
    }
  }
}

// Only the codes the correction lost count, never the ones the converter
// clipped — those are already counted as clipping, and counting them twice
// would blame the declaration for a signal that is simply too hot.
TEST(SampleFormatTest, OnlyTheCodesTheCorrectionLostAreCounted) {
  EXPECT_FALSE(DcOffsetSaturates(0, 3));
  EXPECT_TRUE(DcOffsetSaturates(1, 3));
  EXPECT_TRUE(DcOffsetSaturates(2, 3));
  EXPECT_FALSE(DcOffsetSaturates(3, 3));
  EXPECT_FALSE(DcOffsetSaturates(1022, 3));

  EXPECT_FALSE(DcOffsetSaturates(1023, -3));
  EXPECT_TRUE(DcOffsetSaturates(1022, -3));
  EXPECT_TRUE(DcOffsetSaturates(1021, -3));
  EXPECT_FALSE(DcOffsetSaturates(1020, -3));
  EXPECT_FALSE(DcOffsetSaturates(1, -3));
}

// The count and the conversion agree: a code is counted exactly when the
// conversion had to hold it at an end it would not otherwise have reached.
TEST(SampleFormatTest, TheCountMatchesWhatTheConversionSaturated) {
  for (const int32_t offset : {-100, -3, 3, 100}) {
    for (int32_t value = 1; value < kMaximumSampleValue; ++value) {
      const int32_t exact = (value - kSampleZeroOffset - offset) * kSampleScale;
      const bool held = exact != ToCorrectedSigned16Bit(value, offset);
      EXPECT_EQ(DcOffsetSaturates(static_cast<uint16_t>(value), offset), held)
          << "offset " << offset << " value " << value;
    }
  }
}

TEST(CaptureFormatTest, TheDefaultSuffixSaysWhereTheSamplesCameFrom) {
  EXPECT_EQ(AddCaptureFileSuffix("disc1").string(), "disc1.ddd.flac");
}

TEST(CaptureFormatTest, AddingTheSuffixTwiceDoesNothingTheSecondTime) {
  // The name arrives from a text field a user can type into, so this is a
  // condition that reaches the code rather than a hypothetical one.
  const std::filesystem::path once = AddCaptureFileSuffix("disc1");
  EXPECT_EQ(AddCaptureFileSuffix(once).string(), "disc1.ddd.flac");
}

TEST(CaptureFormatTest, ExtensionsAreComparedWithoutRegardToCase) {
  EXPECT_EQ(LowerCaseExtension("capture.FLAC"), "flac");
  EXPECT_EQ(LowerCaseExtension("capture.Raw"), "raw");
  EXPECT_EQ(LowerCaseExtension("capture"), "");
}

TEST(WireProtocolTest, ARegisterWriteCarriesTheAddressAndTheValue) {
  // The address is the high byte and the value the low one, which is what
  // keeps a register write to a setup packet with no data stage.
  EXPECT_EQ(MakeRegisterWrite(0x10, 0x01), 0x1001);
  EXPECT_EQ(MakeRegisterWrite(0x00, 0xFF), 0x00FF);
  EXPECT_EQ(MakeRegisterWrite(0x7F, 0x00), 0x7F00);
}

TEST(WireProtocolTest, TestModeIsAWriteToItsOwnRegister) {
  EXPECT_EQ(MakeTestModeWrite(true), MakeRegisterWrite(kRegisterTestMode, 1));
  EXPECT_EQ(MakeTestModeWrite(false), MakeRegisterWrite(kRegisterTestMode, 0));
}

TEST(WireProtocolTest, TheIdentitySignatureIsNeitherAllZerosNorAllOnes) {
  // The whole value of the signature is that it tells a real register bank
  // from a floating wire. SPI has no acknowledgement, so an absent or
  // unconfigured FPGA returns whatever MISO carries — which is one of these
  // two — and a signature equal to either would be no check at all.
  EXPECT_NE(kIdentityValue, 0x00);
  EXPECT_NE(kIdentityValue, 0xFF);
}

TEST(WireProtocolTest, TheIdentifiersAreTheAssignedOnes) {
  // pid.codes allocated these. A wrong value here means the application does
  // not find the device at all, which is worth one assertion.
  EXPECT_EQ(kVendorId, 0x1209);
  EXPECT_EQ(kProductId, 0x2347);
  EXPECT_EQ(kBulkInEndpoint, 0x81);
}

// --- The capture's conversion: offset, bit shift, then LSB drop ------------

// The conversions every combination test below walks: each shift and each
// drop, with and without an offset either way.
std::vector<SampleConversion> EveryConversion() {
  std::vector<SampleConversion> conversions;
  for (const int32_t offset : {-40, 0, 7}) {
    for (int shift = 0; shift <= kMaximumBitShift; ++shift) {
      for (int drop = 0; drop <= kMaximumLsbDrop; ++drop) {
        conversions.push_back(SampleConversion{offset, shift, drop});
      }
    }
  }
  return conversions;
}

TEST(SampleConversionTest, TheDefaultIsTheConverterUntouched) {
  for (int32_t value = 0; value <= kMaximumSampleValue; ++value) {
    EXPECT_EQ(ToConvertedSigned16Bit(value, SampleConversion{}),
              ToSigned16Bit(value))
        << value;
  }
}

// Every sample is a whole multiple of the conversion's output step, the
// saturated ones included — which is what lets FLAC store the zero bits under
// it for nothing.
TEST(SampleConversionTest, EverySampleIsAWholeMultipleOfTheStep) {
  for (const SampleConversion& conversion : EveryConversion()) {
    const int32_t step = kSampleScale
                         << (LsbDrop(conversion) + BitShift(conversion));
    for (int32_t value = 0; value <= kMaximumSampleValue; ++value) {
      EXPECT_EQ(ToConvertedSigned16Bit(value, conversion) % step, 0)
          << "value " << value << " shift " << conversion.bit_shift << " drop "
          << conversion.lsb_drop;
    }
  }
}

// The offset comes out first: converting a code with an offset is converting
// the code the offset would have made it with no offset at all. Rounding
// before the offset would round about the wrong zero.
TEST(SampleConversionTest, TheOffsetComesOutBeforeAnythingElse) {
  for (const SampleConversion& conversion : EveryConversion()) {
    SampleConversion without = conversion;
    without.dc_offset = 0;
    for (int32_t value = 50; value <= 970; ++value) {
      EXPECT_EQ(ToConvertedSigned16Bit(value, conversion),
                ToConvertedSigned16Bit(value - conversion.dc_offset, without))
          << "value " << value << " offset " << conversion.dc_offset;
    }
  }
}

// Rounded, not truncated, and half to even: over a stretch of whole steps the
// errors cancel, so dropping bits does not move the signal's DC level.
TEST(SampleConversionTest, DroppingBitsDoesNotShiftTheSignal) {
  for (int dropped = 1; dropped <= kMaximumLsbDrop; ++dropped) {
    int64_t error = 0;
    for (int32_t centred = -256; centred < 256; ++centred) {
      error += RoundToStep(centred, dropped) - centred;
    }
    EXPECT_EQ(error, 0) << "dropped " << dropped;
  }

  // The half-way cases, which truncation and rounding half up both get wrong.
  EXPECT_EQ(RoundToStep(1, 1), 0);
  EXPECT_EQ(RoundToStep(3, 1), 4);
  EXPECT_EQ(RoundToStep(-1, 1), 0);
  EXPECT_EQ(RoundToStep(-3, 1), -4);
  EXPECT_EQ(RoundToStep(5, 2), 4);
  EXPECT_EQ(RoundToStep(6, 2), 8);
}

// The shift multiplies; it does not count towards the bits dropped. Two bits
// of shift with two dropped keeps eight bits of the signal — the same samples
// as two dropped unshifted, four times larger — rather than ten bits with the
// shift's zeros dropped.
TEST(SampleConversionTest, TheBitShiftIsNeverMistakenForResolution) {
  const SampleConversion two_dropped{0, 0, 2};
  const SampleConversion two_dropped_shifted_two{0, 2, 2};
  for (int32_t value = 512 - 120; value <= 512 + 120; ++value) {
    EXPECT_EQ(ToConvertedSigned16Bit(value, two_dropped_shifted_two),
              4 * ToConvertedSigned16Bit(value, two_dropped))
        << value;
  }

  // And at full resolution a shift is exact.
  for (int32_t value = 512 - 255; value <= 512 + 254; ++value) {
    EXPECT_EQ(ToConvertedSigned16Bit(value, SampleConversion{0, 1, 0}),
              2 * ToSigned16Bit(value))
        << value;
  }
}

TEST(SampleConversionTest, WhatTheBitShiftTakesPastFullScaleSaturates) {
  const SampleConversion shift_one{0, 1, 0};
  EXPECT_EQ(ToConvertedSigned16Bit(767, shift_one), 510 * 64);
  EXPECT_EQ(ToConvertedSigned16Bit(768, shift_one), 510 * 64);
  EXPECT_EQ(ToConvertedSigned16Bit(1022, shift_one), 510 * 64);
  EXPECT_EQ(ToConvertedSigned16Bit(256, shift_one), INT16_MIN);
  EXPECT_EQ(ToConvertedSigned16Bit(1, shift_one), INT16_MIN);

  EXPECT_FALSE(BitShiftClips(767, shift_one));
  EXPECT_TRUE(BitShiftClips(768, shift_one));
  EXPECT_FALSE(BitShiftClips(256, shift_one));
  EXPECT_TRUE(BitShiftClips(255, shift_one));

  // What the converter clipped is the converter's, and with no shift nothing
  // is the shift's.
  EXPECT_FALSE(BitShiftClips(kMaximumSampleValue, shift_one));
  EXPECT_FALSE(BitShiftClips(kMinimumSampleValue, shift_one));
  EXPECT_FALSE(BitShiftClips(1022, SampleConversion{}));
}

// Out-of-range settings are held to what the writers can do rather than
// passed through.
TEST(SampleConversionTest, AnUnusableSettingIsHeldInRange) {
  EXPECT_EQ(LsbDrop(SampleConversion{0, 0, 6}), kMaximumLsbDrop);
  EXPECT_EQ(LsbDrop(SampleConversion{0, 0, -1}), 0);
  EXPECT_EQ(BitShift(SampleConversion{0, 9, 0}), kMaximumBitShift);
  EXPECT_EQ(BitShift(SampleConversion{0, -1, 0}), 0);
}

// Packed for another thread and back, every conversion comes out as it went
// in — a negative offset included — and the default is still told apart from
// "nothing set".
TEST(SampleConversionTest, APackedConversionUnpacksUnchanged) {
  for (const SampleConversion& conversion : EveryConversion()) {
    EXPECT_EQ(UnpackSampleConversion(PackSampleConversion(conversion)),
              conversion)
        << "offset " << conversion.dc_offset << " shift "
        << conversion.bit_shift << " drop " << conversion.lsb_drop;
  }
  EXPECT_NE(PackSampleConversion(SampleConversion{}), 0U);
}

TEST(SampleConversionTest, TheScopeSeesWhatTheFileHolds) {
  EXPECT_EQ(ConvertedTenBitCode(600, SampleConversion{}), 600);
  EXPECT_EQ(ConvertedTenBitCode(600, SampleConversion{0, 1, 0}), 688);
  EXPECT_EQ(ConvertedTenBitCode(600, SampleConversion{10, 0, 0}), 590);
  EXPECT_EQ(ConvertedTenBitCode(1022, SampleConversion{0, 1, 0}), 1022);
  EXPECT_EQ(ConvertedTenBitCode(1, SampleConversion{0, 1, 0}), 0);
}

}  // namespace
}  // namespace ddd::capture
