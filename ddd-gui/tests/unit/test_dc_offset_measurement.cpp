/************************************************************************

    test_dc_offset_measurement.cpp

    T1 tests for measuring a board's DC offset
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <gtest/gtest.h>

#include <cstdint>

#include "board_setup.h"
#include "dc_offset_measurement.h"

namespace ddd::capture {
namespace {

// The window is the difference between two readings, so whatever the run did
// before the first one — settling, a different range — has no say in it.
TEST(DcOffsetMeasurementTest, TheMeanIsTakenOverTheWindowOnly) {
  const DcOffsetReading start{1'000'000, -5'000'000};
  const DcOffsetReading end{41'000'000, -5'000'000 + (40'000'000LL * 12)};

  const DcOffsetResult result = ComputeDcOffset(start, end, 505, 530);
  ASSERT_TRUE(result.valid) << result.problem;
  EXPECT_DOUBLE_EQ(result.mean, 12.0);
  EXPECT_EQ(result.offset, 12);
}

TEST(DcOffsetMeasurementTest, TheOffsetIsRoundedToAWholeCode) {
  const DcOffsetReading start{0, 0};

  EXPECT_EQ(ComputeDcOffset(start, {1000, 7400}, 500, 520).offset, 7);
  EXPECT_EQ(ComputeDcOffset(start, {1000, 7600}, 500, 520).offset, 8);
  EXPECT_EQ(ComputeDcOffset(start, {1000, -7600}, 500, 520).offset, -8);
}

TEST(DcOffsetMeasurementTest, TheOffsetIsClampedToWhatTheRecordHolds) {
  const DcOffsetReading start{0, 0};
  const DcOffsetResult low = ComputeDcOffset(start, {10, -5120}, 0, 0);
  EXPECT_EQ(low.offset, kDcOffsetMinimum);
}

TEST(DcOffsetMeasurementTest, NoSamplesIsNoMeasurement) {
  const DcOffsetReading reading{5000, 100};
  const DcOffsetResult result = ComputeDcOffset(reading, reading, 510, 514);
  EXPECT_FALSE(result.valid);
  EXPECT_FALSE(result.problem.empty());
}

// The BNC still connected: a player's RF covers hundreds of codes, and the
// mean of it would be declared on the board as if it were the board's own.
TEST(DcOffsetMeasurementTest, ASignalOnTheInputIsRefused) {
  const DcOffsetReading start{0, 0};
  const DcOffsetResult result = ComputeDcOffset(start, {1000, 3000}, 200, 820);
  EXPECT_FALSE(result.valid);
  EXPECT_NE(result.problem.find("BNC"), std::string::npos);
}

TEST(DcOffsetMeasurementTest, QuietNoiseIsAccepted) {
  const DcOffsetReading start{0, 0};
  EXPECT_TRUE(
      ComputeDcOffset(start, {1000, 3000}, 480, 480 + kDcOffsetQuietSpanCodes)
          .valid);
  EXPECT_FALSE(ComputeDcOffset(start, {1000, 3000}, 480,
                               480 + kDcOffsetQuietSpanCodes + 1)
                   .valid);
}

}  // namespace
}  // namespace ddd::capture
