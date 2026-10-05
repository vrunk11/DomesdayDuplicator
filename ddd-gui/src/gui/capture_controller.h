/************************************************************************

    capture_controller.h

    The bridge between the GUI and the capture engine
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

#include "analysis_worker.h"
#include "board_setup.h"
#include "byte_stream.h"
#include "capture_metadata.h"
#include "capture_metatypes.h"
#include "capture_pipeline.h"
#include "capture_provenance.h"
#include "capture_settings.h"
#include "dc_offset_measurement.h"
#include "device_monitor.h"
#include "flac_sink.h"
#include "fpga_version.h"
#include "monitor_tap.h"
#include "pipe_writer.h"
#include "requantizing_sink.h"
#include "rf_requantizer.h"
#include "usb_device.h"
#include "usb_device_info.h"

namespace ddd::gui {

// Owns the engine on the GUI's behalf, and is the only place the two meet.
//
// Everything below it is Qt-free and everything above it is Qt. That is what
// keeps the rule in AGENTS.md honest rather than aspirational: there is exactly
// one file where a QObject and a std::thread are in scope together, and it is
// this one.
//
// Nothing here blocks. The pipeline runs on its own threads and is asked
// politely to stop; a timer notices when it has, which is what lets a stop take
// as long as finalising a file needs to take without the window freezing. The
// statistics come from the wait-free publisher, so polling them cannot slow the
// capture down however often this asks.
//
// The device backend is borrowed rather than owned, so a test can supply one
// that reports whatever devices it likes and hands back a synthetic source.
// With that, everything in this class — including the parts a user drives — is
// testable on a machine with nothing plugged in.
class CaptureController : public QObject {
  Q_OBJECT

 public:
  CaptureController(capture::IUsbDevice* device, capture::ILogger* logger,
                    QObject* parent = nullptr);
  ~CaptureController() override;

  // Begin watching for devices. Separate from the constructor so that a caller
  // can connect to the signals first and not miss the initial report.
  void Start();

  bool monitoring() const { return monitoring_; }

  // Whether a writer is attached. Always implies monitoring(): a capture is the
  // same stream with a sink on the end of it.
  bool capturing() const { return capturing_; }

  // The file the current capture is being written to, or the last one written.
  // Empty until the first capture of the session.
  QString capture_path() const { return capture_path_; }

  std::vector<capture::DeviceInfo> devices() const { return devices_; }

  // What the selected device's gateware last said about itself.
  //
  // Read when a device appears rather than on demand, so that showing it costs
  // nothing and never blocks. Default constructed — present false — when no
  // device is selected or its gateware could not answer.
  const capture::FpgaVersion& fpga_version() const { return fpga_version_; }

  // The fastest ADC rate, in MHz, a capture may ask this board for: what its
  // gateware says it can drive — MAX_ADC_RATE_MHZ, read alongside the identity
  // block — capped by the converter the board setup declares. The gateware's
  // figure is about the build and the declaration is about the part soldered
  // to the board, and an ADS825 run at 75 MHz produces samples nothing can
  // tell are wrong, so the lower of the two is the only safe one.
  //
  // 0 for "not known": no device, gateware predating the register, or an FPGA
  // that has not answered, which is indistinguishable from the register's own
  // point of view and is why the register interface documentation says to
  // treat a 0 reading as unknown rather than as a literal claim about a
  // zero-MHz converter. CapturePanel is what turns this into which presets
  // are offered.
  uint8_t max_adc_rate_mhz() const;

  // What the device holds about the capture board it is plugged into, as last
  // read — or the conservative defaults, with the source saying why. See
  // board_setup.h: this is a declaration, not a capture setting, and the
  // capture settings are bounded by it.
  const capture::BoardSetupReading& board_setup() const { return board_setup_; }

  // Whether the declared board lets a capture choose its input range — only
  // when RSEL is routed to the FPGA.
  bool input_range_selectable() const {
    return capture::InputRangeIsSelectable(board_setup_.setup);
  }

  // The input range a capture actually runs at: the setting's, where RSEL is
  // routed, and the wired one where it is not.
  bool effective_range_2vpp() const {
    return capture::EffectiveRange2Vpp(board_setup_.setup,
                                       settings_.range_select_2vpp);
  }

  // Write a declaration to the device, confirm it by reading it back, and put
  // it in force. Refused while monitoring: the stream holds the device open,
  // and on Windows a second opener is refused outright.
  //
  // A device whose firmware predates the record cannot store it; the
  // declaration is then put in force for this session only, and the return is
  // still true with `message` saying so. False means nothing changed, with
  // `message` saying why.
  bool WriteBoardSetup(const capture::BoardSetup& setup, QString& message);

  // What the running stream's writers do to every sample — the DC offset and
  // the bit shift — fixed when the stream starts, and the converter
  // untouched in test mode or while measuring. The requantiser, when there is
  // one, comes after this and is not part of it. What the scope
  // applies when asked to show the signal as it is written, and what every
  // file opened during the run is written with.
  const capture::SampleConversion& run_conversion() const {
    return run_conversion_;
  }

  // The DC offset part of it, in converter codes.
  int32_t run_dc_offset() const { return run_conversion_.dc_offset; }

  // What the requantiser in front of the stream's writer has in force: the
  // capture's own while one is written, a preview of what a capture would do
  // while monitoring. Empty when none is running — requantisation off, test
  // mode, a measurement, or no stream. See RequantizationUpdated.
  std::optional<capture::RequantizationStatus::Live> requantization() const;

  // Whether that is a capture's rather than a preview.
  bool requantization_applies() const {
    return requantization_status_ != nullptr && capturing_;
  }

  // Whether the signal panels show the signal as it is written — converted by
  // run_conversion() — or as the converter produced it. One answer for every
  // panel, so that the scope, the spectrum and the amplitude history are
  // never showing two different signals side by side. Off by default, and
  // not saved: it is a way of looking, not a setting.
  bool show_corrected() const { return show_corrected_; }
  void SetShowCorrected(bool show);

  // The ADC rate, in MHz, the capture settings run at — "board default" taken
  // as the 40 MHz every figure assumes for it. The rate whose offset applies.
  uint8_t configured_rate_mhz() const {
    return settings_.pll_preset_mhz != 0 ? settings_.pll_preset_mhz
                                         : capture::kPllPreset40Mhz;
  }

  // Whether a DC offset measurement is running, and how many runs it has.
  bool measuring_dc_offset() const {
    return measure_phase_ != MeasurePhase::kIdle;
  }
  size_t dc_offset_measurement_runs() const { return measure_steps_.size(); }

  const CaptureSettings& settings() const { return settings_; }

  // The USB backend, borrowed. Exposed so that the update flow can open the
  // same device this is watching, through the same backend, rather than
  // starting a second one — two libusb contexts enumerating the same bus is
  // a way to get two different answers about what is attached.
  capture::IUsbDevice* usb_device() const { return device_; }

  // Stop enumerating while something else owns the device.
  //
  // The device monitor opens every attached device to read its identity, and
  // an update holds one open for minutes. Enumerating underneath that is at
  // best wasted work and at worst a second claim on an interface that is
  // being written to — and the device disappears and comes back during an
  // update anyway, so the monitor's report would be noise a user should not
  // be shown.
  void SetDeviceMonitorSuspended(bool suspended);

  // The signal panels' source of waveform and spectrum frames. Owned here
  // rather than by a panel because it is tied to the run rather than to any one
  // display: it is attached when a run starts and detached when it ends, and
  // three panels share what it produces.
  AnalysisWorker* analysis() { return analysis_.get(); }

  // What was in the player when this capture was set up.
  //
  // Set by the automatic-capture coupling and by nothing else, and cleared when
  // there is no longer a disc it describes. It reaches the file the next
  // capture opens rather than the one that is running: a file's tags are
  // written into its header when it is created, so a fact that arrived
  // afterwards has nowhere to go.
  void SetDiscProvenance(const capture::DiscProvenance& disc);
  const capture::DiscProvenance& disc_provenance() const {
    return disc_provenance_;
  }

  // Who the player was, for the sidecar.
  //
  // Set whenever the link comes up or goes down, and so present for a capture
  // taken by hand as well as for an automatic one: a manual capture of a disc
  // in a player is still a capture whose provenance includes which player it
  // came off.
  void SetPlayerIdentity(const capture::PlayerIdentity& player);
  const capture::PlayerIdentity& player_identity() const {
    return player_identity_;
  }

  // What the examination of the disc found, for the sidecar.
  //
  // Set by the automatic-capture coupling on the same terms as the disc
  // provenance above, and cleared with it. A capture taken by hand some time
  // after an examination carries no scan rather than the previous disc's — the
  // disc in the player is not necessarily the disc that was examined, and a
  // file asserting otherwise would be worse than one that says nothing.
  void SetDiscScan(const capture::DiscScan& disc);
  const capture::DiscScan& disc_scan() const { return disc_scan_; }

  // Applying settings while a capture is running changes what the next one will
  // do, not this one. Nothing here can be changed mid-stream without stopping,
  // and pretending otherwise would mean a ring that was resized underneath a
  // running transfer.
  void SetSettings(const CaptureSettings& settings);

  // The same, without saving them.
  //
  // What the command line names applies to the run it was given to and is then
  // forgotten: a script that captures one disc at 20 Msps has not asked for
  // every capture afterwards to be taken at 20 Msps, and SetSettings() above
  // would have made that the user's new saved answer. The window is populated
  // from what this sets, so a capture set up from a script and then taken by
  // hand still runs with what the script asked for — and if the user then edits
  // any of it in the panel, that edit saves in the ordinary way, because at
  // that point it is their choice rather than the script's.
  void ApplySessionSettings(const CaptureSettings& settings);

  // Stream the next capture to `stream` — --pipe. With `save_file` the capture
  // is written to its file exactly as it would be otherwise and the stream
  // carries a copy that gives way to it; without, the stream is the capture
  // and no file, sidecar or rename happens at all. See PipeSink.
  //
  // Used once. The capture after that one is an ordinary capture: a stream is
  // one recording with a beginning and an end, and the program reading it has
  // been told it is finished by the time a second could start.
  void SetPipeOutput(std::shared_ptr<capture::IByteStream> stream,
                     bool save_file);

  // Whether the running capture, or the last one, went to a stream and to
  // nothing else.
  bool pipe_only() const { return pipe_only_; }

  // How long the end of a piped capture waits for the reader to take what is
  // still queued for it. Waited on this thread rather than on the processing
  // thread, where a slow reader would hold up the stream. A reader that can
  // keep up at all empties a full queue in well under a second.
  static constexpr int kPipeDrainMilliseconds = 5000;

  // How often the statistics are republished to the panels. 20 Hz: fast enough
  // that a throughput reading looks live, slow enough that it is nowhere near
  // the cost of anything else the application does.
  static constexpr int kStatsIntervalMilliseconds = 50;

  // How long StartMonitoring() waits after writing a changed PLL_PRESET
  // before writing anything else or opening the stream — see the comment
  // there. A conservative placeholder pending a real measurement of the
  // gateware's reconfigure-and-relock time on hardware, not a measured
  // figure itself.
  static constexpr int kPllPresetSettleMilliseconds = 50;

 public slots:
  // Open the device and start streaming with no sink attached. This is monitor
  // mode: the signal is validated, measured and published, and nothing is
  // written anywhere.
  void StartMonitoring();

  // Stop at the next buffer boundary. Returns immediately; MonitoringChanged
  // follows when the pipeline has actually stopped.
  void StopMonitoring();

  // Attach a writer, so the stream starts reaching a file.
  //
  // Starts monitoring first if nothing is running, so that a user who has not
  // been monitoring gets one action rather than two. From an existing monitor
  // session the sink is attached at the next buffer boundary and the stream is
  // not interrupted — the device never knows a capture began.
  void StartCapture();

  // Detach the writer and finalise the file, leaving the stream running.
  //
  // Stop returns to monitoring rather than to idle, which is what makes taking
  // several captures from one setup session possible without reopening the
  // device between them.
  void StopCapture();

  // Measure the board's DC offset with nothing connected to its input: at each
  // ADC rate up to `max_rate_mhz` that the gateware can drive, and for each
  // input range in `ranges` (true for 2Vpp), start the stream, let it settle,
  // average a second of it, and stop.
  //
  // Empty `ranges` means each range the declared wiring can select, and a zero
  // `max_rate_mhz` the declared converter's rating; the Board setup page passes
  // what it is showing, which may not have been written yet. Eight rates and
  // two ranges is sixteen runs, a little over twenty seconds.
  //
  // Refused while monitoring, for the reason WriteBoardSetup is. Nothing is
  // declared by this: each result arrives through DcOffsetMeasured for the
  // Board setup page to show, and it reaches the device only when that page
  // writes it. DcOffsetMeasurementFinished follows once, however it ended.
  void MeasureDcOffset(const std::vector<bool>& ranges = {},
                       uint8_t max_rate_mhz = 0);

 signals:
  // The board setup changed — read off a device that appeared, written, or
  // cleared because the device went away. Read it with board_setup().
  void BoardSetupChanged();

  // One run's measurement: the range, the ADC rate in MHz, and the offset in
  // converter codes.
  void DcOffsetMeasured(bool range_2vpp, int rate_mhz, int offset);

  // The measurement is over. `message` says why when it did not succeed.
  void DcOffsetMeasurementFinished(bool succeeded, const QString& message);

  // The DC offset correction pushed samples out of range that the converter
  // had not clipped. With an offset measured on this board that cannot happen
  // on its own, so it says the declaration is wrong. Raised once per run.
  void DcOffsetOutOfRange(const QString& message);

  // Every panel's Corrected switch follows this.
  void ShowCorrectedChanged(bool show);

  // run_conversion() changed while the stream ran: the bit shift was changed
  // while monitoring, which takes effect at once rather than at the next
  // start. Never while a file is being written.
  void ConversionChanged();

  // requantization() may read differently: raised with the statistics while
  // one runs, and once when one starts or stops.
  void RequantizationUpdated();

  // The bit shift clipped samples that neither the converter nor the DC
  // offset had: the shift is too large for this signal. A setting rather than a
  // fault, so it is said once per run and nothing is stopped.
  void BitShiftClipping(const QString& message);

  void DevicesChanged(const std::vector<ddd::capture::DeviceInfo>& devices);
  void MonitoringChanged(bool monitoring);
  void StatsUpdated(const ddd::capture::CaptureStats& stats);

  // A writer was attached or detached. The path is the file being written, or
  // empty when the capture has just ended.
  void CapturingChanged(bool capturing, const QString& file_path);

  // The requested name was already taken, so the capture was written under
  // another. Not a failure — nothing was overwritten — but a fact the user has
  // to be told, because the file is not the one they named.
  //
  // Emitted at either of the two moments a capture is named: before the file
  // is opened, and again at the end if the naming appends the capture's length
  // and *that* name is taken as well. The second is what a script produces
  // every run when it captures with a fixed name and a fixed duration limit,
  // so it is not the unlikely half of this.
  void CaptureRenamed(const QString& requested, const QString& written);

  // A capture finished and its file is closed. `bytes` is what reached the
  // disk, which is not derivable from the sample count once a compressor is in
  // the path.
  //
  // The path is where the file ended up, which is not necessarily where it was
  // opened: a capture whose naming asks for the duration in its name is renamed
  // at this point, since the duration is not a fact until the capture has
  // stopped.
  void CaptureFinished(const QString& file_path, quint64 bytes);

  // The sidecar could not be written. Not a capture failure — the recording is
  // on disk and complete — so it is reported separately and never as an error
  // box, which would send somebody looking for a fault in the wrong place.
  void MetadataWriteFailed(const QString& detail);

  // The destination volume has less space left than the warning threshold.
  // Raised once per capture: a warning that repeated every two seconds for the
  // rest of a disc side would be ignored, which is worse than not warning.
  void LowSpaceWarning(const QString& message);

  // Something about the stream a piped capture is going to: its reader left,
  // fell behind, or what it was sent in the end. For whoever is watching the
  // run — a headless one says it on standard error — and never an error box,
  // because none of it is a fault in the capture itself. A piped capture that
  // did fail says so through Failed() as any other does.
  void PipeNotice(const QString& message);

  // The settings changed. Emitted for the panels rather than for the engine:
  // the front-end gain declaration is a display calibration, so a panel that
  // has already drawn a level in converter codes has to be told to draw it
  // again in millivolts without anything being re-acquired.
  void SettingsChanged(const CaptureSettings& settings);

  // A device whose firmware build differs from this application's. Raised once
  // per connection and never blocking — see firmware_version.h.
  void FirmwareWarning(const QString& message);

  // Something went wrong, with a short title and the sentence explaining it.
  void Failed(const QString& title, const QString& detail);

 private:
  void OnDevicesChanged(const std::vector<capture::DeviceInfo>& devices);
  void CheckFirmware(const std::vector<capture::DeviceInfo>& devices);

  // Read and parse the gateware identity block from the device at `path`.
  capture::FpgaVersion ReadFpgaVersion(const std::string& path);

  // Read MAX_ADC_RATE_MHZ from the device at `path`, or 0 if it could not be
  // read — see max_adc_rate_mhz().
  uint8_t ReadMaxAdcRateMhz(const std::string& path);

  // Bring the rate setting inside what max_adc_rate_mhz() allows, for this
  // session. Run whenever either of the two things it depends on changes.
  void ApplyBoardLimits();

  // The input range the next run streams at: a measurement's forced one, or
  // effective_range_2vpp().
  bool RunRange2Vpp() const;

  // The ADC rate, in MHz, the next run streams at: a measurement's forced one,
  // or the setting's, with "board default" taken as 40.
  uint8_t RunRateMhz() const;

  // The DC offset the next run's writers take out, in converter codes: the
  // declared one for its range, and 0 in test mode or while measuring.
  int32_t RunDcOffset() const;

  // The whole conversion the next run's writers apply: RunDcOffset(), and the
  // settings' bit shift — not in test mode or while measuring.
  capture::SampleConversion RunConversion() const;

  // Whether the running stream is requantised, and with what: the settings'
  // margin, at the rate the file is written at and from the bits the bit
  // shift leaves. Never in test mode or while measuring, as the conversion.
  bool RunRequantizes() const;
  capture::RequantizerSettings RunRequantizerSettings() const;

  // The sink a stream has while nothing is being written: a preview of the
  // requantiser in front of nothing when it is on, nothing otherwise.
  std::unique_ptr<capture::ISampleSink> MakeIdleSink();

  // A capture's sink with the requantiser in front of it, when it is on — and
  // what it was asked to do, in the record the file and its sidecar carry.
  std::unique_ptr<capture::ISampleSink> RequantizeCapture(
      std::unique_ptr<capture::ISampleSink> sink);
  capture::RequantizationRecord RequantizationSettingsRecord() const;

  // Replace the idle sink with one built from the settings as they now are,
  // when they changed what it would be. Not while a finished capture is still
  // waiting to be collected: the pipeline keeps one retired sink, and that
  // one is the capture's.
  void UpdateIdleSink();

  // Bring run_conversion() up to the settings while monitoring and not
  // capturing: the pipeline counts against it from the next buffer and the
  // panels redraw with it. A file being written keeps the conversion it was
  // opened with, so nothing changes while one is.
  void UpdateRunConversion();

  // Tell the analysis worker what the panels are to be shown: the run's
  // conversion when show_corrected(), and the converter's own codes otherwise.
  void ApplyDisplayConversion();

  // One step of the DC offset measurement, from measure_timer_.
  void MeasurementStep();
  void FinishMeasurement();

  // Raise DcOffsetOutOfRange the first time a run's statistics show it.
  void CheckDcOffsetSaturation(const capture::CaptureStats& stats);

  // Raise BitShiftClipping the first time a run's statistics show it.
  void CheckBitShiftClipping(const capture::CaptureStats& stats);

  // What the device this capture is coming off was built from, for the file's
  // own tags and for the sidecar beside it.
  //
  // Assembled from what was already read when the device appeared rather than
  // by asking it again: a capture is about to start, and the device is about
  // to be opened for the stream. Whatever could not be established is left
  // empty, which is what both writers treat as "say nothing".
  capture::DeviceBuild CurrentDeviceBuild() const;
  void Tick();
  void FinishRun();

  // Build and open the file for a new capture, in whichever format the settings
  // ask for — with a pipe beside it, or a pipe instead of it, when --pipe
  // asked. Returns null with the reason already reported through Failed().
  std::unique_ptr<capture::ISampleSink> OpenCaptureFile();

  // The pipe-only half of OpenCaptureFile(): nothing on disk at all.
  std::unique_ptr<capture::ISampleSink> OpenPipeOnlyCapture();

  // Notice the reader of a piped capture leaving or falling behind, and say so
  // — stopping the capture when the pipe was all it was.
  void CheckPipe();

  // The end of a piped capture: wait for the reader to take what is queued,
  // and say what it got. False if a pipe-only capture could not be delivered
  // in full, which is that capture failing.
  bool FinishPipe();

  // Notice that the writer has been detached and the file closed, and report
  // it. Called from Tick() rather than from StopCapture(), because finalising a
  // FLAC stream happens on the processing thread and takes as long as it takes.
  void CollectFinishedCapture(const capture::CaptureStats& stats);

  // Everything that happens once a capture's file is closed: the duration
  // rename where the naming asks for one, the sidecar, and the signal that
  // says so.
  //
  // One function for the three because they have to happen in that order and
  // share the path they act on — a sidecar written before the rename would be
  // orphaned by it, and a signal carrying the old path would name a file that
  // is no longer there.
  void FinishCaptureFile(const capture::CaptureStats& stats, uint64_t bytes,
                         uint64_t samples);

  // Write the sidecar beside `capture_file`, and report a failure without
  // treating it as one.
  void WriteMetadataSidecar(const std::filesystem::path& capture_file,
                            const capture::CaptureStats& stats, uint64_t bytes,
                            uint64_t samples, double duration_seconds);

  // Stop the capture because the duration limit has been reached.
  void CheckDurationLimit(const capture::CaptureStats& stats);

  // Warn once if the destination volume is running out.
  void CheckFreeSpace();

  capture::IUsbDevice* device_ = nullptr;
  capture::ILogger* logger_ = nullptr;

  CaptureSettings settings_;

  std::unique_ptr<capture::DeviceMonitor> monitor_;
  std::unique_ptr<capture::CapturePipeline> pipeline_;
  std::unique_ptr<capture::ISampleSource> source_;

  // Declared after the pipeline so that it is destroyed before it: the worker
  // reads through a publisher the pipeline owns, and the reverse order would
  // free the publisher first.
  std::unique_ptr<AnalysisWorker> analysis_;

  QTimer stats_timer_;

  std::vector<capture::DeviceInfo> devices_;
  bool monitoring_ = false;
  bool capturing_ = false;

  QString capture_path_;

  // The sink change this controller is waiting to see completed, so that the
  // finished file is collected at the moment the processing thread actually
  // swapped it rather than at the moment the swap was asked for.
  uint64_t pending_sink_change_ = 0;

  // Ticks until the next free-space check. The volume is interrogated about
  // once every two seconds rather than at the statistics rate: it is a
  // filesystem call, and nothing it reports changes twenty times a second.
  int ticks_until_space_check_ = 0;
  bool low_space_warned_ = false;

  static constexpr int kSpaceCheckIntervalTicks =
      2000 / kStatsIntervalMilliseconds;

  // The device the firmware warning has already been shown for. Cleared when
  // that device goes away, so re-plugging it warns again — which is right,
  // because re-plugging is what a user does after updating the firmware.
  QString warned_device_path_;
  QString warned_device_product_;

  // The gateware version that goes with warned_device_path_
  capture::FpgaVersion fpga_version_;

  // The gateware's own capability reading that goes with it —
  // MAX_ADC_RATE_MHZ, before the declared converter caps it. See
  // max_adc_rate_mhz().
  uint8_t max_adc_rate_mhz_ = 0;

  // See board_setup(). Read when a device appears, alongside the two above.
  capture::BoardSetupReading board_setup_;

  // Whether DcOffsetOutOfRange and BitShiftClipping have been raised this
  // run.
  bool offset_out_of_range_warned_ = false;
  bool shift_clipping_warned_ = false;

  // See run_conversion().
  capture::SampleConversion run_conversion_;

  // See requantization(): the status of the requantiser attached now, and of
  // the capture's, kept until its sidecar has been written. idle_key_ is what
  // the idle sink was built from — on or off, margin, bit shift — so that a
  // settings change that does not touch it does not rebuild it.
  std::shared_ptr<capture::RequantizationStatus> requantization_status_;
  std::shared_ptr<capture::RequantizationStatus> capture_requantization_;
  std::optional<std::tuple<bool, int, int>> idle_key_;

  // See show_corrected().
  bool show_corrected_ = false;

  // Whether the running stream is one a conversion applies to: not the
  // gateware's ramp, and not a DC offset measurement. See
  // UpdateRunConversion().
  bool run_converts_ = false;

  // The DC offset measurement, run as a short sequence of monitoring runs
  // driven from measure_timer_ — see MeasureDcOffset().
  enum class MeasurePhase {
    kIdle,
    kStarting,
    kSettling,
    kAveraging,
    kStopping
  };
  MeasurePhase measure_phase_ = MeasurePhase::kIdle;

  // One run of a measurement: the range and the ADC rate it is taken at, and
  // whether the rate is asked of the gateware or is simply the one it runs at.
  struct MeasureStep {
    bool range_2vpp = true;
    uint8_t rate_mhz = 0;
    bool sets_rate = true;
  };
  std::vector<MeasureStep> measure_steps_;
  size_t measure_index_ = 0;
  QTimer measure_timer_;
  QElapsedTimer measure_clock_;
  capture::DcOffsetReading measure_start_;
  uint16_t measure_minimum_ = UINT16_MAX;
  uint16_t measure_maximum_ = 0;
  QString measure_failure_;

  // The input range and ADC rate a measurement run forces, whatever the
  // settings say.
  std::optional<bool> range_override_;
  std::optional<uint8_t> rate_override_;

  // Set only around the measurement's own call to StartMonitoring(), which is
  // the one start a running measurement admits.
  bool measure_run_starting_ = false;

  // The PLL_PRESET value this controller last actually wrote to the device,
  // or -1 for "never sent this session". Distinct from
  // settings_.pll_preset_mhz, which is what the *next* write should ask for:
  // comparing the two is what tells StartMonitoring() whether a reconfiguration
  // - and the settling delay it costs - is actually needed, rather than paying
  // that delay on every monitor start once a preset has ever been chosen.
  int last_pll_preset_sent_ = -1;

  // See SetDiscProvenance. Empty for every capture taken without a player.
  capture::DiscProvenance disc_provenance_;

  // See SetPlayerIdentity and SetDiscScan.
  capture::PlayerIdentity player_identity_;
  capture::DiscScan disc_scan_;

  // What the running capture will say about itself, filled in when its file is
  // opened and completed when the file is closed.
  //
  // Latched at the start rather than gathered at the end, for the facts that
  // describe the setup: the player and the disc are cleared by the
  // automatic-capture coupling the moment its run ends, which is before the
  // encoder has finished the file this describes. The naming fields are
  // deliberately *not* latched — they are read at the end, so that notes typed
  // while watching a capture reach that capture's own metadata.
  capture::CaptureMetadata pending_metadata_;

  // The device's loss counters as they stood when the capture started, so that
  // the sidecar reports what the device lost while writing this file rather
  // than what it has lost since monitoring began.
  //
  // The signal figures need no equivalent. A minimum and a maximum cannot be
  // differenced, so the engine measures the file's own span in its own right —
  // see SampleMetrics::BeginCaptureSpan.
  uint64_t device_overflows_at_start_ = 0;
  uint64_t device_drops_at_start_ = 0;

  // See SetPipeOutput(). Held until the capture it is for opens, and handed to
  // that capture's PipeWriter then.
  std::shared_ptr<capture::IByteStream> pipe_stream_;
  bool pipe_saves_file_ = false;

  // The running piped capture's writer, kept here as well as in the sink so
  // that it can be watched while the sink is inside the pipeline, and waited
  // for once the sink has been finished. Null for an ordinary capture.
  std::shared_ptr<capture::PipeWriter> pipe_writer_;
  bool pipe_only_ = false;

  // What CheckPipe() has already said, so that each thing is said once.
  bool pipe_closed_reported_ = false;
  bool pipe_drops_reported_ = false;
};

}  // namespace ddd::gui
