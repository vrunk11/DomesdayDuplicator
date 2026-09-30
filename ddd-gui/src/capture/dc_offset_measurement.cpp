/************************************************************************

    dc_offset_measurement.cpp

    Measuring a board's DC offset from a stretch of the stream with nothing
    connected to its input
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "dc_offset_measurement.h"

#include <algorithm>
#include <cmath>

#include "board_setup.h"

namespace ddd::capture {

DcOffsetResult ComputeDcOffset(const DcOffsetReading& start,
                               const DcOffsetReading& end, uint16_t minimum,
                               uint16_t maximum) {
  DcOffsetResult result;

  if (end.sample_count <= start.sample_count) {
    result.problem =
        "No samples arrived while measuring, so there is nothing to average.";
    return result;
  }

  if (maximum < minimum ||
      static_cast<int>(maximum) - static_cast<int>(minimum) >
          kDcOffsetQuietSpanCodes) {
    result.problem =
        "The input is not quiet: the signal covered " +
        std::to_string(static_cast<int>(maximum) - static_cast<int>(minimum)) +
        " codes while measuring. Disconnect the BNC input and measure again.";
    return result;
  }

  const auto samples =
      static_cast<double>(end.sample_count - start.sample_count);
  result.mean = static_cast<double>(end.sum - start.sum) / samples;
  result.offset = std::clamp(static_cast<int>(std::lround(result.mean)),
                             kDcOffsetMinimum, kDcOffsetMaximum);
  result.valid = true;
  return result;
}

}  // namespace ddd::capture
