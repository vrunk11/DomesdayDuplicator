/************************************************************************

    analysis_worker.cpp

    Snapshot analysis, off the thread that has to stay responsive
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "analysis_worker.h"

#include <QTimer>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <utility>

#include "requantizing_sink.h"
#include "rf_requantizer.h"
#include "sample_format.h"

namespace ddd::gui {

void SnapshotAnalyser::SetSource(capture::SnapshotPublisher* snapshots) {
  const std::lock_guard<std::mutex> lock(source_mutex_);
  source_ = snapshots;

  // A new run is a new signal. Carrying the averaged spectrum and the peak hold
  // across would show the previous device's carrier for several seconds after
  // this one was attached.
  spectrum_.Reset();
}

void SnapshotAnalyser::SetSpectrumAveraging(double averaging) {
  requested_averaging_.store(averaging);
  options_changed_.store(true);
}

void SnapshotAnalyser::SetSpectrumTransformSize(size_t transform_size) {
  requested_transform_size_.store(transform_size);
  options_changed_.store(true);
}

void SnapshotAnalyser::RequestPeakHoldReset() {
  peak_hold_reset_requested_.store(true);
}

void SnapshotAnalyser::SetConversion(
    const capture::SampleConversion& conversion) {
  requested_conversion_.store(capture::PackSampleConversion(conversion));
}

void SnapshotAnalyser::SetCorrected(bool scope, bool spectrum) {
  scope_corrected_.store(scope);
  spectrum_corrected_.store(spectrum);
}

void SnapshotAnalyser::SetRequantization(
    std::shared_ptr<capture::RequantizationStatus> status,
    const capture::RequantizerSettings& settings) {
  const std::lock_guard<std::mutex> lock(requantization_mutex_);
  requested_requantization_ = std::move(status);
  requested_requantizer_settings_ = settings;
  ++requantization_requests_;
}

void SnapshotAnalyser::Begin() {
  // Created here rather than in the constructor because a QTimer fires on the
  // thread it was created on. Built in the constructor it would belong to the
  // GUI thread, and every transform would run there — which is the one thing
  // this class exists to prevent.
  timer_ = new QTimer(this);
  timer_->setInterval(AnalysisWorker::kPollIntervalMilliseconds);
  connect(timer_, &QTimer::timeout, this, &SnapshotAnalyser::Poll);
  timer_->start();
}

void SnapshotAnalyser::Poll() {
  if (options_changed_.exchange(false)) {
    // The analyser holds its window and its buffers sized to a transform, so
    // changing either option means building another one. It happens when a user
    // moves a control, not per frame.
    analysis::SpectrumAnalyser::Options options;
    options.averaging = requested_averaging_.load();
    options.transform_size = requested_transform_size_.load();
    spectrum_ = analysis::SpectrumAnalyser(options);
    noise_spectrum_ = analysis::SpectrumAnalyser(options);
  }

  if (peak_hold_reset_requested_.exchange(false)) {
    spectrum_.ResetPeakHold();
  }

  // Which panel shows which signal. The spectrum's averages start again when
  // what it is averaging changes; the scope has nothing to start again.
  const bool scope_corrected = scope_corrected_.load();
  const bool spectrum_corrected = spectrum_corrected_.load();
  if (spectrum_corrected != applied_spectrum_corrected_) {
    applied_spectrum_corrected_ = spectrum_corrected;
    spectrum_.Reset();
  }

  const uint64_t requested_conversion = requested_conversion_.load();
  if (requested_conversion != applied_conversion_) {
    applied_conversion_ = requested_conversion;
    if (spectrum_corrected) {
      spectrum_.Reset();
    }
  }
  const capture::SampleConversion conversion =
      capture::UnpackSampleConversion(applied_conversion_);

  {
    const std::lock_guard<std::mutex> lock(requantization_mutex_);
    if (requantization_requests_ != requantization_applied_) {
      requantization_applied_ = requantization_requests_;
      if ((requantization_ != nullptr) !=
          (requested_requantization_ != nullptr)) {
        if (spectrum_corrected) {
          spectrum_.Reset();
        }
        noise_spectrum_.Reset();
      }
      requantization_ = requested_requantization_;
      requantizer_settings_ = requested_requantizer_settings_;
    }
  }

  // The decision in force, with the quantiser it carries: the filter adaptive
  // shaping designed for that segment, or the fixed one. Built from its
  // coefficients, which costs nothing beside the snapshot it rounds.
  bool requantizing = false;
  if (requantization_ != nullptr) {
    const capture::RequantizationStatus::Live live =
        requantization_->ReadLive();
    if (live.current.lsb_drop > 0) {
      quantizer_ =
          capture::DecisionQuantizer(requantizer_settings_, live.current);
      requantizing = true;
    }
  }

  {
    const std::lock_guard<std::mutex> lock(source_mutex_);
    if (source_ == nullptr) {
      return;
    }

    uint64_t generation = 0;
    if (!source_->TryRead(wire_, generation)) {
      // Nothing new since the last poll, which is the ordinary case: the
      // pipeline publishes about nine snapshots a second and this looks thirty
      // times.
      return;
    }
  }

  const size_t sample_count = wire_.size() / capture::kBytesPerSample;
  codes_.resize(sample_count);
  for (size_t index = 0; index < sample_count; ++index) {
    // Assembled from bytes rather than reinterpreted as uint16_t: the wire
    // format is little-endian regardless of what this machine is, and a cast
    // would be right on one architecture and silently wrong on another.
    const uint16_t word =
        static_cast<uint16_t>(wire_[index * capture::kBytesPerSample]) |
        static_cast<uint16_t>(
            static_cast<uint16_t>(wire_[(index * capture::kBytesPerSample) + 1])
            << 8);
    codes_[index] = capture::SampleValueFromWord(word);
  }

  // The signal as the capture writes it, in the codes every display is drawn
  // in, made only when a panel is showing it: converted, then rounded as the
  // file is by the decision's own quantiser, from a fresh start — a snapshot
  // is not continuous with the one before it. What the rounding added is kept
  // as well, for the spectrum to show on its own.
  const bool correcting = scope_corrected || spectrum_corrected;
  const bool showing_noise = requantizing && spectrum_corrected;
  if (correcting) {
    corrected_codes_.resize(sample_count);
    samples_.resize(sample_count);
    for (size_t index = 0; index < sample_count; ++index) {
      samples_[index] =
          capture::ToConvertedSigned16Bit(codes_[index], conversion);
      corrected_codes_[index] =
          static_cast<uint16_t>(capture::ToTenBit(samples_[index]));
    }
    if (requantizing) {
      quantizer_.Reset();
      quantizer_.QuantizeInPlace(samples_.data(), sample_count);
      if (showing_noise) {
        noise_codes_.resize(sample_count);
      }
      for (size_t index = 0; index < sample_count; ++index) {
        const int32_t rounded = capture::ToTenBit(samples_[index]);
        if (showing_noise) {
          // Centred on mid-scale, where the analyser expects a signal to sit.
          noise_codes_[index] = static_cast<uint16_t>(std::clamp(
              rounded - static_cast<int32_t>(corrected_codes_[index]) +
                  capture::kSampleZeroOffset,
              static_cast<int32_t>(capture::kMinimumSampleValue),
              static_cast<int32_t>(capture::kMaximumSampleValue)));
        }
        corrected_codes_[index] = static_cast<uint16_t>(rounded);
      }
    }
  }

  emit WaveformReady(scope_corrected ? corrected_codes_ : codes_);

  const std::vector<uint16_t>& analysed =
      spectrum_corrected ? corrected_codes_ : codes_;
  if (spectrum_.Analyse(analysed.data(), analysed.size())) {
    emit SpectrumReady(spectrum_.magnitudes_db(), spectrum_.peak_hold_db(),
                       spectrum_.snapshot_db(), spectrum_.segment_count());

    // The requantiser's added noise on its own, on the same scale, or nothing
    // when there is none to show.
    if (showing_noise &&
        noise_spectrum_.Analyse(noise_codes_.data(), noise_codes_.size())) {
      emit NoiseSpectrumReady(noise_spectrum_.magnitudes_db());
    } else if (!showing_noise) {
      emit NoiseSpectrumReady({});
    }
  }
}

AnalysisWorker::AnalysisWorker(QObject* parent) : QObject(parent) {
  // Registered here rather than in main() so that the types are known to the
  // meta-object system before any connection is made, whatever creates this.
  qRegisterMetaType<std::vector<uint16_t>>();
  qRegisterMetaType<std::vector<double>>();
}

AnalysisWorker::~AnalysisWorker() {
  Stop();

  // Only reachable when Start() was never called; otherwise Stop() has already
  // let the thread delete it.
  delete analyser_;
}

void AnalysisWorker::Start() {
  if (thread_.isRunning()) {
    return;
  }

  analyser_ = new SnapshotAnalyser();

  // Auto connections, which become queued because the sender ends up on the
  // worker thread and the receiver stays here. That is what puts the vectors
  // back on the GUI thread as values rather than as references to something the
  // worker is about to overwrite.
  connect(analyser_, &SnapshotAnalyser::WaveformReady, this,
          &AnalysisWorker::WaveformReady);
  connect(analyser_, &SnapshotAnalyser::SpectrumReady, this,
          &AnalysisWorker::SpectrumReady);
  connect(analyser_, &SnapshotAnalyser::NoiseSpectrumReady, this,
          &AnalysisWorker::NoiseSpectrumReady);

  analyser_->moveToThread(&thread_);
  connect(&thread_, &QThread::started, analyser_, &SnapshotAnalyser::Begin);
  connect(&thread_, &QThread::finished, analyser_, &QObject::deleteLater);

  thread_.start();
}

void AnalysisWorker::Stop() {
  if (!thread_.isRunning()) {
    return;
  }

  // Detached before the thread is asked to stop, so that whatever the caller
  // does next to the publisher cannot race a poll already under way.
  if (analyser_ != nullptr) {
    analyser_->SetSource(nullptr);
  }

  thread_.quit();
  thread_.wait();

  // The deleteLater above has run by now: a thread's deferred deletions are
  // processed as its event loop exits.
  analyser_ = nullptr;
}

void AnalysisWorker::SetSource(capture::SnapshotPublisher* snapshots) {
  if (analyser_ != nullptr) {
    analyser_->SetSource(snapshots);
  }
}

void AnalysisWorker::SetSpectrumAveraging(double averaging) {
  if (analyser_ != nullptr) {
    analyser_->SetSpectrumAveraging(averaging);
  }
}

void AnalysisWorker::SetSpectrumTransformSize(size_t transform_size) {
  if (analyser_ != nullptr) {
    analyser_->SetSpectrumTransformSize(transform_size);
  }
}

void AnalysisWorker::ResetPeakHold() {
  if (analyser_ != nullptr) {
    analyser_->RequestPeakHoldReset();
  }
}

void AnalysisWorker::SetConversion(
    const capture::SampleConversion& conversion) {
  if (analyser_ != nullptr) {
    analyser_->SetConversion(conversion);
  }
}

void AnalysisWorker::SetCorrected(bool scope, bool spectrum) {
  if (analyser_ != nullptr) {
    analyser_->SetCorrected(scope, spectrum);
  }
}

void AnalysisWorker::SetRequantization(
    std::shared_ptr<capture::RequantizationStatus> status,
    const capture::RequantizerSettings& settings) {
  if (analyser_ != nullptr) {
    analyser_->SetRequantization(std::move(status), settings);
  }
}

}  // namespace ddd::gui
