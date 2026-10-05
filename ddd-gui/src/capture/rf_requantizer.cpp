/************************************************************************

    rf_requantizer.cpp

    Re-quantising a capture to as few bits as its own noise allows
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "rf_requantizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

#include "log_format.h"

namespace ddd::capture {
namespace {

// The noise floor is a low percentile of each bin's history rather than its
// mean: the signal comes and goes, the floor is what is left when it has
// gone. Each group spectrum averages eight frames, so a bin's values follow a
// chi-squared distribution of sixteen degrees of freedom, whose tenth
// percentile is 0.58 of its mean and whose median is 0.959 of it; dividing by
// those turns either back into an estimate of the mean.
constexpr double kFloorPercentile = 0.10;
constexpr double kFloorPercentileCorrection = 0.58;
constexpr double kMedianCorrection = 0.959;

// Where the video carrier never leaves, the floor is taken from the quietest
// bin within this distance instead. Cautious: it can only under-estimate the
// floor, and so only make the decision more careful.
constexpr double kEnvelopeHalfWidthMhz = 2.0;

// The slices the degradation is judged in.
constexpr double kSliceMhz = 1.0;

// The grid the shaping filter's weighting is integrated over.
constexpr size_t kDesignGrid = 4096;

// The fewest bits a segment can keep. See kMaximumRequantizerDrop.
constexpr int kMinimumAdaptiveBits = 4;

constexpr int kSampleBits = 16;
constexpr size_t kMinimumHistoryGroups = 8;
constexpr double kSmallestPower = 1e-12;
constexpr double kFullScale = 32768.0;

// Round to nearest, ties to even, by letting the floating-point unit do it:
// adding 1.5 x 2^52 leaves no bits for a fraction, so the sum is rounded in
// the default mode, and subtracting it again gives the rounded value. Exact
// for anything within +-2^51, which a 16-bit sample is far inside. Written out
// because std::nearbyint is a library call that saves and restores the
// floating-point environment on some platforms, and cost thirty-odd
// nanoseconds a sample here — most of what requantising a stream costs.
constexpr double kRoundingConstant = 6755399441055744.0;

inline double RoundToNearestEven(double value) {
  return (value + kRoundingConstant) - kRoundingConstant;
}

struct MarginLevel {
  const char* name;
  double limit_db;
  double hold_seconds;
};

// Aggressive takes any reduction at once; each level after it allows half as
// much degradation, or less, and waits longer before reducing.
constexpr std::array<MarginLevel, 5> kMarginLevels{{
    {"aggressive", 1.0, 0.0},
    {"moderate", 0.5, 0.1},
    {"safe", 0.2, 0.25},
    {"extra safe", 0.1, 0.5},
    {"ultra safe", 0.05, 1.0},
}};

const MarginLevel& Level(int level) {
  return kMarginLevels[static_cast<size_t>(
      std::clamp(level, kMinimumMarginLevel, kMaximumMarginLevel))];
}

bool InBands(const std::vector<FrequencyBand>& bands, double frequency_mhz) {
  return std::any_of(
      bands.begin(), bands.end(), [frequency_mhz](const FrequencyBand& band) {
        return frequency_mhz >= band.low_mhz && frequency_mhz <= band.high_mhz;
      });
}

}  // namespace

double MarginLimitDb(int level) { return Level(level).limit_db; }

double MarginHoldSeconds(int level) { return Level(level).hold_seconds; }

const char* MarginLevelName(int level) { return Level(level).name; }

std::vector<FrequencyBand> DefaultProtectedBands(double sample_rate_mhz) {
  return {{0.0, std::min(14.0, (sample_rate_mhz / 2.0) - 1.5)}};
}

std::string DescribeBands(const std::vector<FrequencyBand>& bands) {
  // To the hundredth of a megahertz, without the zeros: "0-13.5 MHz".
  const auto megahertz = [](double value) {
    std::string text = FormatDecimal(value, 2);
    while (text.find('.') != std::string::npos &&
           (text.back() == '0' || text.back() == '.')) {
      text.pop_back();
    }
    return text;
  };

  std::string text;
  for (const FrequencyBand& band : bands) {
    if (!text.empty()) {
      text += ", ";
    }
    text += megahertz(band.low_mhz) + "-" + megahertz(band.high_mhz);
  }
  return text.empty() ? text : text + " MHz";
}

std::vector<double> DesignNoiseTransferFunction(
    const std::vector<FrequencyBand>& bands, double sample_rate_mhz,
    double depth_db, int order) {
  const auto size = static_cast<size_t>(std::max(order, 0));
  const double outside = std::pow(10.0, -depth_db / 10.0);

  // The autocorrelation of the wanted noise weighting: 1 in the bands, the
  // depth below it outside them.
  std::vector<double> correlation(size + 1, 0.0);
  for (size_t lag = 0; lag <= size; ++lag) {
    double sum = 0.0;
    for (size_t index = 0; index < kDesignGrid; ++index) {
      const double frequency = (static_cast<double>(index) + 0.5) *
                               (sample_rate_mhz / 2.0) /
                               static_cast<double>(kDesignGrid);
      const double weight = InBands(bands, frequency) ? 1.0 : outside;
      sum += weight * std::cos(2.0 * std::numbers::pi * frequency /
                               sample_rate_mhz * static_cast<double>(lag));
    }
    correlation[lag] = sum / static_cast<double>(kDesignGrid);
  }
  correlation[0] *= 1.0 + 1e-9;

  // Levinson-Durbin.
  std::vector<double> coefficients(size + 1, 0.0);
  coefficients[0] = 1.0;
  double error = correlation[0];
  for (size_t i = 1; i <= size; ++i) {
    double accumulated = correlation[i];
    for (size_t j = 1; j < i; ++j) {
      accumulated += coefficients[j] * correlation[i - j];
    }
    const double reflection = -accumulated / error;
    const std::vector<double> previous = coefficients;
    for (size_t j = 1; j < i; ++j) {
      coefficients[j] = previous[j] + (reflection * previous[i - j]);
    }
    coefficients[i] = reflection;
    error *= 1.0 - (reflection * reflection);
  }
  return coefficients;
}

double NoiseTransferGainSquared(const std::vector<double>& coefficients,
                                double frequency_mhz, double sample_rate_mhz) {
  const double omega = 2.0 * std::numbers::pi * frequency_mhz / sample_rate_mhz;
  double real = 0.0;
  double imaginary = 0.0;
  for (size_t k = 0; k < coefficients.size(); ++k) {
    real += coefficients[k] * std::cos(omega * static_cast<double>(k));
    imaginary -= coefficients[k] * std::sin(omega * static_cast<double>(k));
  }
  return (real * real) + (imaginary * imaginary);
}

// --- ShapingQuantizer ----------------------------------------------------

ShapingQuantizer::ShapingQuantizer(int output_bits,
                                   std::vector<double> coefficients)
    : step_(std::ldexp(1.0, kSampleBits - output_bits)),
      inverse_step_(1.0 / step_),
      top_(kFullScale - step_) {
  if (coefficients.size() > 1) {
    feedback_.assign(coefficients.begin() + 1, coefficients.end());
  }
  history_.assign(2 * feedback_.size(), 0.0);
}

int16_t ShapingQuantizer::Quantize(int32_t sample) {
  const size_t order = feedback_.size();
  double value = static_cast<double>(sample);

  if (order > 0) {
    // The error history's contribution, the newest last. Everything but the
    // newest term is known before the previous sample has been rounded, and
    // summing it first in four independent partial sums keeps the chain from
    // one sample to the next down to a multiply and a rounding — which is
    // what lets this keep up with a live stream.
    const double* const errors = history_.data() + position_;
    std::array<double, 4> partial{};
    size_t k = 1;
    for (; k + 3 < order; k += 4) {
      partial[0] += feedback_[k] * errors[k];
      partial[1] += feedback_[k + 1] * errors[k + 1];
      partial[2] += feedback_[k + 2] * errors[k + 2];
      partial[3] += feedback_[k + 3] * errors[k + 3];
    }
    for (; k < order; ++k) {
      partial[0] += feedback_[k] * errors[k];
    }
    value = (value + ((partial[0] + partial[1]) + (partial[2] + partial[3]))) +
            (feedback_[0] * errors[0]);
  }

  // Round to nearest, ties to even: no DC bias.
  double quantised = RoundToNearestEven(value * inverse_step_) * step_;
  if (quantised < -kFullScale) {
    quantised = -kFullScale;
    ++clipped_;
  } else if (quantised > top_) {
    quantised = top_;
    ++clipped_;
  }

  if (order > 0) {
    // Held to one step after a clip, or the feedback runs away with it.
    const double error = std::clamp(quantised - value, -step_, step_);
    position_ = position_ == 0 ? order - 1 : position_ - 1;
    history_[position_] = error;
    history_[position_ + order] = error;
  }
  return static_cast<int16_t>(quantised);
}

void ShapingQuantizer::QuantizeInPlace(int16_t* samples, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    samples[index] = Quantize(samples[index]);
  }
}

void ShapingQuantizer::Reset() {
  std::fill(history_.begin(), history_.end(), 0.0);
  position_ = 0;
}

// --- RfRequantizer -------------------------------------------------------

ShapingQuantizer DecisionQuantizer(const RequantizerSettings& settings,
                                   int lsb_drop, bool shaped) {
  const int input_bits =
      std::clamp(settings.input_bits, kMinimumAdaptiveBits + 1, kSampleBits);
  const int bits = std::clamp(input_bits - std::max(lsb_drop, 0),
                              kMinimumAdaptiveBits, input_bits);
  if (!shaped) {
    return ShapingQuantizer(bits, {1.0});
  }
  const std::vector<FrequencyBand> bands =
      settings.protected_bands.empty()
          ? DefaultProtectedBands(settings.sample_rate_mhz)
          : settings.protected_bands;
  return ShapingQuantizer(
      bits, DesignNoiseTransferFunction(bands, settings.sample_rate_mhz,
                                        settings.shaping_depth_db,
                                        settings.shaping_order));
}

RfRequantizer::RfRequantizer(const RequantizerSettings& settings)
    : settings_(settings),
      bands_(settings.protected_bands.empty()
                 ? DefaultProtectedBands(settings.sample_rate_mhz)
                 : settings.protected_bands),
      limit_db_(MarginLimitDb(settings.margin_level)) {
  const double samples_per_second = settings_.sample_rate_mhz * 1e6;
  hold_segments_ = static_cast<int>(
      std::ceil(MarginHoldSeconds(settings_.margin_level) * samples_per_second /
                static_cast<double>(kSegmentSamples)));

  const int input_bits =
      std::clamp(settings_.input_bits, kMinimumAdaptiveBits + 1, kSampleBits);
  input_lsb_ = std::ldexp(1.0, kSampleBits - input_bits);

  // The candidates, most aggressive first; at equal bits the unshaped one
  // first, because it is the smaller file and is taken whenever it passes.
  const std::vector<double> shaping = DesignNoiseTransferFunction(
      bands_, settings_.sample_rate_mhz, settings_.shaping_depth_db,
      settings_.shaping_order);
  const double bin_width =
      settings_.sample_rate_mhz / static_cast<double>(kTransformSize);
  for (int bits =
           std::max(input_bits - kMaximumRequantizerDrop, kMinimumAdaptiveBits);
       bits < input_bits; ++bits) {
    for (const bool shaped : {false, true}) {
      Candidate candidate;
      candidate.lsb_drop = input_bits - bits;
      candidate.shaped = shaped;
      candidate.quantizer =
          ShapingQuantizer(bits, shaped ? shaping : std::vector<double>{1.0});

      // The variance a requantisation of an integer signal adds.
      const double step = candidate.quantizer.step();
      candidate.variance =
          ((step * step) + (2.0 * input_lsb_ * input_lsb_)) / 12.0;

      candidate.gain.resize(kBinCount);
      for (size_t bin = 0; bin < kBinCount; ++bin) {
        candidate.gain[bin] =
            shaped ? NoiseTransferGainSquared(
                         shaping, static_cast<double>(bin) * bin_width,
                         settings_.sample_rate_mhz)
                   : 1.0;
      }
      candidates_.push_back(std::move(candidate));
    }
  }
  applied_ = candidates_.size();
  previous_ = candidates_.size();

  history_groups_ =
      std::max(kMinimumHistoryGroups,
               static_cast<size_t>(
                   std::ceil(settings_.history_seconds * samples_per_second /
                             static_cast<double>(kGroupSpan))));
  history_.assign(history_groups_ * kBinCount, 0.0F);
  column_.resize(history_groups_);

  floor_.assign(kBinCount, kSmallestPower);
  frame_.resize(kTransformSize);
  spectrum_.resize(kBinCount);
  group_.resize(kBinCount);
  raw_floor_.resize(kBinCount);
  real_.resize(kTransformSize);
  imaginary_.resize(kTransformSize);

  // The transform's tables, once.
  size_t levels = 0;
  while ((size_t{1} << levels) < kTransformSize) {
    ++levels;
  }
  bit_reverse_.resize(kTransformSize);
  window_.resize(kTransformSize);
  for (size_t i = 0; i < kTransformSize; ++i) {
    size_t reversed = 0;
    for (size_t bit = 0; bit < levels; ++bit) {
      if ((i & (size_t{1} << bit)) != 0) {
        reversed |= size_t{1} << (levels - 1 - bit);
      }
    }
    bit_reverse_[i] = reversed;
    window_[i] =
        0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                              static_cast<double>(kTransformSize)));
    window_power_ += window_[i] * window_[i];
  }
  twiddle_real_.resize(kTransformSize / 2);
  twiddle_imaginary_.resize(kTransformSize / 2);
  for (size_t i = 0; i < kTransformSize / 2; ++i) {
    const double angle = 2.0 * std::numbers::pi * static_cast<double>(i) /
                         static_cast<double>(kTransformSize);
    twiddle_real_[i] = std::cos(angle);
    twiddle_imaginary_[i] = -std::sin(angle);
  }
}

bool RfRequantizer::InBand(double frequency_mhz) const {
  return InBands(bands_, frequency_mhz);
}

void RfRequantizer::Periodogram(const double* frame, double* power) {
  for (size_t i = 0; i < kTransformSize; ++i) {
    real_[bit_reverse_[i]] = frame[i] * window_[i];
    imaginary_[bit_reverse_[i]] = 0.0;
  }
  for (size_t length = 2; length <= kTransformSize; length <<= 1) {
    const size_t half = length / 2;
    const size_t stride = kTransformSize / length;
    for (size_t start = 0; start < kTransformSize; start += length) {
      for (size_t j = 0; j < half; ++j) {
        const double twiddle_real = twiddle_real_[j * stride];
        const double twiddle_imaginary = twiddle_imaginary_[j * stride];
        const size_t upper = start + j + half;
        const double product_real = (real_[upper] * twiddle_real) -
                                    (imaginary_[upper] * twiddle_imaginary);
        const double product_imaginary = (real_[upper] * twiddle_imaginary) +
                                         (imaginary_[upper] * twiddle_real);
        real_[upper] = real_[start + j] - product_real;
        imaginary_[upper] = imaginary_[start + j] - product_imaginary;
        real_[start + j] += product_real;
        imaginary_[start + j] += product_imaginary;
      }
    }
  }
  // Scaled so that the two-sided mean is the variance.
  for (size_t bin = 0; bin < kBinCount; ++bin) {
    power[bin] =
        ((real_[bin] * real_[bin]) + (imaginary_[bin] * imaginary_[bin])) /
        window_power_;
  }
}

void RfRequantizer::Analyse(const int16_t* samples, size_t count) {
  const double bin_width =
      settings_.sample_rate_mhz / static_cast<double>(kTransformSize);
  double band_power = 0.0;
  size_t band_bins = 0;
  size_t groups_now = 0;

  for (size_t start = 0; start + kGroupSpan <= count; start += kGroupSpan) {
    std::fill(group_.begin(), group_.end(), 0.0);
    for (size_t frame = 0; frame < kGroupFrames; ++frame) {
      const int16_t* const source =
          samples + start + (frame * kTransformSize * kFrameStride);
      for (size_t i = 0; i < kTransformSize; ++i) {
        frame_[i] = static_cast<double>(source[i]);
      }
      Periodogram(frame_.data(), spectrum_.data());
      for (size_t bin = 0; bin < kBinCount; ++bin) {
        group_[bin] += spectrum_[bin] / static_cast<double>(kGroupFrames);
      }
    }

    float* const slot = history_.data() + (history_position_ * kBinCount);
    for (size_t bin = 0; bin < kBinCount; ++bin) {
      slot[bin] = static_cast<float>(group_[bin]);
      if (InBand(static_cast<double>(bin) * bin_width)) {
        band_power += group_[bin];
        ++band_bins;
      }
    }
    history_position_ = (history_position_ + 1) % history_groups_;
    history_filled_ = std::min(history_filled_ + 1, history_groups_);
    ++groups_now;
  }

  // Too short to hold a group — the end of a stream. The floor stands.
  if (band_bins == 0) {
    return;
  }

  const auto percentile_index = static_cast<size_t>(
      kFloorPercentile * static_cast<double>(history_filled_ - 1));
  for (size_t bin = 0; bin < kBinCount; ++bin) {
    for (size_t group = 0; group < history_filled_; ++group) {
      column_[group] = history_[(group * kBinCount) + bin];
    }
    std::nth_element(
        column_.begin(),
        column_.begin() + static_cast<std::ptrdiff_t>(percentile_index),
        column_.begin() + static_cast<std::ptrdiff_t>(history_filled_));
    double estimate = static_cast<double>(column_[percentile_index]) /
                      kFloorPercentileCorrection;

    // This segment on its own, so that a floor that falls abruptly is
    // followed at once rather than after the history has caught up with it.
    if (groups_now < history_filled_) {
      for (size_t group = 0; group < groups_now; ++group) {
        const size_t index =
            (history_position_ + history_groups_ - 1 - group) % history_groups_;
        column_[group] = history_[(index * kBinCount) + bin];
      }
      const size_t middle = groups_now / 2;
      std::nth_element(
          column_.begin(),
          column_.begin() + static_cast<std::ptrdiff_t>(middle),
          column_.begin() + static_cast<std::ptrdiff_t>(groups_now));
      const double current =
          static_cast<double>(column_[middle]) / kMedianCorrection;
      estimate = std::min(estimate, current);
    }
    raw_floor_[bin] = estimate;
  }

  // The low envelope, within the bands.
  const auto half_width =
      static_cast<std::ptrdiff_t>(std::ceil(kEnvelopeHalfWidthMhz / bin_width));
  const auto bins = static_cast<std::ptrdiff_t>(kBinCount);
  for (std::ptrdiff_t bin = 0; bin < bins; ++bin) {
    double lowest = raw_floor_[static_cast<size_t>(bin)];
    for (std::ptrdiff_t other = std::max<std::ptrdiff_t>(0, bin - half_width);
         other <= std::min(bins - 1, bin + half_width); ++other) {
      const auto index = static_cast<size_t>(other);
      if (InBand(static_cast<double>(index) * bin_width)) {
        lowest = std::min(lowest, raw_floor_[index]);
      }
    }
    floor_[static_cast<size_t>(bin)] = std::max(lowest, kSmallestPower);
  }
  have_floor_ = true;

  // What the floor and the band amount to, for whoever is watching.
  std::vector<double> in_band;
  for (size_t bin = 0; bin < kBinCount; ++bin) {
    if (InBand(static_cast<double>(bin) * bin_width)) {
      in_band.push_back(floor_[bin]);
    }
  }
  if (!in_band.empty()) {
    const size_t middle = in_band.size() / 2;
    std::nth_element(in_band.begin(),
                     in_band.begin() + static_cast<std::ptrdiff_t>(middle),
                     in_band.end());
    const double median = in_band[middle];
    noise_floor_lsb_ = std::sqrt(median) / input_lsb_;
    carrier_to_noise_db_ =
        10.0 *
        std::log10((band_power / static_cast<double>(band_bins)) / median);
  }
}

double RfRequantizer::Degradation(const Candidate& candidate) const {
  const double bin_width =
      settings_.sample_rate_mhz / static_cast<double>(kTransformSize);
  double worst = 0.0;
  for (const FrequencyBand& band : bands_) {
    // Counted rather than stepped in floating point, so that no slice is
    // gained or lost to rounding at the top of a band.
    const auto slices = static_cast<int>(
        std::ceil((band.high_mhz - band.low_mhz - 1e-9) / kSliceMhz));
    for (int slice = 0; slice < slices; ++slice) {
      const double low =
          band.low_mhz + (static_cast<double>(slice) * kSliceMhz);
      const double high = std::min(low + kSliceMhz, band.high_mhz);
      double added = 0.0;
      double floor = 0.0;
      for (size_t bin = 0; bin < kBinCount; ++bin) {
        const double frequency = static_cast<double>(bin) * bin_width;
        if (frequency >= low && frequency < high) {
          added += candidate.variance * candidate.gain[bin];
          floor += floor_[bin];
        }
      }
      if (floor > 0.0) {
        worst = std::max(worst, 10.0 * std::log10(1.0 + (added / floor)));
      }
    }
  }
  return worst;
}

RequantizerDecision RfRequantizer::Process(int16_t* samples, size_t count,
                                           bool apply) {
  Analyse(samples, count);

  const size_t none = candidates_.size();
  size_t wanted = none;
  if (have_floor_) {
    for (size_t index = 0; index < none; ++index) {
      if (Degradation(candidates_[index]) <= limit_db_) {
        wanted = index;
        break;
      }
    }
  }

  // More careful at once; more aggressive only once it has been justified for
  // longer than the margin's hold, so that a passing lull does not cost the
  // next loud moment its bits.
  if (wanted >= applied_ || ++pending_ > hold_segments_) {
    applied_ = wanted;
    pending_ = 0;
  }
  if (applied_ < none && Degradation(candidates_[applied_]) > limit_db_) {
    applied_ = wanted;
  }

  RequantizerDecision decision;
  if (applied_ < none) {
    Candidate& candidate = candidates_[applied_];
    decision.lsb_drop = candidate.lsb_drop;
    decision.shaped = candidate.shaped;
    decision.degradation_db = Degradation(candidate);

    if (apply) {
      // A quantiser taken over from another carries an error history that
      // belongs to that one's shaping.
      if (applied_ != previous_) {
        candidate.quantizer.Reset();
      }
      candidate.quantizer.QuantizeInPlace(samples, count);
    }
  }
  previous_ = applied_;
  return decision;
}

uint64_t RfRequantizer::clipped() const {
  uint64_t total = 0;
  for (const Candidate& candidate : candidates_) {
    total += candidate.quantizer.clipped();
  }
  return total;
}

}  // namespace ddd::capture
