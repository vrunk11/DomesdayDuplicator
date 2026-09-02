/************************************************************************

    test_sample_format.cpp

    T1 tests for the device's sample layout and the capture file naming
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <gtest/gtest.h>

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

}  // namespace
}  // namespace ddd::capture
