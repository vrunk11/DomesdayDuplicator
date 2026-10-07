/************************************************************************

    analysis_worker.h

    Snapshot analysis, off the thread that has to stay responsive
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <QObject>
#include <QThread>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "capture_metatypes.h"
#include "monitor_tap.h"
#include "requantizing_sink.h"
#include "rf_requantizer.h"
#include "sample_format.h"
#include "spectrum_analyser.h"

class QTimer;

namespace ddd::gui {

// Where the transforms happen, and why not on the GUI thread.
//
// A 4,096-point FFT is a millisecond or so. Thirty a second on the GUI thread
// would be three per cent of it — not enough to stutter, until the machine is
// also compositing, encoding and moving 80 MB/s off a USB device, which is
// precisely when a user is watching the display to find out whether it is
// coping. The measurement must not be part of what it is measuring.
//
// Nothing here can slow the capture down. The snapshots come from the
// triple-buffered publisher in monitor_tap.h, which never makes its writer wait
// for anything: a poll that finds no new snapshot returns immediately, and a
// consumer too slow to keep up misses snapshots rather than delaying the
// pipeline. Frames are dropped, never queued — an old picture of a live signal
// is of no interest, and a backlog of them would be worse than useless.

// The half that lives on the worker thread. Created on the GUI thread, moved
// once, and never touched from anywhere else except through the small guarded
// surface below.
class SnapshotAnalyser : public QObject {
  Q_OBJECT

 public:
  SnapshotAnalyser() = default;

  // Attach to a running pipeline's publisher, or detach with nullptr.
  //
  // Blocks until the worker is out of the publisher, so a caller may destroy it
  // the moment this returns — which matters, because the pipeline builds a new
  // publisher for every run. The wait is a snapshot copy at most, and it is the
  // GUI thread waiting on the worker rather than anything waiting on the
  // capture.
  void SetSource(capture::SnapshotPublisher* snapshots);

  void SetSpectrumAveraging(double averaging);

  // The segment length the spectrum is estimated with, which is what sets its
  // bin width. Rebuilding the analyser is what applies it, so this is a control
  // a user moves and not something to call per frame.
  void SetSpectrumTransformSize(size_t transform_size);

  void RequestPeakHoldReset();

  // The signal as the capture would write it: every code put through
  // `conversion`, for whichever panel is showing it (SetCorrected). Taken at
  // the next snapshot, and the spectrum's averages start again with it when
  // the spectrum is showing it — an average across the change would be of two
  // different signals.
  void SetConversion(const capture::SampleConversion& conversion);

  // Which panels are shown the signal as written rather than the converter's
  // own codes: the scope and the spectrum each choose for themselves.
  void SetCorrected(bool scope, bool spectrum);

  // Show the requantiser's effect on top of the conversion: every snapshot
  // rounded as the decision in force in `status` would round it, with the
  // quantiser the file gets (capture::DecisionQuantizer). A null status shows
  // none. Decisions change several times a second and the spectrum's averages
  // carry across them; turning this on or off starts them again.
  void SetRequantization(std::shared_ptr<capture::RequantizationStatus> status,
                         const capture::RequantizerSettings& settings);

 public slots:
  // Builds the poll timer. Connected to the thread's started() signal so that
  // the timer is created on the thread it will fire on.
  void Begin();

  void Poll();

 signals:
  void WaveformReady(const std::vector<uint16_t>& codes);

  // Three readings of the same transform, because the two displays want
  // different ones: the trace wants the averaged levels and its peak hold,
  // the spectrogram wants this snapshot alone.
  //
  // segments is how many half-overlapped segments were averaged to produce
  // them. Carried with the levels rather than worked out by the panel because
  // it depends on the length of the buffer that happened to arrive, and a
  // readout that stated a figure the measurement did not use would be the
  // least useful kind of wrong: plausible, and unfalsifiable by looking.
  void SpectrumReady(const std::vector<double>& magnitudes_db,
                     const std::vector<double>& peak_hold_db,
                     const std::vector<double>& snapshot_db, size_t segments);

  // What the requantiser adds on its own — the rounded signal less the signal
  // it rounded — averaged as the trace is and on the same scale, with every
  // SpectrumReady while the spectrum shows a requantised signal; empty with
  // each one when it does not, so the panel clears it. The added noise is
  // under the signal in the bands it protects and is hard to see in the
  // whole; on its own it shows where the shaping put it.
  void NoiseSpectrumReady(const std::vector<double>& magnitudes_db);

 private:
  // Guards the source pointer and the read through it, and nothing else. Held
  // for a memcpy, never for a transform.
  std::mutex source_mutex_;
  capture::SnapshotPublisher* source_ = nullptr;

  QTimer* timer_ = nullptr;

  analysis::SpectrumAnalyser spectrum_;
  analysis::SpectrumAnalyser noise_spectrum_;
  std::atomic<double> requested_averaging_{analysis::kDefaultAveraging};

  // See SetCorrected(), and the spectrum's choice as it was last applied.
  std::atomic<bool> scope_corrected_{false};
  std::atomic<bool> spectrum_corrected_{false};
  bool applied_spectrum_corrected_ = false;
  std::atomic<size_t> requested_transform_size_{
      analysis::kDefaultTransformSize};

  // One flag for both requests above, because both are applied the same way —
  // by building another analyser — and a poll that found two separate flags set
  // would build two of them to no purpose.
  std::atomic<bool> options_changed_{false};
  std::atomic<bool> peak_hold_reset_requested_{false};

  // The conversion SetConversion() asked for, packed so that it is handed over
  // whole (capture::PackSampleConversion), and the one in force here.
  std::atomic<uint64_t> requested_conversion_{
      capture::PackSampleConversion(capture::SampleConversion{})};
  uint64_t applied_conversion_ =
      capture::PackSampleConversion(capture::SampleConversion{});

  // What SetRequantization() asked for, under its own lock, with a count of
  // the requests so the worker copies it only when it changes; and the
  // worker's copy, with the quantiser of the decision last seen.
  std::mutex requantization_mutex_;
  std::shared_ptr<capture::RequantizationStatus> requested_requantization_;
  capture::RequantizerSettings requested_requantizer_settings_;
  uint64_t requantization_requests_ = 0;

  uint64_t requantization_applied_ = 0;
  std::shared_ptr<capture::RequantizationStatus> requantization_;
  capture::RequantizerSettings requantizer_settings_;
  capture::ShapingQuantizer quantizer_;

  // Worker-thread scratch. Reused rather than reallocated per frame.
  std::vector<uint8_t> wire_;
  std::vector<uint16_t> codes_;
  std::vector<uint16_t> corrected_codes_;
  std::vector<uint16_t> noise_codes_;
  std::vector<int16_t> samples_;
};

// The GUI-side handle. Owns the thread and the object on it, and re-emits what
// that object produces so that nothing outside this file has to know there is a
// second thread at all.
class AnalysisWorker : public QObject {
  Q_OBJECT

 public:
  explicit AnalysisWorker(QObject* parent = nullptr);
  ~AnalysisWorker() override;

  // About 30 Hz. Ahead of the pipeline's own snapshot rate of roughly 9 Hz, so
  // a snapshot is picked up in the frame after it is published rather than
  // waiting out a slower poll.
  static constexpr int kPollIntervalMilliseconds = 33;

  void Start();
  void Stop();

  bool running() const { return thread_.isRunning(); }

  // All of these are no-ops before Start() and after Stop(): there is no thread
  // to carry the request to, and a caller should not have to check.
  void SetSource(capture::SnapshotPublisher* snapshots);
  void SetSpectrumAveraging(double averaging);
  void SetSpectrumTransformSize(size_t transform_size);
  void ResetPeakHold();
  void SetConversion(const capture::SampleConversion& conversion);
  void SetCorrected(bool scope, bool spectrum);
  void SetRequantization(std::shared_ptr<capture::RequantizationStatus> status,
                         const capture::RequantizerSettings& settings);

 signals:
  void WaveformReady(const std::vector<uint16_t>& codes);

  // Three readings of the same transform and the number of segments they were
  // averaged over. See SnapshotAnalyser::SpectrumReady.
  void SpectrumReady(const std::vector<double>& magnitudes_db,
                     const std::vector<double>& peak_hold_db,
                     const std::vector<double>& snapshot_db, size_t segments);

  // See SnapshotAnalyser::NoiseSpectrumReady.
  void NoiseSpectrumReady(const std::vector<double>& magnitudes_db);

 private:
  QThread thread_;

  // Raw rather than a unique_ptr: once the thread is running this object
  // belongs to it, and deleting it from here would be deleting an object on
  // another thread. It is destroyed by a deleteLater connected to the thread's
  // finished signal, which runs on the thread that owns it.
  SnapshotAnalyser* analyser_ = nullptr;
};

}  // namespace ddd::gui
