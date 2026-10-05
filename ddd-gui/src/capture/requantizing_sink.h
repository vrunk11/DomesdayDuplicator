/************************************************************************

    requantizing_sink.h

    The requantiser in front of a writer, on a thread of its own
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rf_requantizer.h"
#include "sample_format.h"
#include "sample_sink.h"

namespace ddd::capture {

// What the requantiser has done, for whoever started it: the decision in
// force and the floor it was made against, while the stream runs; and the
// record of every change, once it has finished, for the capture's metadata.
//
// Written by the requantiser's thread once a segment — about thirty times a
// second — and read by anybody, under a lock neither side holds for longer
// than a copy.
class RequantizationStatus {
 public:
  using Change = RequantizationChange;

  struct Summary {
    RequantizerDecision current;
    double noise_floor_lsb = 0.0;
    double carrier_to_noise_db = 0.0;
    uint64_t segments = 0;
    uint64_t samples = 0;

    // Only the changes: the first segment's decision, and every one after it
    // that differs from the one before.
    std::vector<Change> changes;

    // Samples by bits dropped, index 0 for none.
    std::vector<uint64_t> samples_by_drop;
    uint64_t shaped_samples = 0;
    double worst_degradation_db = 0.0;
    uint64_t clipped = 0;
  };

  Summary Read() const;

  // What is in force now, for a display that updates several times a second
  // and has no use for the record.
  struct Live {
    RequantizerDecision current;
    double noise_floor_lsb = 0.0;
    double carrier_to_noise_db = 0.0;
    uint64_t segments = 0;
  };

  Live ReadLive() const;

  // The requantiser's side.
  void Record(const RequantizerDecision& decision, uint64_t first_sample,
              size_t count, double noise_floor_lsb, double carrier_to_noise_db,
              uint64_t clipped);

 private:
  mutable std::mutex mutex_;
  Summary summary_;
};

// A writer that requantises everything it is given before handing it on.
//
// Wire words come in on the processing thread, are converted to signed 16-bit
// with the capture's DC offset and bit shift, and are gathered into segments of
// RfRequantizer::kSegmentSamples. Each full segment goes to a thread of this
// sink's own, which decides what it keeps, requantises it and gives it to the
// wrapped sink — so the cost of the decision and of the noise shaping is never
// paid on the processing thread, where every buffer has a deadline.
//
// The queue between them is short. A requantiser that cannot keep up makes
// Write() wait for room, which the ring absorbs as it absorbs a slow disk; one
// that persistently cannot is a capture that fails, as it should.
//
// In front of a sink that stores nothing — monitoring — the decisions are
// made and published but nothing is requantised: a preview of what a capture
// would do, for the cost of the analysis alone.
class RequantizingSink : public ISampleSink {
 public:
  RequantizingSink(std::unique_ptr<ISampleSink> inner,
                   const SampleConversion& conversion,
                   const RequantizerSettings& settings,
                   std::shared_ptr<RequantizationStatus> status);
  ~RequantizingSink() override;

  // Segments waiting for the requantiser before Write() has to wait. Four is
  // about a seventh of a second at 30 Msps.
  static constexpr size_t kQueueSegments = 4;

  const char* Name() const override { return name_.c_str(); }
  bool StoresData() const override { return inner_->StoresData(); }

  bool Write(const uint8_t* wire_data, size_t sample_count) override;

  // Requantises what is left — a last, shorter segment — and waits for all of
  // it to reach the wrapped sink before finishing that.
  bool Finish() override;

  uint64_t BytesWritten() const override { return inner_->BytesWritten(); }
  uint64_t SamplesWritten() const override { return inner_->SamplesWritten(); }
  uint64_t SamplesPending() const override;
  const std::string& LastError() const override { return last_error_; }

 private:
  void Submit();
  void Run();

  std::unique_ptr<ISampleSink> inner_;
  SampleConversion conversion_;
  RfRequantizer requantizer_;
  std::shared_ptr<RequantizationStatus> status_;
  std::string name_;
  std::string last_error_;

  // The segment being gathered, on the processing thread.
  std::vector<int16_t> gathering_;
  size_t gathered_ = 0;

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<std::vector<int16_t>> queue_;
  std::vector<std::vector<int16_t>> spare_;
  bool ending_ = false;
  std::atomic<uint64_t> queued_samples_{0};

  // Set by the requantiser's thread when the wrapped sink refused a segment;
  // the next Write() reports it.
  std::atomic<bool> failed_{false};
  std::string failure_;

  bool finished_ = false;
  std::thread thread_;
};

}  // namespace ddd::capture
