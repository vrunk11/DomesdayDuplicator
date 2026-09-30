/************************************************************************

    dc_offset_measurement.h

    Measuring a board's DC offset from a stretch of the stream with nothing
    connected to its input
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <cstdint>
#include <string>

namespace ddd::capture {

// How long a measurement averages over. A second is tens of millions of
// samples at any rate the board runs at, so the converter's own noise averages
// away completely, and it is a whole number of cycles of 50 Hz and 60 Hz mains
// both, so hum picked up by an open input does too.
inline constexpr int kDcOffsetAveragingMilliseconds = 1000;

// How long to let the converter settle before averaging: the input range has
// just been selected and the stream has just started. Generous, because it
// costs a quarter of a second once and a measurement taken during a settling
// reference would be declared on the board for good.
inline constexpr int kDcOffsetSettlingMilliseconds = 250;

// The widest span, in converter codes, a stream can cover and still be taken
// for an input with nothing connected to it. The converter's own noise covers
// a few codes, twice that at 1Vpp; a LaserDisc player's RF covers hundreds.
// Sixty-four codes is six percent of the range — far above any quiet input and
// far below any signal — so a measurement made with the BNC still connected
// is refused rather than declared.
inline constexpr int kDcOffsetQuietSpanCodes = 64;

// One reading of the running metrics: the sample count and the sum of
// (value - 512) since the run began. Two of them bracket the averaging window.
struct DcOffsetReading {
  uint64_t sample_count = 0;
  int64_t sum = 0;
};

struct DcOffsetResult {
  bool valid = false;

  // The mean over the window, in converter codes above 512.
  double mean = 0.0;

  // The mean rounded to a whole code and clamped to what the board setup
  // record can hold — the figure to declare. Whole codes, because the samples
  // it corrects are written as a code times 64: see board_setup.h.
  int offset = 0;

  // Why not, when invalid; empty when valid. A sentence for the user.
  std::string problem;
};

// The DC offset over the window between two readings.
//
// `minimum` and `maximum` are the extremes seen during the window, in
// converter codes. Their span is what tells a quiet input from a connected one
// — the mean of a symmetric RF signal would be a perfectly good DC offset, but
// the point of measuring with nothing connected is that nothing but the board
// contributes to it, and a player that is not playing still puts its own
// output stage on the line.
DcOffsetResult ComputeDcOffset(const DcOffsetReading& start,
                               const DcOffsetReading& end, uint16_t minimum,
                               uint16_t maximum);

}  // namespace ddd::capture
