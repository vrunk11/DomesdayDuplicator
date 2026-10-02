/************************************************************************

    capture_controller.cpp

    The bridge between the GUI and the capture engine
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "capture_controller.h"

#include <QDir>
#include <QMetaObject>
#include <QThread>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <system_error>

#include "capture_failure_presenter.h"
#include "capture_format.h"
#include "capture_metadata.h"
#include "capture_naming.h"
#include "capture_provenance.h"
#include "firmware_version.h"
#include "free_space.h"
#include "gain_choices.h"
#include "log_format.h"
#include "logger.h"
#include "pipe_sink.h"
#include "raw_sink.h"
#include "sample_format.h"
#include "sample_sink.h"
#include "statistics_presenter.h"
#include "version.h"
#include "wire_protocol.h"

namespace ddd::gui {
namespace {

QString ToQString(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// What a piped stream carries, said when it starts. A raw stream has no header
// to hold any of it, so this line is the only place a script can learn the rate
// and the correction it is reading — said to whoever is watching, and kept by
// whatever is capturing standard error.
QString DescribePipedStream(uint32_t sample_rate_hz, bool range_2vpp,
                            const capture::SampleConversion& conversion,
                            bool test_mode) {
  QString text =
      QObject::tr(
          "Standard output carries signed 16-bit samples at %1 Msps, %2 input "
          "range")
          .arg(QString::fromStdString(capture::FormatDecimal(
              static_cast<double>(sample_rate_hz) / 1.0e6, 3)))
          .arg(QString::fromUtf8(capture::InputRangeName(range_2vpp)));
  if (test_mode) {
    text += QObject::tr(", in test mode");
  } else {
    text += QObject::tr(", DC offset %1 taken out, bit shift %2, LSB drop %3")
                .arg(conversion.dc_offset)
                .arg(capture::BitShift(conversion))
                .arg(capture::LsbDrop(conversion));
  }
  return text + QStringLiteral(".");
}

// How much a running total has moved since a capture started.
//
// The pipeline's counters cover the whole session, and a file's metadata is
// about the file — so what is recorded is the difference. The guard is for a
// counter that has somehow gone backwards, which cannot happen within a run;
// treating the reading as the whole of it is a wrong-but-bounded answer, where
// the subtraction would wrap to something like eighteen quintillion and read as
// a catastrophe.
uint64_t Since(uint64_t now, uint64_t at_start) {
  return now >= at_start ? now - at_start : now;
}

}  // namespace

CaptureController::CaptureController(capture::IUsbDevice* device,
                                     capture::ILogger* logger, QObject* parent)
    : QObject(parent),
      device_(device),
      logger_(logger),
      settings_(LoadCaptureSettings()),
      pipeline_(std::make_unique<capture::CapturePipeline>(logger)),
      analysis_(std::make_unique<AnalysisWorker>()) {
  qRegisterMetaType<capture::CaptureStats>();
  qRegisterMetaType<std::vector<capture::DeviceInfo>>();

  stats_timer_.setInterval(kStatsIntervalMilliseconds);
  connect(&stats_timer_, &QTimer::timeout, this, &CaptureController::Tick);

  measure_timer_.setInterval(kStatsIntervalMilliseconds);
  connect(&measure_timer_, &QTimer::timeout, this,
          &CaptureController::MeasurementStep);
}

CaptureController::~CaptureController() {
  // Order matters. The monitor's thread calls back into this object, so it has
  // to be stopped and joined before anything it might touch is destroyed. The
  // pipeline is torn down after, and its own destructor aborts and joins.
  if (monitor_ != nullptr) {
    monitor_->Stop();
  }
  if (analysis_ != nullptr) {
    analysis_->Stop();
  }
  if (pipeline_ != nullptr) {
    pipeline_->Abort();
    pipeline_->Wait();
  }
}

void CaptureController::Start() {
  if (device_ == nullptr) {
    emit Failed(tr("No USB support"),
                tr("The USB subsystem could not be started, so no device can "
                   "be found. Check that libusb is installed."));
    return;
  }

  monitor_ = std::make_unique<capture::DeviceMonitor>(device_, logger_);
  monitor_->Start([this](const std::vector<capture::DeviceInfo>& devices) {
    // This runs on the monitor's thread. Everything past this point is on the
    // GUI thread, which is the whole reason the hop exists: a QWidget touched
    // from another thread is undefined behaviour that usually works.
    QMetaObject::invokeMethod(
        this, [this, devices] { OnDevicesChanged(devices); },
        Qt::QueuedConnection);
  });
}

void CaptureController::SetDeviceMonitorSuspended(bool suspended) {
  // Null-checked because Start() may never have been called, or may have
  // failed, and neither is a reason to refuse an update.
  if (monitor_ != nullptr) {
    monitor_->SetSuspended(suspended);
  }
}

void CaptureController::SetSettings(const CaptureSettings& settings) {
  settings_ = settings;
  SaveCaptureSettings(settings_);
  emit SettingsChanged(settings_);

  // A rate the board cannot run is brought back inside it for the session,
  // whatever the settings arrived with — a dialog opened before the board
  // setup changed hands back the rate it was opened with.
  ApplyBoardLimits();
}

void CaptureController::ApplySessionSettings(const CaptureSettings& settings) {
  settings_ = settings;
  emit SettingsChanged(settings_);
  ApplyBoardLimits();
}

void CaptureController::SetPipeOutput(
    std::shared_ptr<capture::IByteStream> stream, bool save_file) {
  pipe_stream_ = std::move(stream);
  pipe_saves_file_ = save_file;
}

void CaptureController::SetDiscProvenance(const capture::DiscProvenance& disc) {
  disc_provenance_ = disc;
}

void CaptureController::SetPlayerIdentity(
    const capture::PlayerIdentity& player) {
  player_identity_ = player;
}

void CaptureController::SetDiscScan(const capture::DiscScan& disc) {
  disc_scan_ = disc;
}

void CaptureController::OnDevicesChanged(
    const std::vector<capture::DeviceInfo>& devices) {
  devices_ = devices;
  CheckFirmware(devices);
  emit DevicesChanged(devices_);
}

void CaptureController::CheckFirmware(
    const std::vector<capture::DeviceInfo>& devices) {
  const capture::DeviceInfo* const selected = capture::SelectDevice(
      devices, settings_.preferred_device_path.toStdString());

  if (selected == nullptr) {
    warned_device_path_.clear();
    warned_device_product_.clear();
    fpga_version_ = capture::FpgaVersion{};
    max_adc_rate_mhz_ = 0;
    last_pll_preset_sent_ = -1;
    if (board_setup_.source != capture::BoardSetupSource::kUnavailable) {
      board_setup_ = capture::BoardSetupReading{};
      emit BoardSetupChanged();
    }
    return;
  }

  const QString path = QString::fromStdString(selected->path);
  const QString product = QString::fromStdString(selected->product_string);
  if (path == warned_device_path_ && product == warned_device_product_) {
    return;
  }
  warned_device_path_ = path;
  warned_device_product_ = product;

  // A device seen for the first time, or again after it went away, has been
  // sent no preset. Its PLL came up at the rate its gateware was compiled for
  // whatever this application last asked of a device, and remembering the old
  // request here skipped the write that would have changed it: a board
  // replugged after a 60 MHz request captured at 75 MHz, in a file labelled 60.
  last_pll_preset_sent_ = -1;

  // Read the gateware's identity while the device is being looked at anyway,
  // so the Firmware dialog can show all three versions without opening the
  // device itself. A device that cannot answer leaves this default
  // constructed, which reads as "not known" and is not an error — gateware
  // predating the register interface, or an FPGA that was never configured,
  // both land here and both capture perfectly well.
  fpga_version_ = ReadFpgaVersion(selected->path);
  max_adc_rate_mhz_ = ReadMaxAdcRateMhz(selected->path);

  // The board setup, from the device itself: it is kept by the FX3 kit rather
  // than by this machine, so a kit moved between computers brings its
  // declaration with it. Read on every appearance, because a kit can also be
  // moved to another capture board between two of them.
  board_setup_ = device_ != nullptr
                     ? capture::ReadBoardSetup(*device_, selected->path)
                     : capture::BoardSetupReading{};
  if (logger_ != nullptr) {
    const capture::BoardSetup& board = board_setup_.setup;
    const auto table = [](const capture::DcOffsetTable& offsets) {
      std::string text;
      for (const int16_t offset : offsets) {
        text += (text.empty() ? "" : " ") + std::to_string(offset);
      }
      return text;
    };
    std::string described =
        std::string(capture::AdcPartName(board.adc)) + ", RSEL " +
        capture::RselWiringName(board.rsel_wiring) +
        ", DC offsets at 40-75 MHz: 1Vpp " + table(board.dc_offset_1vpp) +
        ", 2Vpp " + table(board.dc_offset_2vpp);
    switch (board_setup_.source) {
      case capture::BoardSetupSource::kDeclared:
        logger_->Info("Board setup: " +
                      (board.name.empty() ? std::string("unnamed board")
                                          : "\"" + board.name + "\"") +
                      ", " + described);
        break;
      case capture::BoardSetupSource::kBlank:
        logger_->Info(
            "Board setup: nothing declared on this device, so the "
            "defaults apply (" +
            described + ")");
        break;
      case capture::BoardSetupSource::kDamaged:
        logger_->Warning(
            "Board setup: the record on this device is damaged, "
            "so the defaults apply until it is written again");
        break;
      case capture::BoardSetupSource::kNewerLayout:
        logger_->Warning(
            "Board setup: the record on this device was written "
            "by a newer application, so the defaults apply");
        break;
      case capture::BoardSetupSource::kUnsupported:
        logger_->Info(
            "Board setup: this device's firmware cannot store one, "
            "so the defaults apply (" +
            described + ")");
        break;
      case capture::BoardSetupSource::kUnavailable:
        break;
    }
  }
  emit BoardSetupChanged();

  ApplyBoardLimits();

  const capture::FirmwareIdentity firmware =
      capture::DescribeFirmware(selected->product_string);

  // All three still recorded on connect, and this is where the three-way
  // comparison went: into the log, where it is evidence in a bug report, and
  // out of the interface, where it was an accusation. The application and the
  // device come from separate release streams, so a difference between them is
  // the ordinary state of an up-to-date Duplicator rather than something to
  // interrupt anybody over.
  //
  // The device's two commits are here; the application's own is logged once at
  // startup, on a line of its own. Putting it on the end of this one made a
  // line about the device carry a fact about the application, and a reader
  // looking for the build had to know to find it there.
  if (logger_ != nullptr && firmware.NamesCommit()) {
    logger_->Info("Device firmware commit " + firmware.commit);
  }

  if (logger_ != nullptr && fpga_version_.present) {
    logger_->Info("Device gateware commit " +
                  (fpga_version_.commit.empty() ? std::string("unknown")
                                                : fpga_version_.commit) +
                  (fpga_version_.dirty ? " (modified)" : ""));
  }

  if (firmware.ShouldWarn()) {
    emit FirmwareWarning(QString::fromStdString(firmware.message));
  }
}

capture::DeviceBuild CaptureController::CurrentDeviceBuild() const {
  capture::DeviceBuild build;

  const capture::DeviceInfo* const selected = capture::SelectDevice(
      devices_, settings_.preferred_device_path.toStdString());
  if (selected != nullptr) {
    if (const std::optional<std::string> commit =
            capture::ParseFirmwareCommit(selected->product_string);
        commit.has_value()) {
      build.firmware_version = *commit;
    }
  }

  // The gateware's answer as it stood when the device was last looked at. Only
  // where the identity block was actually read: an absent or unconfigured FPGA
  // leaves this default constructed, and a register map of zero recorded from
  // that would be a reading rather than the absence of one.
  if (fpga_version_.present) {
    build.gateware_register_map = fpga_version_.map_version;
    if (!fpga_version_.commit.empty()) {
      // The same "-dirty" convention the application's own stamp uses, so a
      // reader has one rule for all three versions in the document — see
      // DeviceBuild::gateware_version.
      build.gateware_version =
          fpga_version_.commit + (fpga_version_.dirty ? "-dirty" : "");
    }
  }

  return build;
}

capture::FpgaVersion CaptureController::ReadFpgaVersion(
    const std::string& path) {
  if (device_ == nullptr) {
    return {};
  }

  std::vector<uint8_t> identity;
  if (!device_->ReadRegisters(path, capture::kRegisterId,
                              capture::kIdentityLength, identity)) {
    return {};
  }

  return capture::ParseFpgaIdentity(identity);
}

uint8_t CaptureController::ReadMaxAdcRateMhz(const std::string& path) {
  if (device_ == nullptr) {
    return 0;
  }

  std::vector<uint8_t> value;
  if (!device_->ReadRegisters(path, capture::kRegisterMaxAdcRateMhz, 1,
                              value) ||
      value.empty()) {
    return 0;
  }

  return value[0];
}

uint8_t CaptureController::max_adc_rate_mhz() const {
  if (max_adc_rate_mhz_ == 0) {
    return 0;
  }
  return std::min(max_adc_rate_mhz_,
                  capture::AdcPartMaxRateMhz(board_setup_.setup.adc));
}

void CaptureController::ApplyBoardLimits() {
  // A board that reports its capability runs at a rate this application can
  // name, so the setting names it rather than leaving it as "board default" -
  // which every figure worked out from the rate (the file's label and tags,
  // the displays, the duration limit) had to read as the historical 40 MHz,
  // wrongly for any faster board. A setting the board cannot run is replaced
  // the same way — which now includes a setting faster than the converter the
  // board setup declares, however fast the gateware could drive one. Applied
  // for this session rather than saved, because a command line's overrides
  // may be in effect and those are never written; the next change made in the
  // window saves it along with everything else.
  const uint8_t max_rate = max_adc_rate_mhz();
  if (max_rate == 0 || monitoring_) {
    return;
  }

  const uint8_t wanted = settings_.pll_preset_mhz;
  const uint8_t board_rate = capture::HighestPllPresetAtMost(max_rate);
  if (board_rate != 0 &&
      (wanted > max_rate || !capture::IsSupportedPllPreset(wanted))) {
    settings_.pll_preset_mhz = board_rate;
    emit SettingsChanged(settings_);
  }
}

bool CaptureController::RunRange2Vpp() const {
  return range_override_.has_value() ? *range_override_
                                     : effective_range_2vpp();
}

uint8_t CaptureController::RunRateMhz() const {
  // "Board default" is the historical 40 MHz as far as every figure worked out
  // from the rate is concerned (CaptureSettings::BaseSampleRateHz), and so it
  // is here: the offset measured at 40 is the one that applies.
  return rate_override_.value_or(configured_rate_mhz());
}

int32_t CaptureController::RunDcOffset() const {
  // Nothing is corrected in test mode, whose samples are the gateware's
  // counter and not the converter's, or while measuring, where what is wanted
  // is the offset itself.
  if (settings_.test_mode || measuring_dc_offset()) {
    return 0;
  }
  return capture::DcOffsetFor(board_setup_.setup, RunRange2Vpp(), RunRateMhz());
}

capture::SampleConversion CaptureController::RunConversion() const {
  capture::SampleConversion conversion;
  conversion.dc_offset = RunDcOffset();

  // Neither in test mode, whose ramp has to reach the file exactly as the
  // gateware counted it for the ramp check to mean anything, nor while
  // measuring, where what is wanted is the converter as it is.
  if (!settings_.test_mode && !measuring_dc_offset()) {
    conversion.bit_shift = settings_.bit_shift;
    conversion.lsb_drop = settings_.lsb_drop;
  }
  return conversion;
}

bool CaptureController::WriteBoardSetup(const capture::BoardSetup& setup,
                                        QString& message) {
  if (monitoring_ || measuring_dc_offset()) {
    message = tr("Stop monitoring before writing to the board.");
    return false;
  }

  const capture::DeviceInfo* const selected = capture::SelectDevice(
      devices_, settings_.preferred_device_path.toStdString());
  if (selected == nullptr || device_ == nullptr) {
    message = tr("No Domesday Duplicator is attached.");
    return false;
  }

  // What the record will actually hold — the name cut to fit, the offsets
  // inside the converter's range — so that what is put in force here is what
  // the device will read back next time, not what was typed.
  const capture::BoardSetup normalised =
      capture::DecodeBoardSetup(capture::EncodeBoardSetup(setup)).setup;

  switch (capture::WriteBoardSetup(*device_, selected->path, normalised)) {
    case capture::BoardSetupWriteResult::kWritten:
      board_setup_.source = capture::BoardSetupSource::kDeclared;
      board_setup_.setup = normalised;
      message = tr("Written to the board.");
      if (logger_ != nullptr) {
        logger_->Info("Board setup written to the device");
      }
      break;

    case capture::BoardSetupWriteResult::kUnsupported:
      // In force anyway, and said to be temporary. A board on old firmware is
      // still the board it is, and refusing to let its owner say so would
      // leave a 75 MHz converter capped at 40 until they updated.
      board_setup_.source = capture::BoardSetupSource::kUnsupported;
      board_setup_.setup = normalised;
      message =
          tr("This device's firmware cannot store a board setup, so it applies "
             "until the application closes. Update the firmware to keep it on "
             "the board.");
      if (logger_ != nullptr) {
        logger_->Warning(
            "Board setup put in force for this session only: the device's "
            "firmware cannot store it");
      }
      break;

    case capture::BoardSetupWriteResult::kNotConfirmed:
      message =
          tr("The board did not keep what was written to it, so nothing "
             "has changed. Try again, and update the firmware if it "
             "keeps happening.");
      if (logger_ != nullptr) {
        logger_->Warning("Board setup write was not confirmed by the readback");
      }
      return false;

    case capture::BoardSetupWriteResult::kUnavailable:
      message =
          tr("The device could not be opened. It may have been "
             "unplugged, or another application may be using it.");
      return false;
  }

  emit BoardSetupChanged();
  ApplyBoardLimits();
  return true;
}

void CaptureController::MeasureDcOffset(const std::vector<bool>& ranges,
                                        uint8_t max_rate_mhz) {
  if (measuring_dc_offset()) {
    return;
  }
  if (monitoring_) {
    emit DcOffsetMeasurementFinished(
        false, tr("Stop monitoring before measuring the DC offset."));
    return;
  }
  if (capture::SelectDevice(
          devices_, settings_.preferred_device_path.toStdString()) == nullptr) {
    emit DcOffsetMeasurementFinished(false,
                                     tr("No Domesday Duplicator is attached."));
    return;
  }

  // Unless told otherwise: both ranges where RSEL reaches the FPGA, since each
  // has an offset of its own; only the wired one where it does not, since the
  // other cannot be selected and a measurement labelled with it would be of the
  // wired one.
  std::vector<bool> measured_ranges = ranges;
  if (measured_ranges.empty()) {
    measured_ranges = input_range_selectable()
                          ? std::vector<bool>{false, true}
                          : std::vector<bool>{effective_range_2vpp()};
  }

  // Every rate the gateware can drive and the converter is declared for,
  // since the offset moves with the clock. A gateware that cannot report its
  // capability cannot be asked for a rate either: it runs at the one it was
  // built for, which every figure takes to be 40 MHz, so that is where its
  // measurement is filed.
  measure_steps_.clear();
  if (max_adc_rate_mhz_ == 0) {
    for (const bool range_2vpp : measured_ranges) {
      measure_steps_.push_back({range_2vpp, capture::kPllPreset40Mhz, false});
    }
  } else {
    const uint8_t ceiling = max_rate_mhz != 0
                                ? std::min(max_rate_mhz, max_adc_rate_mhz_)
                                : max_adc_rate_mhz();
    for (const uint8_t rate : capture::kDcOffsetRatesMhz) {
      if (rate > ceiling) {
        continue;
      }
      // Both ranges at one rate before moving on, so the PLL is retuned once
      // per rate rather than once per run.
      for (const bool range_2vpp : measured_ranges) {
        measure_steps_.push_back({range_2vpp, rate, true});
      }
    }
  }
  if (measure_steps_.empty()) {
    emit DcOffsetMeasurementFinished(
        false, tr("This board has no ADC rate to measure at."));
    return;
  }

  measure_index_ = 0;
  measure_failure_.clear();
  measure_phase_ = MeasurePhase::kStarting;

  if (logger_ != nullptr) {
    logger_->Info("Measuring the DC offset over " +
                  std::to_string(capture::kDcOffsetAveragingMilliseconds) +
                  " ms at each of " + std::to_string(measure_steps_.size()) +
                  " rates and ranges");
  }

  measure_timer_.start();
  MeasurementStep();
}

void CaptureController::MeasurementStep() {
  switch (measure_phase_) {
    case MeasurePhase::kIdle:
      measure_timer_.stop();
      return;

    case MeasurePhase::kStarting: {
      const MeasureStep& step = measure_steps_[measure_index_];
      range_override_ = step.range_2vpp;
      if (step.sets_rate) {
        rate_override_ = step.rate_mhz;
      } else {
        rate_override_.reset();
      }
      measure_run_starting_ = true;
      StartMonitoring();
      measure_run_starting_ = false;
      if (!monitoring_) {
        // StartMonitoring has already said why through Failed().
        measure_failure_ = tr("The stream could not be started.");
        FinishMeasurement();
        return;
      }
      measure_clock_.start();
      measure_phase_ = MeasurePhase::kSettling;
      return;
    }

    case MeasurePhase::kSettling: {
      if (!monitoring_) {
        measure_failure_ = tr("The stream stopped while settling.");
        FinishMeasurement();
        return;
      }
      if (measure_clock_.elapsed() < capture::kDcOffsetSettlingMilliseconds) {
        return;
      }
      const capture::CaptureStats stats = pipeline_->stats().Read();
      measure_start_ = {stats.metrics.sample_count, stats.metrics.sum};
      measure_minimum_ = UINT16_MAX;
      measure_maximum_ = 0;
      measure_clock_.start();
      measure_phase_ = MeasurePhase::kAveraging;
      return;
    }

    case MeasurePhase::kAveraging: {
      if (!monitoring_) {
        measure_failure_ = tr("The stream stopped while measuring.");
        FinishMeasurement();
        return;
      }

      // The extremes of each buffer published during the window. Every tick
      // sees the latest buffer rather than every buffer, which is enough: a
      // signal on the input is there in all of them.
      const capture::CaptureStats stats = pipeline_->stats().Read();
      if (stats.metrics.sample_count > measure_start_.sample_count) {
        measure_minimum_ =
            std::min(measure_minimum_, stats.metrics.recent_minimum_value);
        measure_maximum_ =
            std::max(measure_maximum_, stats.metrics.recent_maximum_value);
      }
      if (measure_clock_.elapsed() < capture::kDcOffsetAveragingMilliseconds) {
        return;
      }

      const MeasureStep& step = measure_steps_[measure_index_];
      const capture::DcOffsetResult result = capture::ComputeDcOffset(
          measure_start_, {stats.metrics.sample_count, stats.metrics.sum},
          measure_minimum_, measure_maximum_);

      if (result.valid) {
        if (logger_ != nullptr) {
          logger_->Info(std::string("DC offset at ") +
                        capture::InputRangeName(step.range_2vpp) + ", " +
                        std::to_string(step.rate_mhz) + " MHz: mean " +
                        capture::FormatDecimal(result.mean, 2) +
                        " codes over a span of " +
                        std::to_string(static_cast<int>(measure_maximum_) -
                                       static_cast<int>(measure_minimum_)) +
                        ", declared as " + std::to_string(result.offset));
        }
        emit DcOffsetMeasured(step.range_2vpp, step.rate_mhz, result.offset);
      } else {
        measure_failure_ = QString::fromStdString(result.problem);
      }

      StopMonitoring();
      measure_phase_ = MeasurePhase::kStopping;
      return;
    }

    case MeasurePhase::kStopping: {
      // Stopping finishes on the stream's own schedule, noticed by Tick().
      if (monitoring_) {
        return;
      }
      ++measure_index_;
      if (!measure_failure_.isEmpty() ||
          measure_index_ >= measure_steps_.size()) {
        FinishMeasurement();
        return;
      }
      measure_phase_ = MeasurePhase::kStarting;
      return;
    }
  }
}

void CaptureController::FinishMeasurement() {
  measure_timer_.stop();
  measure_phase_ = MeasurePhase::kIdle;
  range_override_.reset();
  rate_override_.reset();

  if (monitoring_) {
    StopMonitoring();
  }

  const bool succeeded = measure_failure_.isEmpty();
  if (logger_ != nullptr && !succeeded) {
    logger_->Warning("DC offset measurement failed: " +
                     measure_failure_.toStdString());
  }
  emit DcOffsetMeasurementFinished(succeeded, measure_failure_);
}

void CaptureController::CheckDcOffsetSaturation(
    const capture::CaptureStats& stats) {
  if (offset_out_of_range_warned_ || !monitoring_ ||
      stats.metrics.offset_saturated_count == 0) {
    return;
  }
  offset_out_of_range_warned_ = true;

  const QString message =
      tr("The DC offset correction is pushing samples out of range. The offset "
         "declared for this board does not match its signal — the board setup "
         "is wrong, not the input level. Measure it again in Board setup.");
  if (logger_ != nullptr) {
    logger_->Warning(
        "The DC offset correction pushed " +
        std::to_string(stats.metrics.offset_saturated_count) +
        " samples out of range that the converter had not clipped: the "
        "declared offset of " +
        std::to_string(RunDcOffset()) + " does not belong to this board");
  }
  emit DcOffsetOutOfRange(message);
}

void CaptureController::CheckBitShiftClipping(
    const capture::CaptureStats& stats) {
  if (shift_clipping_warned_ || !monitoring_ ||
      stats.metrics.shift_clipped_count == 0) {
    return;
  }
  shift_clipping_warned_ = true;

  const QString message =
      tr("The bit shift is clipping the signal. Lower it in the capture "
         "panel, or leave it at 0: it makes a weak signal easier to read and "
         "adds no detail, so nothing is lost without it.");
  if (logger_ != nullptr) {
    logger_->Warning(
        "A bit shift of " + std::to_string(capture::BitShift(run_conversion_)) +
        " clipped " + std::to_string(stats.metrics.shift_clipped_count) +
        " samples that neither the converter nor the DC offset had");
  }
  emit BitShiftClipping(message);
}

void CaptureController::StartMonitoring() {
  if (monitoring_ || device_ == nullptr) {
    return;
  }

  // A measurement owns the stream until it finishes: it starts and stops it
  // once per range, and a run started from anywhere else in between would be
  // averaged as if it were one of its own.
  if (measuring_dc_offset() && !measure_run_starting_) {
    return;
  }

  const std::string path = settings_.preferred_device_path.toStdString();

  // First of every register written here, and deliberately: a preset change
  // makes the gateware drop its PLL out of lock while the newly scanned-in
  // counters settle, and everything else on this register bank - including
  // the writes just below - shares the PLL's own reset while that happens
  // (see the reset synchroniser in DomesdayDuplicator.v). Writing this after
  // the others would risk one of them landing mid-reset and being silently
  // dropped.
  //
  // Only sent when it is actually changing: PllPresetNone (0) is never acted
  // on by design (see pllPresetController.v) and costs nothing to send
  // regardless, and repeating the preset already active would trigger a
  // second unnecessary relock.
  //
  // kPllPresetSettleMilliseconds is a conservative placeholder, not a
  // measurement - the real reconfigure-and-relock time has not been
  // characterised on hardware yet (see TODO.md). It blocks this thread
  // rather than the capture, which has not started, so a delay that turns
  // out to be far too generous costs a one-time pause when the rate is
  // changed and nothing while capturing.
  //
  // A DC offset measurement steps through the rates itself, so its own rate
  // wins over the setting for the runs it starts.
  const uint8_t preset = rate_override_.value_or(settings_.pll_preset_mhz);
  if (preset != last_pll_preset_sent_) {
    if (!device_->WriteRegister(path, capture::kRegisterPllPreset, preset)) {
      emit Failed(tr("The ADC rate could not be set"),
                  tr("The device did not accept the sample-rate preset "
                     "request. It may have been unplugged, or another "
                     "application may be using it."));
      return;
    }
    last_pll_preset_sent_ = preset;

    if (preset != 0) {
      QThread::msleep(kPllPresetSettleMilliseconds);
    }
  }

  // Never in test mode while measuring a DC offset: test mode replaces the
  // converter with the gateware's counter, and the offset is the converter's.
  const bool test_mode = settings_.test_mode && !measuring_dc_offset();

  // Written before the device is opened for streaming rather than after. The
  // gateware applies it immediately and there is no acknowledgement, so doing
  // it while data is already flowing would put the mode change somewhere
  // unpredictable in the stream.
  if (!device_->WriteRegister(path, capture::kRegisterTestMode,
                              test_mode ? 1 : 0)) {
    emit Failed(tr("The device could not be configured"),
                tr("The device did not accept the configuration request. It "
                   "may have been unplugged, or another application may be "
                   "using it."));
    return;
  }

  // Decimation, on the same terms and for the same reason: the gateware
  // applies it immediately and without acknowledgement, so it is settled
  // before any data is flowing rather than somewhere unpredictable in it.
  //
  // Decimation is the device's to do, not this application's. Halving the rate
  // means low-passing the signal at 10 MHz first, or everything above that
  // folds down on top of the signal — and that filter is in the FPGA, where it
  // costs 13% of the logic and no CPU at all.
  if (!device_->WriteRegister(
          path, capture::kRegisterDecimation,
          static_cast<uint8_t>(settings_.decimation_factor))) {
    emit Failed(tr("The decimation could not be set"),
                tr("The device did not accept the decimation request. It may "
                   "have been unplugged, or another application may be using "
                   "it."));
    return;
  }

  // The ADC's input range, on the same terms: applied before the stream
  // opens, so it is settled before any data is flowing. The range the board
  // actually runs at rather than the one the settings ask for: on a board
  // whose RSEL is tied to a level the register reaches nothing, and writing
  // the wired range keeps what the register says in step with what the
  // hardware does.
  if (!device_->WriteRegister(path, capture::kRegisterRangeSelect,
                              RunRange2Vpp() ? capture::kRangeSelect2Vpp
                                             : capture::kRangeSelect1Vpp)) {
    emit Failed(tr("The input range could not be set"),
                tr("The device did not accept the input-range request. It "
                   "may have been unplugged, or another application may be "
                   "using it."));
    return;
  }

  // Before the stream is opened, because this is what puts the firmware into
  // its capturing state and the firmware spends that state holding the USB
  // link out of U1/U2. A link that drops into U2 once data is flowing loses
  // samples inside a transfer the host still sees as complete - see
  // kCollectionRequest. Refused rather than ignored: streaming without it is
  // how a capture comes out with a sequence break and no explanation.
  if (!device_->SetCollecting(path, true,
                              settings_.eco_mode
                                  ? capture::kCollectionFlagDarkLeds
                                  : capture::kCollectionFlagsNone)) {
    emit Failed(tr("The device could not be told to start"),
                tr("The device did not accept the start request. It may have "
                   "been unplugged, or another application may be using it."));
    return;
  }

  capture::TransferResult opened = capture::TransferResult::kConnectionFailure;
  source_ = device_->OpenSource(path, settings_.UsbOptions(), opened);
  if (source_ == nullptr) {
    device_->SetCollecting(path, false, capture::kCollectionFlagsNone);
    emit Failed(tr("The device could not be opened"),
                QString::fromUtf8(capture::TransferResultDescription(opened)));
    return;
  }

  capture::CapturePipeline::Options options;
  options.queue_size_bytes = settings_.queue_size_bytes;
  options.test_mode = test_mode;

  // Counted by the pipeline so that a declaration belonging to another board,
  // or a shift too large for the signal, shows during monitoring, before
  // anything has been written with it.
  options.conversion = RunConversion();
  run_conversion_ = options.conversion;
  offset_out_of_range_warned_ = false;
  shift_clipping_warned_ = false;

  // Only the log uses this, and it is why the log's times are right under
  // decimation: a 2:1 capture delivers half as many samples a second, so a
  // count of them stands for twice as long.
  options.sample_rate_hz = settings_.SampleRateHz();

  // Enumerating opens devices and does control transfers on them. Doing that to
  // a device that is streaming would put avoidable traffic on the bus for an
  // answer that is already obvious: data is arriving, so it is plainly still
  // attached.
  //
  // Null-checked because monitoring does not depend on the monitor: Start() may
  // never have been called, or may have failed, and neither is a reason to
  // refuse to stream from a device the caller has named.
  if (monitor_ != nullptr) {
    monitor_->SetSuspended(true);
  }

  if (!pipeline_->Start(source_.get(), std::make_unique<capture::NullSink>(),
                        options)) {
    if (monitor_ != nullptr) {
      monitor_->SetSuspended(false);
    }
    source_.reset();
    device_->SetCollecting(path, false, capture::kCollectionFlagsNone);
    emit Failed(tr("Monitoring could not be started"),
                QString::fromStdString(pipeline_->ResultDetail()));
    return;
  }

  // Started only for the duration of a run. A worker thread polling thirty
  // times a second for snapshots that cannot arrive is nothing measurable, but
  // it is also nothing at all, and a thread that only exists while it has work
  // is one fewer thing to explain in a stack trace.
  analysis_->Start();
  analysis_->SetSource(&pipeline_->snapshots());

  monitoring_ = true;
  stats_timer_.start();
  emit MonitoringChanged(true);
}

void CaptureController::StopMonitoring() {
  if (!monitoring_) {
    return;
  }

  // A capture still running when monitoring stops is finalised by the pipeline
  // on its way out — the sink is finished after both workers have joined. What
  // has to happen here is the bookkeeping: the file is no longer being written
  // to, and the panels have to be told before the run ends rather than after,
  // or a capture that ended with the stream would leave a button saying "stop".
  pipeline_->RequestStop();
}

std::unique_ptr<capture::ISampleSink> CaptureController::OpenCaptureFile() {
  // Cleared for every capture: only the one --pipe was set up for is piped.
  pipe_writer_.reset();
  pipe_only_ = false;
  pipe_closed_reported_ = false;
  pipe_drops_reported_ = false;

  if (pipe_stream_ != nullptr && !pipe_saves_file_) {
    return OpenPipeOnlyCapture();
  }

  const std::time_t now = std::time(nullptr);

  const QString directory = settings_.ResolvedCaptureDirectory();

  // Created rather than required. A user who types a folder name that does not
  // exist yet means "put it there", and refusing would be a dialog for
  // something the application can simply do.
  QDir().mkpath(directory);

  const int decimation = settings_.decimation_factor;

  // The same call the panel and the automatic capture ask, so that the name on
  // screen and the name on disk cannot disagree. The stem carries whatever the
  // naming fields say as well as whatever was typed — see
  // CaptureSettings::CaptureStem, which is the one place those are combined.
  const capture::CaptureDestination destination =
      capture::ResolveCaptureDestination(
          std::filesystem::path(directory.toStdString()),
          settings_.CaptureStem(now), settings_.test_mode, now,
          settings_.output_format);

  const std::filesystem::path& path = destination.path;

  if (!destination.as_requested) {
    // Never an overwrite — the path was made unique before anything was
    // opened — but it is a different file from the one the user asked for, and
    // a rename nobody was told about is how two captures of the same side end
    // up impossible to tell apart later.
    //
    // The name that was asked for is the stem the naming produced, not the
    // Name field's text: with the naming fields in use those are different
    // things, and a message naming the field would be reporting a collision
    // between two names neither of which is on disk.
    emit CaptureRenamed(QString::fromStdString(settings_.CaptureStem(now)),
                        QString::fromStdString(destination.stem));
  }

  std::unique_ptr<capture::ISampleSink> sink;
  std::string open_error;

  // The range this file is recorded at and what is done to its samples, fixed
  // for the file: both come from the run that is already streaming, so the
  // file is written with exactly what the pipeline has been counting against
  // and the scope has been showing.
  const bool range_2vpp = RunRange2Vpp();
  const capture::SampleConversion conversion = run_conversion_;
  const int32_t dc_offset = conversion.dc_offset;
  const int bit_shift = capture::BitShift(conversion);
  const int lsb_drop = capture::LsbDrop(conversion);
  const capture::BoardSetup& board = board_setup_.setup;
  const bool board_known =
      board_setup_.source != capture::BoardSetupSource::kUnavailable;
  const bool board_declared =
      board_setup_.source == capture::BoardSetupSource::kDeclared;

  if (settings_.output_format == capture::CaptureOutputFormat::kSigned16Bit) {
    auto raw = std::make_unique<capture::RawSink>();
    if (raw->Open(path, conversion)) {
      sink = std::move(raw);
    } else {
      open_error = raw->LastError();
    }
  } else {
    capture::FlacWriter::Options options;
    options.compression_level = settings_.compression_level;
    options.sample_rate_label = capture::FlacSampleRateLabelFor(
        decimation, settings_.BaseSampleRateHz());
    options.conversion = conversion;

    const capture::DeviceBuild build = CurrentDeviceBuild();

    capture::CaptureProvenance provenance;
    provenance.title = path.filename().string();
    provenance.application_version = std::string(capture::Commit());
    provenance.firmware_version = build.firmware_version;
    provenance.gateware_version = build.gateware_version;
    provenance.test_mode = settings_.test_mode;
    provenance.decimation_factor = decimation;
    provenance.base_sample_rate_hz = settings_.BaseSampleRateHz();
    provenance.input_range = capture::InputRangeName(range_2vpp);
    if (board_known) {
      provenance.board_setup = board_declared ? "declared" : "default";
      provenance.board_name = board.name;
      provenance.board_adc = capture::AdcPartName(board.adc);
      provenance.board_rsel_wiring = capture::RselWiringName(board.rsel_wiring);
      provenance.dc_offset = dc_offset;
    }
    provenance.bit_shift = bit_shift;
    provenance.lsb_drop = lsb_drop;
    provenance.started = now;
    provenance.disc = disc_provenance_;

    // Written only when a declaration was actually made. DescribeFrontEndGain
    // returns a sentence saying nothing has been declared for the undeclared
    // pattern, and putting that in a metadata field would be worse than leaving
    // the field out: it would read as calibration data.
    if (settings_.DeclaredGain().declared()) {
      provenance.front_end_gain =
          DescribeFrontEndGain(settings_.front_end_gain_switches).toStdString();
    }

    options.tags = capture::BuildProvenanceTags(provenance);

    auto flac = std::make_unique<capture::FlacSink>();
    if (flac->Open(path, options)) {
      sink = std::move(flac);
    } else {
      open_error = flac->LastError();
    }
  }

  if (sink == nullptr) {
    const CaptureFailureView view =
        PresentCaptureFailure(capture::TransferResult::kFileCreationError,
                              QString::fromStdString(open_error), QString());
    emit Failed(view.title, view.ToMessage());
    return nullptr;
  }

  // --pipe --save: the file is the capture, exactly as it would have been, and
  // the pipe is a copy of it that gives way to it — a reader that falls behind
  // loses blocks of the copy and never holds up the file. See PipeSink.
  if (pipe_stream_ != nullptr) {
    pipe_writer_ = std::make_shared<capture::PipeWriter>(
        std::move(pipe_stream_), conversion,
        capture::PipeWriter::WhenFull::kDrop);
    pipe_stream_.reset();
    sink = std::make_unique<capture::PipeSink>(std::move(sink), pipe_writer_);
    emit PipeNotice(DescribePipedStream(settings_.SampleRateHz(), range_2vpp,
                                        conversion, settings_.test_mode));
  }

  capture_path_ = QString::fromStdString(path.string());

  // What the sidecar will say about the setup this capture ran with. Taken now
  // rather than at the end because the player and the disc are cleared by the
  // automatic-capture coupling as soon as its run finishes, which is before the
  // encoder has finished writing this file.
  pending_metadata_ = capture::CaptureMetadata{};
  pending_metadata_.capture_file_name = path.filename().string();
  pending_metadata_.application_version = std::string(capture::Commit());
  pending_metadata_.format =
      settings_.output_format == capture::CaptureOutputFormat::kSigned16Bit
          ? "signed 16-bit"
          : "FLAC";
  pending_metadata_.test_mode = settings_.test_mode;
  pending_metadata_.decimation_factor = decimation;
  pending_metadata_.sample_rate_hz = settings_.SampleRateHz();
  pending_metadata_.input_range = capture::InputRangeName(range_2vpp);
  pending_metadata_.board.known = board_known;
  if (board_known) {
    pending_metadata_.board.declared = board_declared;
    pending_metadata_.board.name = board.name;
    pending_metadata_.board.adc = capture::AdcPartName(board.adc);
    pending_metadata_.board.rsel_wiring =
        capture::RselWiringName(board.rsel_wiring);
    pending_metadata_.board.dc_offset = dc_offset;
  }
  pending_metadata_.bit_shift = bit_shift;
  pending_metadata_.lsb_drop = lsb_drop;
  pending_metadata_.started = now;
  pending_metadata_.device = CurrentDeviceBuild();
  pending_metadata_.player = player_identity_;
  pending_metadata_.disc = disc_scan_;
  if (settings_.DeclaredGain().declared()) {
    pending_metadata_.front_end_gain =
        DescribeFrontEndGain(settings_.front_end_gain_switches).toStdString();
  }

  // The device's loss counters as they stand, so that what the sidecar reports
  // is what the device lost while writing this file rather than what it has
  // lost since monitoring began. The signal figures need no equivalent: the
  // engine measures those over a span of their own — see
  // SampleMetrics::BeginCaptureSpan.
  const capture::CaptureStats opening = pipeline_->stats().Read();
  device_overflows_at_start_ = opening.device_overflow_events;
  device_drops_at_start_ = opening.device_dropped_words;

  if (logger_ != nullptr) {
    logger_->Info("Capturing to " + path.string() + " (" + sink->Name() +
                  (decimation == capture::kUndecimatedFactor
                       ? ""
                       : ", " + std::to_string(decimation) + ":1 decimated") +
                  ")");

    // The setup this file was recorded under, in one line, so that a log read
    // afterwards does not have to be cross-referenced against a settings file
    // that has since been changed.
    logger_->Debug(
        "Capture settings: " + pending_metadata_.format + " at " +
        capture::FormatDecimal(
            static_cast<double>(pending_metadata_.sample_rate_hz) / 1.0e6, 3) +
        " Msps, ring " + capture::FormatBytes(settings_.queue_size_bytes) +
        ", test mode " + (settings_.test_mode ? "on" : "off") +
        ", input range " + capture::InputRangeName(range_2vpp) +
        ", DC offset " + std::to_string(dc_offset) + ", " + "bit shift " +
        std::to_string(bit_shift) + ", LSB drop " + std::to_string(lsb_drop) +
        ", duration limit " +
        (settings_.duration_limit_seconds > 0
             ? capture::FormatDuration(
                   static_cast<double>(settings_.duration_limit_seconds))
             : std::string("none")) +
        ", device buffer already " +
        std::to_string(opening.device_overflow_events) +
        " overflows into the "
        "session");

    // What the volume has, as the time it stands for. A capture that runs out
    // of disk is one of the few failures that is entirely predictable at the
    // moment it starts, and this is the line that makes it predictable
    // afterwards as well.
    const capture::FreeSpace space =
        capture::AvailableSpace(directory.toStdString());
    if (space.known) {
      logger_->Debug(
          "Destination has " + capture::FormatBytes(space.bytes_available) +
          " free, about " +
          capture::FormatDuration(capture::CaptureSecondsRemaining(
              space.bytes_available, settings_.EstimatedBytesPerSecond())) +
          " of capture at this setting");
    } else {
      logger_->Debug(
          "Destination free space could not be read, so nothing can be said "
          "about how long this capture will fit");
    }
  }
  return sink;
}

std::unique_ptr<capture::ISampleSink> CaptureController::OpenPipeOnlyCapture() {
  const bool range_2vpp = RunRange2Vpp();
  const capture::SampleConversion conversion = run_conversion_;

  // The pipe is the capture, so a reader that falls behind fails it rather
  // than thinning it out: the samples it would lose exist nowhere else.
  pipe_writer_ = std::make_shared<capture::PipeWriter>(
      std::move(pipe_stream_), conversion,
      capture::PipeWriter::WhenFull::kFail);
  pipe_stream_.reset();
  pipe_only_ = true;

  // Nothing is written to disk, so nothing is named, renamed or described in a
  // sidecar. The path stays empty, which is how everything after this knows.
  capture_path_.clear();

  // Only what the end of the run reads back: the rate, for the duration.
  pending_metadata_ = capture::CaptureMetadata{};
  pending_metadata_.sample_rate_hz = settings_.SampleRateHz();
  pending_metadata_.started = std::time(nullptr);

  const capture::CaptureStats opening = pipeline_->stats().Read();
  device_overflows_at_start_ = opening.device_overflow_events;
  device_drops_at_start_ = opening.device_dropped_words;

  const QString description = DescribePipedStream(
      settings_.SampleRateHz(), range_2vpp, conversion, settings_.test_mode);
  if (logger_ != nullptr) {
    logger_->Info("Capturing to standard output. " + description.toStdString());
  }
  emit PipeNotice(description);

  return std::make_unique<capture::PipeSink>(pipe_writer_);
}

void CaptureController::StartCapture() {
  if (capturing_ || measuring_dc_offset()) {
    return;
  }

  // One action rather than two. Someone who has not been monitoring and presses
  // Start capture means "capture", and making them start the stream first would
  // be ceremony.
  if (!monitoring_) {
    StartMonitoring();
    if (!monitoring_) {
      // StartMonitoring has already said why through Failed().
      return;
    }
  }

  std::unique_ptr<capture::ISampleSink> sink = OpenCaptureFile();
  if (sink == nullptr) {
    return;
  }

  low_space_warned_ = false;
  ticks_until_space_check_ = 0;

  pipeline_->AttachSink(std::move(sink));

  capturing_ = true;

  // Where the capture is going, for whoever shows it. A pipe-only capture has
  // no path, and saying where it goes is still the point of the signal.
  emit CapturingChanged(true,
                        pipe_only_ ? tr("standard output") : capture_path_);
}

void CaptureController::StopCapture() {
  if (!capturing_) {
    return;
  }

  // Detach rather than stop. The stream keeps running and the display keeps
  // moving while the encoder writes out its last frames and patches the header,
  // which is what makes taking several captures from one setup session possible
  // without reopening the device between them.
  pending_sink_change_ = pipeline_->DetachSink();

  capturing_ = false;
  emit CapturingChanged(false, QString());
}

void CaptureController::CollectFinishedCapture(
    const capture::CaptureStats& stats) {
  if (pending_sink_change_ == 0) {
    return;
  }
  if (pipeline_->SinkChangeCount() < pending_sink_change_) {
    return;
  }

  pending_sink_change_ = 0;

  // Read off the retired sink rather than off the statistics, and this is the
  // only place either figure survives: the published statistics report whatever
  // sink is attached now, which by this point is the null one, so both would
  // read zero.
  const std::unique_ptr<capture::ISampleSink> retired =
      pipeline_->TakeRetiredSink();
  const uint64_t bytes = retired != nullptr ? retired->BytesWritten() : 0;
  const uint64_t samples = retired != nullptr ? retired->SamplesWritten() : 0;

  FinishCaptureFile(stats, bytes, samples);
}

bool CaptureController::FinishPipe() {
  // Here rather than in the sink's Finish(), which runs on the processing
  // thread: a reader taking its time would be holding up the stream there.
  const bool finished = pipe_writer_->WaitUntilFinished(
      std::chrono::milliseconds(kPipeDrainMilliseconds));

  QString summary = tr("Standard output was sent %1 samples")
                        .arg(pipe_writer_->samples_delivered());
  if (pipe_writer_->samples_dropped() > 0) {
    summary += tr("; %1 were dropped while its reader was behind")
                   .arg(pipe_writer_->samples_dropped());
  }
  if (!finished) {
    summary += tr("; %1 were still waiting when the reader stopped taking them")
                   .arg(pipe_writer_->samples_queued());
  }
  summary += QStringLiteral(".");

  if (logger_ != nullptr) {
    logger_->Info(summary.toStdString());
  }
  emit PipeNotice(summary);

  // Beside a file, the copy coming up short is regrettable and nothing more.
  // Alone, it is the capture coming up short.
  return finished || !pipe_only_;
}

void CaptureController::FinishCaptureFile(const capture::CaptureStats& stats,
                                          uint64_t bytes, uint64_t samples) {
  if (pipe_writer_ != nullptr) {
    const bool delivered = FinishPipe();

    if (pipe_only_) {
      // Nothing on disk, so no rename and no sidecar: what the reader was sent
      // is the whole record of this capture, and it has just been said.
      const uint64_t sent =
          pipe_writer_->samples_delivered() * capture::kSigned16BytesPerSample;
      pipe_writer_.reset();

      if (logger_ != nullptr) {
        logger_->Info("Capture to standard output finished: " +
                      std::to_string(sent) + " bytes");
      }

      // Finished first and failed second, the order a file reports in, so
      // that a headless run's exit code comes out the same way for both.
      emit CaptureFinished(QString(), static_cast<quint64>(sent));
      if (!delivered) {
        emit Failed(
            tr("The capture did not all reach standard output"),
            tr("The program reading standard output stopped taking samples "
               "before the end of the capture, so what it received is "
               "incomplete."));
      }
      return;
    }

    pipe_writer_.reset();
  }

  // The length of what was recorded, worked out from the file's own contents
  // rather than from a clock. Samples divided by the rate they were written at
  // is exactly the duration of the recording, where an elapsed time would
  // include the encoder's final flush and, on the path where a run ends by
  // itself, whatever the device took to stop.
  const uint32_t rate = pending_metadata_.sample_rate_hz != 0
                            ? pending_metadata_.sample_rate_hz
                            : settings_.SampleRateHz();
  const double duration_seconds =
      rate == 0 ? 0.0
                : static_cast<double>(samples) / static_cast<double>(rate);

  std::filesystem::path file(capture_path_.toStdString());

  // The duration in the name, where the naming asks for it. Done here because
  // this is the first moment the duration is a fact, and by renaming rather
  // than by having guessed at the start.
  if (settings_.naming.append_duration && duration_seconds > 0.0) {
    const std::string suffix = capture::MatchedCaptureFileSuffix(file.string());
    const std::string base = capture::StripCaptureFileSuffix(file.string());
    const std::filesystem::path wanted(
        capture::AppendDurationToStem(base, duration_seconds) + suffix);

    // Made unique here as well, and for the same reason it was made unique
    // before the file was opened.
    //
    // The uniqueness settled at the open is about the name without the
    // duration, and this is a different name — so a capture that took the same
    // length as an earlier one of the same name arrives at a destination that
    // is already occupied. std::filesystem::rename replaces whatever is there
    // without a word, which would destroy a finished recording to tidy up a
    // file name. That is not a rare shape: it is exactly what a script that
    // captures with a fixed --capture-name and a fixed --duration-limit
    // produces every time it runs.
    const std::filesystem::path renamed =
        capture::MakeUniqueCapturePath(wanted);

    std::error_code error;
    std::filesystem::rename(file, renamed, error);
    if (error) {
      // Reported and then dropped. The recording is complete under the name it
      // already has, and refusing to finish a capture because a rename failed
      // would turn a cosmetic disappointment into a lost session.
      if (logger_ != nullptr) {
        logger_->Warning("The capture could not be renamed to " +
                         renamed.string() + ": " + error.message());
      }
    } else {
      if (renamed != wanted) {
        // The same thing the open says when it happens there, and it has to be
        // said here too: the file is not the one the naming asked for, and
        // nobody watching would otherwise know.
        emit CaptureRenamed(
            QString::fromStdString(
                capture::StripCaptureFileSuffix(wanted.filename().string())),
            QString::fromStdString(
                capture::StripCaptureFileSuffix(renamed.filename().string())));
      }

      file = renamed;
      capture_path_ = QString::fromStdString(file.string());
      pending_metadata_.capture_file_name = file.filename().string();
    }
  }

  WriteMetadataSidecar(file, stats, bytes, samples, duration_seconds);

  if (logger_ != nullptr) {
    logger_->Info("Capture finished: " + file.string() + ", " +
                  std::to_string(bytes) + " bytes");

    // The file's own account, measured across the span it covers rather than
    // across the session it sat in — the same figures the sidecar records, said
    // where somebody debugging will see them without opening it.
    const uint64_t raw_bytes = samples * capture::kBytesPerSample;
    std::string written = "Capture file: " + std::to_string(samples) +
                          " samples over " +
                          capture::FormatDuration(duration_seconds) + ", " +
                          capture::FormatBytes(bytes);
    if (raw_bytes > 0) {
      written += " of " + capture::FormatBytes(raw_bytes) + " that arrived (" +
                 capture::FormatDecimal(100.0 * static_cast<double>(bytes) /
                                            static_cast<double>(raw_bytes),
                                        1) +
                 "%)";
    }
    if (duration_seconds > 0.0) {
      written += ", " +
                 capture::FormatDecimal(
                     static_cast<double>(bytes) / duration_seconds / 1.0e6, 1) +
                 " MB/s to disk";
    }
    logger_->Debug(written);

    if (stats.metrics.capture_sample_count > 0) {
      logger_->Debug("Signal in the file: range " +
                     std::to_string(stats.metrics.capture_minimum_value) + "-" +
                     std::to_string(stats.metrics.capture_maximum_value) +
                     " of 1023, "
                     "RMS " +
                     capture::FormatDecimal(stats.metrics.capture_rms, 1) +
                     ", clipped low " +
                     std::to_string(stats.metrics.capture_clipped_low_count) +
                     " high " +
                     std::to_string(stats.metrics.capture_clipped_high_count));
    }

    // Differences rather than totals, for the reason the sidecar takes
    // differences: the pipeline's counters run for the whole session and what
    // matters here is what this file cost.
    logger_->Debug("While this file was open: device lost " +
                   std::to_string(Since(stats.device_dropped_words,
                                        device_drops_at_start_)) +
                   " samples in " +
                   std::to_string(Since(stats.device_overflow_events,
                                        device_overflows_at_start_)) +
                   " overflows; session peak back pressure " +
                   std::to_string(stats.peak_back_pressure_percent) +
                   "%, session peak ring depth " +
                   std::to_string(stats.peak_slots_in_use) + " of " +
                   std::to_string(stats.slot_count) + " slots");
  }

  emit CaptureFinished(capture_path_, static_cast<quint64>(bytes));
}

void CaptureController::WriteMetadataSidecar(
    const std::filesystem::path& capture_file,
    const capture::CaptureStats& stats, uint64_t bytes, uint64_t samples,
    double duration_seconds) {
  capture::CaptureMetadata metadata = pending_metadata_;

  // The naming fields as they are now rather than as they were at the start.
  // The file's name was settled when it was opened and cannot change, but what
  // is *said* about the disc can: somebody who types a note while watching a
  // capture means it to describe that capture.
  metadata.naming = settings_.naming;
  metadata.finished = std::time(nullptr);

  metadata.outcome.completed = !capture::TransferFailed(stats.result);
  if (!metadata.outcome.completed) {
    metadata.outcome.detail =
        std::string(capture::TransferResultDescription(stats.result));
  }
  metadata.outcome.duration_seconds = duration_seconds;
  metadata.outcome.samples = samples;
  metadata.outcome.bytes = bytes;

  // Differences, because the pipeline's device counters run for the whole
  // session and what belongs in a file's metadata is what the device lost while
  // that file was being written. Nothing about ring depth or back pressure is
  // recorded at all — see CaptureOutcome, where the line between the two is
  // drawn.
  metadata.outcome.device_overflow_events =
      Since(stats.device_overflow_events, device_overflows_at_start_);
  metadata.outcome.device_dropped_words =
      Since(stats.device_dropped_words, device_drops_at_start_);

  // The validator's own word for how it ended, rather than a boolean derived
  // from it — see CaptureOutcome::sequence_check, where the reason "disabled"
  // cannot be folded into "intact" is set out, and why the session-long check
  // is nonetheless a statement about this file.
  metadata.outcome.sequence_check =
      capture::SequenceStateName(stats.sequence_state);

  // Derived rather than copied. The pipeline's flag says the ramp was checked
  // somewhere in the session, which for a file's own metadata is the wrong
  // question: in test mode every buffer is checked, so a test capture with
  // samples in it is a test capture that was checked.
  metadata.outcome.test_pattern_checked = metadata.test_mode && samples > 0;
  metadata.outcome.test_pattern_passed = stats.test_pattern_passed;

  // Measured over the file's own samples by a span the engine opens and closes
  // with the file — not the session-long accumulators the Statistics panel
  // reads. See SampleMetrics::BeginCaptureSpan.
  metadata.signal.known = stats.metrics.capture_sample_count > 0;
  metadata.signal.minimum_value = stats.metrics.capture_minimum_value;
  metadata.signal.maximum_value = stats.metrics.capture_maximum_value;
  metadata.signal.rms = stats.metrics.capture_rms;
  metadata.signal.clipped_low_samples = stats.metrics.capture_clipped_low_count;
  metadata.signal.clipped_high_samples =
      stats.metrics.capture_clipped_high_count;
  metadata.board.offset_saturated_samples =
      stats.metrics.capture_offset_saturated_count;
  metadata.signal.shift_clipped_samples =
      stats.metrics.capture_shift_clipped_count;

  const std::filesystem::path sidecar =
      capture::CaptureMetadataPath(capture_file);

  std::string error;
  if (capture::WriteCaptureMetadataFile(sidecar, metadata, error)) {
    if (logger_ != nullptr) {
      logger_->Info("Capture metadata written to " + sidecar.string());
    }
    return;
  }

  // Said, and then let go. The capture is on disk and is complete; a failure to
  // write a text file beside it is worth knowing about and is not a reason to
  // tell somebody their recording went wrong.
  if (logger_ != nullptr) {
    logger_->Warning(error);
  }
  emit MetadataWriteFailed(QString::fromStdString(error));
}

void CaptureController::CheckDurationLimit(const capture::CaptureStats& stats) {
  if (!capturing_ || settings_.duration_limit_seconds <= 0) {
    return;
  }

  // Read every tick rather than latched at the start, so that changing the
  // limit mid-capture takes effect. Unlike the engine settings beside it there
  // is nothing here that cannot be changed under a running stream: this is a
  // number compared against a counter, not a ring that would have to be
  // reallocated.
  //
  // At the rate samples reach the file, because samples_written counts what
  // reached the file rather than what came off the device: a 2:1 capture puts
  // half as many samples in a file per second of signal, and a limit that
  // ignored that would run for twice as long as it was asked to. The same goes
  // for the converter's own rate - a limit worked out from the default 40 MHz
  // stopped a 75 MHz capture a little past halfway.
  const uint64_t limit_samples =
      static_cast<uint64_t>(settings_.duration_limit_seconds) *
      settings_.SampleRateHz();

  if (stats.samples_written < limit_samples) {
    return;
  }

  // Checked here rather than on the processing thread, so the overshoot is
  // bounded by the statistics interval and the buffer in flight — about 50 ms,
  // or 4 MB at the device's rate — rather than being exact. That is the right
  // trade: an exact limit would mean putting a GUI policy decision on the
  // real-time path, and the whole design of this application is that nothing
  // the GUI does can cost a sample.

  // Counted in samples the sink accepted, and a sink only ever receives whole
  // buffers — so the stop lands on a buffer boundary by construction rather
  // than by a timer firing somewhere in the middle of one.
  if (logger_ != nullptr) {
    logger_->Info("Duration limit reached after " +
                  std::to_string(stats.samples_written) + " samples");
  }
  StopCapture();
}

void CaptureController::CheckPipe() {
  if (!capturing_ || pipe_writer_ == nullptr) {
    return;
  }

  if (pipe_writer_->closed() && !pipe_closed_reported_) {
    pipe_closed_reported_ = true;

    if (pipe_only_) {
      // Capturing for nobody. Stopped the way any capture is stopped, so that
      // ending the program at the far end of the pipe is an ordinary way to
      // end the run rather than a failure of it.
      const QString message =
          tr("The program reading standard output has closed it, so the "
             "capture is stopping.");
      if (logger_ != nullptr) {
        logger_->Info(message.toStdString());
      }
      emit PipeNotice(message);
      StopCapture();
      return;
    }

    const QString message =
        tr("The program reading standard output has closed it. The capture to "
           "%1 carries on.")
            .arg(capture_path_);
    if (logger_ != nullptr) {
      logger_->Warning(message.toStdString());
    }
    emit PipeNotice(message);
  }

  // Said once, when it starts. How much was lost in all is said at the end.
  if (!pipe_drops_reported_ && pipe_writer_->samples_dropped() > 0) {
    pipe_drops_reported_ = true;
    const QString message =
        tr("The program reading standard output is not keeping up, so its "
           "copy is losing blocks. The capture file is not affected.");
    if (logger_ != nullptr) {
      logger_->Warning(message.toStdString());
    }
    emit PipeNotice(message);
  }
}

void CaptureController::CheckFreeSpace() {
  // A pipe-only capture puts nothing on any volume.
  if (!capturing_ || low_space_warned_ || pipe_only_ ||
      settings_.low_space_warning_minutes <= 0) {
    return;
  }

  if (ticks_until_space_check_ > 0) {
    --ticks_until_space_check_;
    return;
  }
  ticks_until_space_check_ = kSpaceCheckIntervalTicks;

  const capture::FreeSpace space = capture::AvailableSpace(
      settings_.ResolvedCaptureDirectory().toStdString());
  if (!space.known) {
    return;
  }

  const double seconds_left = capture::CaptureSecondsRemaining(
      space.bytes_available, settings_.EstimatedBytesPerSecond());
  const double threshold =
      static_cast<double>(settings_.low_space_warning_minutes) * 60.0;
  if (seconds_left >= threshold) {
    return;
  }

  low_space_warned_ = true;

  // A warning, and only a warning. The capture is not stopped and the estimate
  // is an estimate — real RF compresses better than the figure used here, so
  // an application that halted on this prediction would sometimes end a good
  // capture early.
  emit LowSpaceWarning(
      tr("The destination volume has about %1 of capture left. The capture is "
         "still running; free some space or it will stop when the volume "
         "fills.")
          .arg(FormatElapsed(seconds_left)));
}

void CaptureController::Tick() {
  const capture::CaptureStats stats = pipeline_->stats().Read();
  emit StatsUpdated(stats);

  CheckDcOffsetSaturation(stats);
  CheckBitShiftClipping(stats);
  CheckDurationLimit(stats);
  CheckFreeSpace();
  CheckPipe();
  CollectFinishedCapture(stats);

  // The pipeline stops on its own schedule: a requested stop still has to drain
  // the ring and finalise whatever sink is attached. Noticing here rather than
  // waiting for it is what keeps the window responsive while that happens.
  if (monitoring_ && !pipeline_->Running()) {
    FinishRun();
  }
}

void CaptureController::FinishRun() {
  stats_timer_.stop();
  monitoring_ = false;

  // A capture that was still running when the stream stopped. The pipeline has
  // already finished the sink — it does that after joining both workers,
  // however the run ended — so the file on disk is closed and readable. What is
  // left is to say so, and to say it before the failure below, so that the
  // panels are consistent by the time a message box takes over the event loop.
  const bool was_capturing = capturing_;
  const capture::CaptureStats final_stats = pipeline_->stats().Read();

  if (was_capturing) {
    capturing_ = false;
    emit CapturingChanged(false, QString());
  }
  pending_sink_change_ = 0;

  // First, and before the pipeline is touched: the next run builds a new
  // snapshot publisher, and this is what guarantees nothing is reading the old
  // one when it goes.
  analysis_->Stop();

  // Returns immediately — Running() is already false — and joins the threads.
  pipeline_->Wait();

  const capture::TransferResult result = pipeline_->Result();
  const std::string detail = pipeline_->ResultDetail();

  // Finish() has already been called by the pipeline; this only releases the
  // object.
  source_.reset();

  // After the streaming handle is released, so the request is not a second
  // opener competing with it. This hands U1/U2 back to the USB driver, which
  // the firmware needs for compliance once a capture is no longer relying on
  // the link staying awake. Failure is not reported: the run is already over,
  // the device restores this on reset and disconnect anyway, and a message box
  // about it would arrive on top of whatever ended the capture.
  device_->SetCollecting(settings_.preferred_device_path.toStdString(), false,
                         capture::kCollectionFlagsNone);

  if (monitor_ != nullptr) {
    monitor_->SetSuspended(false);
  }

  emit StatsUpdated(pipeline_->stats().Read());
  emit MonitoringChanged(false);

  if (was_capturing) {
    // The figures come from the statistics rather than from a retired sink,
    // because on this path the sink was never detached: the pipeline finished
    // it on its way out, with the counters still attached to it.
    //
    // The result is taken after the join rather than from the snapshot, so that
    // a run which ended in a failure records the failure rather than whatever
    // had been published a fiftieth of a second before it.
    capture::CaptureStats closing = final_stats;
    closing.result = result;

    FinishCaptureFile(closing, final_stats.bytes_written,
                      final_stats.samples_written);
  }

  if (capture::TransferFailed(result)) {
    // capture_path_ rather than the path the file was opened under: a capture
    // whose naming asks for the duration has just been renamed, and a message
    // naming the old path would send somebody to a file that is not there.
    CaptureFailureView view = PresentCaptureFailure(
        result, ToQString(detail), was_capturing ? capture_path_ : QString());

    // With the pipe alone the only thing written to is the pipe, so a write
    // failure is its reader falling behind — and the remedy for a full disk
    // would send somebody to look at the wrong thing.
    if (was_capturing && pipe_only_ &&
        result == capture::TransferResult::kFileWriteError) {
      view.remedy =
          tr("Read standard output with something that keeps up with the "
             "device, or add --save so that the capture goes to a file and "
             "the pipe carries a copy of it that is allowed to fall behind.");
    }
    emit Failed(view.title, view.ToMessage());
  }
}

}  // namespace ddd::gui
