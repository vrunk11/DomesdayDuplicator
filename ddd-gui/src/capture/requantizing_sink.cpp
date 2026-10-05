/************************************************************************

    requantizing_sink.cpp

    The requantiser in front of a writer, on a thread of its own
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "requantizing_sink.h"

#include <algorithm>
#include <utility>

namespace ddd::capture {

// --- RequantizationStatus ------------------------------------------------

RequantizationStatus::Summary RequantizationStatus::Read() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return summary_;
}

RequantizationStatus::Live RequantizationStatus::ReadLive() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return {summary_.current, summary_.noise_floor_lsb,
          summary_.carrier_to_noise_db, summary_.segments};
}

void RequantizationStatus::Record(const RequantizerDecision& decision,
                                  uint64_t first_sample, size_t count,
                                  double noise_floor_lsb,
                                  double carrier_to_noise_db,
                                  uint64_t clipped) {
  const std::lock_guard<std::mutex> lock(mutex_);
  Summary& summary = summary_;

  if (summary.segments == 0 || summary.current.lsb_drop != decision.lsb_drop ||
      summary.current.shaped != decision.shaped) {
    summary.changes.push_back(
        {first_sample, decision.lsb_drop, decision.shaped});
  }

  summary.current = decision;
  summary.noise_floor_lsb = noise_floor_lsb;
  summary.carrier_to_noise_db = carrier_to_noise_db;
  ++summary.segments;
  summary.samples += count;

  const auto drop = static_cast<size_t>(std::max(decision.lsb_drop, 0));
  if (summary.samples_by_drop.size() <= drop) {
    summary.samples_by_drop.resize(drop + 1, 0);
  }
  summary.samples_by_drop[drop] += count;
  if (decision.shaped) {
    summary.shaped_samples += count;
  }
  summary.worst_degradation_db =
      std::max(summary.worst_degradation_db, decision.degradation_db);
  summary.clipped = clipped;
}

// --- RequantizingSink ----------------------------------------------------

RequantizingSink::RequantizingSink(std::unique_ptr<ISampleSink> inner,
                                   const SampleConversion& conversion,
                                   const RequantizerSettings& settings,
                                   std::shared_ptr<RequantizationStatus> status)
    : inner_(std::move(inner)),
      conversion_(conversion),
      requantizer_(settings),
      status_(status != nullptr ? std::move(status)
                                : std::make_shared<RequantizationStatus>()),
      name_(std::string(inner_->Name()) + "+requantised"),
      gathering_(RfRequantizer::kSegmentSamples) {
  thread_ = std::thread(&RequantizingSink::Run, this);
}

RequantizingSink::~RequantizingSink() { Finish(); }

bool RequantizingSink::Write(const uint8_t* wire_data, size_t sample_count) {
  size_t done = 0;
  while (done < sample_count) {
    if (failed_.load()) {
      const std::lock_guard<std::mutex> lock(mutex_);
      last_error_ = failure_;
      return false;
    }

    // Converted with the capture's DC offset and bit shift on the way in, so
    // that what is requantised is what the file would otherwise have held.
    const size_t take =
        std::min(sample_count - done, gathering_.size() - gathered_);
    const uint8_t* const source = wire_data + (done * kBytesPerSample);
    for (size_t index = 0; index < take; ++index) {
      const auto ten_bit_value = static_cast<uint16_t>(
          static_cast<uint16_t>(source[index * kBytesPerSample]) |
          static_cast<uint16_t>(
              static_cast<uint16_t>(source[(index * kBytesPerSample) + 1])
              << 8));
      gathering_[gathered_ + index] = ToConvertedSigned16Bit(
          static_cast<int32_t>(ten_bit_value), conversion_);
    }
    gathered_ += take;
    done += take;

    if (gathered_ == gathering_.size()) {
      Submit();
    }
  }

  if (failed_.load()) {
    const std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = failure_;
    return false;
  }
  return true;
}

void RequantizingSink::Submit() {
  std::unique_lock<std::mutex> lock(mutex_);
  changed_.wait(lock, [this] {
    return queue_.size() < kQueueSegments || failed_.load();
  });
  if (failed_.load()) {
    gathered_ = 0;
    return;
  }

  std::vector<int16_t> segment = std::move(gathering_);
  segment.resize(gathered_);
  queued_samples_ += gathered_;
  queue_.push_back(std::move(segment));

  // A spare buffer from a segment already written, so that a capture
  // allocates its few segments once rather than thirty times a second.
  if (spare_.empty()) {
    gathering_.assign(RfRequantizer::kSegmentSamples, 0);
  } else {
    gathering_ = std::move(spare_.back());
    spare_.pop_back();
    gathering_.resize(RfRequantizer::kSegmentSamples);
  }
  gathered_ = 0;
  changed_.notify_all();
}

void RequantizingSink::Run() {
  uint64_t position = 0;
  for (;;) {
    std::vector<int16_t> segment;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this] { return !queue_.empty() || ending_; });
      if (queue_.empty()) {
        break;
      }
      segment = std::move(queue_.front());
      queue_.pop_front();
    }

    // Requantised only where it is going to be kept: a preview decides and
    // publishes, and leaves the samples it would have changed alone.
    const RequantizerDecision decision = requantizer_.Process(
        segment.data(), segment.size(), inner_->StoresData());
    status_->Record(decision, position, segment.size(),
                    requantizer_.noise_floor_lsb(),
                    requantizer_.carrier_to_noise_db(), requantizer_.clipped());
    position += segment.size();

    if (!failed_.load() &&
        !inner_->WriteConverted(segment.data(), segment.size())) {
      const std::lock_guard<std::mutex> lock(mutex_);
      failure_ = inner_->LastError().empty()
                     ? std::string("RequantizingSink: ") + inner_->Name() +
                           " does not take requantised samples"
                     : inner_->LastError();
      failed_.store(true);
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    queued_samples_ -= segment.size();
    spare_.push_back(std::move(segment));
    changed_.notify_all();
  }
}

bool RequantizingSink::Finish() {
  if (finished_) {
    return !failed_.load();
  }
  finished_ = true;

  // The last segment is whatever the stream ended with, however short.
  if (gathered_ > 0) {
    Submit();
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    ending_ = true;
    changed_.notify_all();
  }
  if (thread_.joinable()) {
    thread_.join();
  }

  bool succeeded = true;
  if (failed_.load()) {
    last_error_ = failure_;
    succeeded = false;
  }
  if (!inner_->Finish()) {
    last_error_ = inner_->LastError();
    succeeded = false;
  }
  return succeeded;
}

uint64_t RequantizingSink::SamplesPending() const {
  return queued_samples_.load() + gathered_ + inner_->SamplesPending();
}

}  // namespace ddd::capture
