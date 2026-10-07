/************************************************************************

    rf_requantizer.h

    Re-quantising a capture to as few bits as its own noise allows
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ddd::capture {

// The adaptive re-quantiser, after rfquant's dynamic mode.
//
// A capture's low bits carry the converter's quantisation noise and the
// front end's analogue noise, and the analogue noise is usually the larger:
// dropping a bit whose contribution is buried under it costs nothing anybody
// can measure and saves nearly a bit a sample in a FLAC file. How many can go
// depends on how noisy this capture is, and that changes with the disc, the
// player and the moment — so it is decided again for every segment of about
// 35 ms, from the noise floor of the last half second.
//
// The decision is made per 1 MHz slice of the protected bands: a setting is
// allowed when the noise it adds raises the floor of the worst slice by no
// more than the margin's limit. The added noise is known analytically — the
// variance of a requantisation, shaped by the noise transfer function where
// noise shaping is used — so trying a candidate costs a sum, not a pass over
// the samples.
//
// Noise shaping pushes the added noise out of the protected bands, where the
// decoder's filters remove it, so a shaped setting passes at fewer bits than
// an unshaped one. The filter is designed by Levinson-Durbin from the wanted
// weighting: A(z) minimum phase, |A|^2 proportional to 1/weight.
//
// Samples are signed 16-bit and left-aligned, as every writer produces them;
// a re-quantised sample is still one, with more zero bits at the bottom, which
// FLAC stores for nothing and which reads as an ordinary capture everywhere.

// A band the decoder needs, in MHz.
struct FrequencyBand {
  double low_mhz = 0.0;
  double high_mhz = 0.0;

  bool operator==(const FrequencyBand& other) const = default;
};

// How much the noise floor of any 1 MHz slice of the protected bands may rise,
// and how long a more aggressive setting has to stay justified before it is
// taken. Five levels, from aggressive to ultra safe.
inline constexpr int kMinimumMarginLevel = 0;
inline constexpr int kMaximumMarginLevel = 4;
inline constexpr int kDefaultMarginLevel = 2;

double MarginLimitDb(int level);
double MarginHoldSeconds(int level);
const char* MarginLevelName(int level);

// The most of the input's bits a segment can lose. The quality limit decides
// in practice; this only bounds how many candidates are tried.
inline constexpr int kMaximumRequantizerDrop = 6;

// From DC to 14 MHz — the LaserDisc's RF — or to 1.5 MHz short of the
// Nyquist limit at a rate that cannot hold all of that.
std::vector<FrequencyBand> DefaultProtectedBands(double sample_rate_mhz);

// The bands as a capture records them: "0-14 MHz", comma separated.
std::string DescribeBands(const std::vector<FrequencyBand>& bands);

// Bands as a person types them, in MHz: "0-12", or several, "0-1.9,2.1-13.5",
// with ':' accepted for '-' and spaces and a trailing "MHz" ignored. Plain
// decimals, read the same whatever the locale. Each band's low end below its
// high end and none negative; empty for anything else.
std::vector<FrequencyBand> ParseBands(std::string_view text);

// `bands` as they can be used at `sample_rate_mhz`: cut at the Nyquist limit,
// and any left with nothing in them dropped. Empty when none survive.
std::vector<FrequencyBand> BandsWithin(const std::vector<FrequencyBand>& bands,
                                       double sample_rate_mhz);

// A stretch outside the protected bands with a shaping depth of its own: how
// far above the bands the added noise may be pushed there, in place of the
// one depth everywhere else. Outside the bands is not all alike — the EFM
// below the video is still read, and the top of the spectrum is not — so a
// shallow zone over the one keeps the noise off it, and a deep one over the
// other takes what it is spared.
struct ShapingZone {
  double low_mhz = 0.0;
  double high_mhz = 0.0;
  double depth_db = 0.0;

  bool operator==(const ShapingZone& other) const = default;
};

// The zones as a capture records them: "0-2 MHz @ 10 dB", comma separated.
std::string DescribeShapingZones(const std::vector<ShapingZone>& zones);

// Shaping depths as a person types them, in dB: one, "40", or one for each
// stretch between the bands, low to high, "10, 40". Plain decimals, 0 to
// kMaximumShapingDepthDb, each with an optional "dB". Empty for anything else.
std::vector<double> ParseShapingDepths(std::string_view text);

// The depths as they were read back: "10, 40 dB". Empty for none.
std::string DescribeShapingDepths(const std::vector<double>& depths);

// The stretches outside `bands`, from DC to the Nyquist limit of
// `sample_rate_mhz`, low to high, each with the depth at the same place in
// `depths` — the first below the lowest band, the next between it and the
// one after — the last repeated where there are more stretches than depths,
// and any beyond the stretches unused here: more depths than stretches is
// refused before a capture gets this far. With bands 2-14 MHz at 35 Msps,
// "10, 40" puts 10 dB on the EFM below 2 MHz and 40 dB above 14 MHz.
std::vector<ShapingZone> GapShapingZones(
    const std::vector<FrequencyBand>& bands, const std::vector<double>& depths,
    double sample_rate_mhz);

struct RequantizerSettings {
  // The rate of the samples this is given, after any decimation.
  double sample_rate_mhz = 40.0;

  int margin_level = kDefaultMarginLevel;

  // Empty means DefaultProtectedBands().
  std::vector<FrequencyBand> protected_bands;

  // How many of the sixteen bits the input carries: the converter's ten, less
  // the bit shift applied before this — a shifted sample's low bits are zero
  // already, and a drop is counted from the first one that is not.
  int input_bits = 10;

  // How the added noise is shaped.
  //
  //  - kFixed: one filter, designed at the start from the bands alone — low in
  //    them, shaping_depth_db higher outside. The added noise is the same
  //    everywhere in the bands, so the band's quietest slice decides for all of
  //    it, however much more noise the rest could take.
  //  - kAdaptive: designed again for every segment from the floor just
  //    measured, so that the added noise follows it — more where the band is
  //    already noisy, less where it is quiet — and as much as possible goes
  //    outside the bands, up to shaping_depth_db above the band's highest
  //    floor. The floor is also smoothed over a quarter of a megahertz before
  //    its low envelope is taken, which takes out the bias a minimum of noisy
  //    estimates has: about 1.2 dB too low on a flat floor.
  //
  // Fixed is the original design, and what is decided with it is what rfquant
  // decides. See UseAdaptiveShaping() for the adaptive mode's own order and
  // depth.
  enum class Shaping { kFixed, kAdaptive };
  Shaping shaping = Shaping::kFixed;
  int shaping_order = 16;
  double shaping_depth_db = 10.0;

  // Stretches outside the bands with a depth of their own, in place of
  // shaping_depth_db; see ShapingZone. Where one overlaps a band the band wins,
  // and where two overlap the shallower does.
  std::vector<ShapingZone> shaping_zones;

  // How far back the noise floor is estimated from.
  double history_seconds = 0.5;
};

// Switch `settings` to adaptive shaping, with the order and depth it was
// measured at: order 32, 20 dB. A finer filter is what lets the noise follow a
// sloping floor, and the wider depth what lets it be pushed out of the bands.
void UseAdaptiveShaping(RequantizerSettings& settings);

// What a person asks of the requantiser, in the terms the capture application
// and ddd-requantize both offer, before it is turned into RequantizerSettings.
// One translation, used by both, so that a capture re-requantised offline gets
// exactly what the capture application would have given it.
struct RequantizerRequest {
  // The rate of the samples, after any decimation.
  double sample_rate_mhz = 40.0;

  int margin_level = kDefaultMarginLevel;

  // As asked for; cut at the Nyquist limit, and the default for the rate where
  // none is left.
  std::vector<FrequencyBand> bands;

  int input_bits = 10;
  bool adaptive = false;

  // Empty for the mode's own depth; one for everywhere outside the bands;
  // several for each stretch between them (GapShapingZones).
  std::vector<double> depths_db;

  // 0 for the mode's own.
  int order = 0;
};

RequantizerSettings SettingsFor(const RequantizerRequest& request);

// The bands `request` protects: those asked for that are left below the
// Nyquist limit, or the default for the rate.
std::vector<FrequencyBand> RequestedBands(const RequantizerRequest& request);

// Why the depths in `request` cannot be used — more of them than there are
// stretches between the bands at the rate — in a sentence; empty when they
// can. One depth always can, and fewer than the stretches can too: the last
// stands for the rest.
std::string ShapingDepthsProblem(const RequantizerRequest& request);

// The range a shaping filter's order and depth may be set to by hand, to try
// them. The order is the length of the error feedback every sample pays for,
// so it is held to what a stream can afford; the depth is how far above the
// bands the added noise may be pushed, and beyond 40 dB the filter's gain
// outside them is more than the quantiser's error clamp lets it use.
inline constexpr int kMinimumShapingOrder = 2;
inline constexpr int kMaximumShapingOrder = 64;
inline constexpr double kMaximumShapingDepthDb = 40.0;

// What was done to one segment: how many of the input's low bits were
// dropped, whether the added noise was shaped, and what that cost the worst
// protected slice. A drop of 0 leaves the segment as it was.
struct RequantizerDecision {
  int lsb_drop = 0;
  bool shaped = false;
  double degradation_db = 0.0;

  // What stopped it going further: the 1 MHz slice of the protected bands
  // that the next more aggressive setting would have raised the most, and that
  // slice's own floor in input LSBs. The median floor of the band says how
  // noisy the capture is; this says why that was not enough — one quiet slice,
  // a notch in the player's RF or the edge of a filter, holds the whole band
  // back. Zero-width when nothing was more aggressive to try.
  double limit_low_mhz = 0.0;
  double limit_high_mhz = 0.0;
  double limit_floor_lsb = 0.0;

  // A(z) as it was applied, {1.0} for plain rounding; empty when nothing was
  // dropped. Carried because adaptive shaping designs it afresh for every
  // segment, and something showing a decision's effect — the signal panels —
  // has to round with exactly this. See DecisionQuantizer().
  std::vector<double> coefficients;

  bool operator==(const RequantizerDecision& other) const = default;
};

// A change of decision, from `first_sample` of the stream on — how a capture
// records what was done to which of its samples without a line per segment.
struct RequantizationChange {
  uint64_t first_sample = 0;
  int lsb_drop = 0;
  bool shaped = false;

  bool operator==(const RequantizationChange& other) const = default;
};

// What a capture records of its requantisation: the settings, which are known
// when it starts and go in the FLAC tags, and what was done, known only when
// it ends and written to the sidecar — the samples each choice was applied to,
// and the changes that place them. A change only where the decision changed,
// so a capture that settled on one setting has one however long it ran.
struct RequantizationRecord {
  bool enabled = false;
  int margin_level = kDefaultMarginLevel;
  std::vector<FrequencyBand> protected_bands;
  int input_bits = 0;
  bool adaptive_shaping = false;
  int shaping_order = 0;
  double shaping_depth_db = 0.0;
  std::vector<ShapingZone> shaping_zones;

  // Samples by bits dropped, index 0 for none.
  std::vector<uint64_t> samples_by_drop;
  uint64_t shaped_samples = 0;
  double worst_degradation_db = 0.0;
  uint64_t clipped_samples = 0;
  std::vector<RequantizationChange> changes;
};

// The settings half of a record: what a requantiser set up with `settings` is
// recorded as having been asked to do. The capture application and
// ddd-requantize both record through this, so a file says the same of itself
// whichever of them wrote it.
RequantizationRecord RecordFor(const RequantizerSettings& settings);

// The noise transfer function A(z), minimum phase, coefficients a[0..order]
// with a[0] = 1, whose |A(f)|^2 is low in `bands` and high outside them by
// about `depth_db`.
std::vector<double> DesignNoiseTransferFunction(
    const std::vector<FrequencyBand>& bands, double sample_rate_mhz,
    double depth_db, int order);

// The same, high outside the bands by each zone's own depth where `zones` set
// one, and by `depth_db` everywhere else.
std::vector<double> DesignNoiseTransferFunction(
    const std::vector<FrequencyBand>& bands,
    const std::vector<ShapingZone>& zones, double sample_rate_mhz,
    double depth_db, int order);

// |A(f)|^2.
double NoiseTransferGainSquared(const std::vector<double>& coefficients,
                                double frequency_mhz, double sample_rate_mhz);

// Rounds to a coarser step, feeding the error back through A(z) when it has
// more than one coefficient. Signed 16-bit in and out; the output is a whole
// multiple of the step, saturated to the range.
class ShapingQuantizer {
 public:
  ShapingQuantizer() = default;

  // `output_bits` of the sixteen are kept. `coefficients` is A(z) as
  // DesignNoiseTransferFunction() returns it, or {1.0} for plain rounding.
  ShapingQuantizer(int output_bits, std::vector<double> coefficients);

  int16_t Quantize(int32_t sample);

  // The same, over a block, in place.
  void QuantizeInPlace(int16_t* samples, size_t count);

  // Forget the error history, as at the start of a stream.
  void Reset();

  // Shape with another A(z) from the next sample on. The error history is kept
  // when the order is the same — adaptive shaping changes the filter between
  // segments of one stream, and the errors already made belong to it — and
  // forgotten when it is not.
  void SetCoefficients(const std::vector<double>& coefficients);

  // A(z) as given, {1.0} for plain rounding.
  std::vector<double> coefficients() const;

  double step() const { return step_; }
  uint64_t clipped() const { return clipped_; }

 private:
  double step_ = 1.0;
  double inverse_step_ = 1.0;
  double top_ = 32767.0;

  // a[1..order], and the error history held twice over so that the newest
  // `order` errors are always one contiguous window — no shuffle per sample.
  std::vector<double> feedback_;
  std::vector<double> history_;
  size_t position_ = 0;
  uint64_t clipped_ = 0;
};

// The quantiser a decision applies: its bits dropped from the input's, with
// the A(z) it carries. For showing a decision's effect somewhere other than the
// file — the signal panels — exactly as the file gets it.
ShapingQuantizer DecisionQuantizer(const RequantizerSettings& settings,
                                   const RequantizerDecision& decision);

class RfRequantizer {
 public:
  // The analysis transform, and how it is spent: frames of 1,024 samples,
  // averaged eight at a time into one spectrum, using one frame in four — the
  // floor moves slowly, and a quarter of the stream is plenty to follow it at
  // a quarter of the cost.
  static constexpr size_t kTransformSize = 1024;
  static constexpr size_t kBinCount = (kTransformSize / 2) + 1;
  static constexpr size_t kGroupFrames = 8;
  static constexpr size_t kFrameStride = 4;
  static constexpr size_t kGroupSpan =
      kTransformSize * kGroupFrames * kFrameStride;

  // One decision per segment: 2^20 samples, about 35 ms at 30 Msps — and a
  // whole number of every FLAC block size from 4,096 to 32,768, so a block
  // never straddles two settings and loses the zero bits of both.
  static constexpr size_t kSegmentSamples = size_t{1} << 20;

  explicit RfRequantizer(const RequantizerSettings& settings);

  // Decide what to do with one segment of at most kSegmentSamples, from the
  // floor including it, and do it in place when `apply` is set. A preview
  // passes false and pays only for the analysis.
  RequantizerDecision Process(int16_t* samples, size_t count, bool apply);

  // The floor the last decision was made against, as the RMS of white noise
  // of the same power, in input LSBs; and the band's mean power over it.
  double noise_floor_lsb() const { return noise_floor_lsb_; }
  double carrier_to_noise_db() const { return carrier_to_noise_db_; }

  double limit_db() const { return limit_db_; }
  const RequantizerSettings& settings() const { return settings_; }
  const std::vector<FrequencyBand>& bands() const { return bands_; }

  // Samples the requantisation held at full scale, over the whole run.
  uint64_t clipped() const;

 private:
  struct Candidate {
    int lsb_drop = 0;
    bool shaped = false;
    double variance = 0.0;
    std::vector<double> gain;
    ShapingQuantizer quantizer;
  };

  // A candidate's cost: the most it raises any slice's floor, and which slice
  // that is, with the slice's mean floor power.
  struct SliceCost {
    double db = 0.0;
    double low_mhz = 0.0;
    double high_mhz = 0.0;
    double floor_power = 0.0;
  };

  bool InBand(double frequency_mhz) const;
  void Analyse(const int16_t* samples, size_t count);
  void Periodogram(const double* frame, double* power);
  SliceCost Degradation(const Candidate& candidate) const;

  // Adaptive shaping: A(z) designed from the floor just measured, and handed
  // to every shaped candidate with the gain it has in each bin.
  void RedesignShaping();

  RequantizerSettings settings_;
  std::vector<FrequencyBand> bands_;
  double limit_db_ = 0.0;
  int hold_segments_ = 0;
  double input_lsb_ = 64.0;

  std::vector<Candidate> candidates_;

  // Group spectra, newest last, as a ring of kBinCount floats each.
  std::vector<float> history_;
  size_t history_groups_ = 0;
  size_t history_filled_ = 0;
  size_t history_position_ = 0;

  std::vector<double> floor_;
  bool have_floor_ = false;

  // Index into candidates_ of what is applied, candidates_.size() for nothing;
  // how many segments in a row have justified something more aggressive; and
  // what was applied to the last segment, whose quantiser carries its error
  // into the next when it is applied again.
  size_t applied_ = 0;
  int pending_ = 0;
  size_t previous_ = 0;

  double noise_floor_lsb_ = 0.0;
  double carrier_to_noise_db_ = 0.0;

  std::vector<float> column_;
  std::vector<double> frame_;
  std::vector<double> spectrum_;
  std::vector<double> group_;
  std::vector<double> raw_floor_;
  std::vector<double> smoothed_floor_;

  // For adaptive shaping: cos and sin of each bin's frequency times each lag,
  // lag by lag, worked out once — the design and the gain it gives are then
  // sums over them.
  std::vector<double> lag_cos_;
  std::vector<double> lag_sin_;
  // And how far above the bands' highest floor each bin outside them may be
  // pushed, as a power ratio: the depth there, a zone's or the setting's.
  std::vector<double> outside_scale_;
  // The fixed transform: bit-reversal order, twiddles and the Hann window,
  // worked out once.
  std::vector<size_t> bit_reverse_;
  std::vector<double> twiddle_real_;
  std::vector<double> twiddle_imaginary_;
  std::vector<double> window_;
  double window_power_ = 0.0;
  std::vector<double> real_;
  std::vector<double> imaginary_;
};

}  // namespace ddd::capture
