/************************************************************************

    test_capture_format.cpp

    T1 tests for the ADC rate and input range helpers of the capture format
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "capture_format.h"
#include "wire_protocol.h"

namespace ddd::capture {
namespace {

// A board's maximum is a preset on every build this project makes, and the
// rate it comes up at is that preset itself.
TEST(CaptureFormatTest, ABoardComesUpAtItsOwnMaximum) {
  EXPECT_EQ(HighestPllPresetAtMost(75), kPllPreset75Mhz);
  EXPECT_EQ(HighestPllPresetAtMost(40), kPllPreset40Mhz);
}

// A maximum between two presets is rounded down, never up: running a
// converter faster than it is rated for produces garbage, where running it
// slower only costs bandwidth.
TEST(CaptureFormatTest, AMaximumBetweenPresetsIsRoundedDown) {
  EXPECT_EQ(HighestPllPresetAtMost(62), kPllPreset60Mhz);
  EXPECT_EQ(HighestPllPresetAtMost(255), kPllPreset75Mhz);
}

// 0 is MAX_ADC_RATE_MHZ's "not known" reading, and a board slower than every
// preset has nothing this application can name. Neither may be turned into a
// rate.
TEST(CaptureFormatTest, NoRateIsInventedWhereNoPresetFits) {
  EXPECT_EQ(HighestPllPresetAtMost(0), 0);
  EXPECT_EQ(HighestPllPresetAtMost(39), 0);
}

TEST(CaptureFormatTest, EveryRateItReturnsIsOneTheGatewareImplements) {
  for (int max = 0; max <= 255; ++max) {
    const uint8_t rate = HighestPllPresetAtMost(static_cast<uint8_t>(max));
    if (rate != 0) {
      EXPECT_TRUE(IsSupportedPllPreset(rate)) << "max " << max;
      EXPECT_LE(rate, max);
    }
  }
}

TEST(CaptureFormatTest, TheInputRangeIsSpelledAsASourceLevelIsSpecified) {
  EXPECT_EQ(std::string(InputRangeName(true)), "2Vpp");
  EXPECT_EQ(std::string(InputRangeName(false)), "1Vpp");
}

}  // namespace
}  // namespace ddd::capture
