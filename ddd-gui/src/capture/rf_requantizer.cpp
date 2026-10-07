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
#include <cctype>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
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

// Adaptive shaping only: the floor is averaged over this distance either side
// before its low envelope is taken. A minimum of noisy estimates sits below
// their mean — by about 13% on a flat floor, a 1.2 dB caution nobody chose —
// and an average over a quarter of a megahertz has too little noise left to
// do that, while a quiet place much narrower than a slice still shows.
constexpr double kFloorSmoothingHalfWidthMhz = 0.25;

// What UseAdaptiveShaping() sets: measured, on a floor falling 7.5 dB across
// the top of the band, to drop a bit more than the fixed filter at the same
// margin.
constexpr int kAdaptiveShapingOrder = 32;
constexpr double kAdaptiveShapingDepthDb = 20.0;

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

// The depth outside the bands at `frequency_mhz`: the shallowest of the zones
// that hold it, or `depth_db` where none does.
double DepthAt(const std::vector<ShapingZone>& zones, double depth_db,
               double frequency_mhz) {
  std::optional<double> zoned;
  for (const ShapingZone& zone : zones) {
    if (frequency_mhz >= zone.low_mhz && frequency_mhz <= zone.high_mhz) {
      zoned = std::min(zoned.value_or(zone.depth_db), zone.depth_db);
    }
  }
  return zoned.value_or(depth_db);
}

// A(z), minimum phase with a[0] = 1, from the autocorrelation of the wanted
// noise weighting, by Levinson-Durbin: |A|^2 comes out proportional to
// 1/weight.
std::vector<double> LevinsonDurbin(const std::vector<double>& correlation) {
  const size_t size = correlation.empty() ? 0 : correlation.size() - 1;
  std::vector<double> coefficients(size + 1, 0.0);
  coefficients[0] = 1.0;
  if (size == 0) {
    return coefficients;
  }
  double error = correlation[0] * (1.0 + 1e-9);
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

}  // namespace

void UseAdaptiveShaping(RequantizerSettings& settings) {
  settings.shaping = RequantizerSettings::Shaping::kAdaptive;
  settings.shaping_order = kAdaptiveShapingOrder;
  settings.shaping_depth_db = kAdaptiveShapingDepthDb;
}

double MarginLimitDb(int level) { return Level(level).limit_db; }

double MarginHoldSeconds(int level) { return Level(level).hold_seconds; }

const char* MarginLevelName(int level) { return Level(level).name; }

std::vector<FrequencyBand> DefaultProtectedBands(double sample_rate_mhz) {
  return {{0.0, std::min(14.0, (sample_rate_mhz / 2.0) - 1.5)}};
}

namespace {

// To the hundredth, without the zeros: "13.5", "2".
std::string ShortDecimal(double value) {
  std::string text = FormatDecimal(value, 2);
  while (text.find('.') != std::string::npos &&
         (text.back() == '0' || text.back() == '.')) {
    text.pop_back();
  }
  return text;
}

}  // namespace

std::string DescribeBands(const std::vector<FrequencyBand>& bands) {
  std::string text;
  for (const FrequencyBand& band : bands) {
    if (!text.empty()) {
      text += ", ";
    }
    text += ShortDecimal(band.low_mhz) + "-" + ShortDecimal(band.high_mhz);
  }
  return text.empty() ? text : text + " MHz";
}

std::string DescribeShapingZones(const std::vector<ShapingZone>& zones) {
  std::string text;
  for (const ShapingZone& zone : zones) {
    if (!text.empty()) {
      text += ", ";
    }
    text += ShortDecimal(zone.low_mhz) + "-" + ShortDecimal(zone.high_mhz) +
            " MHz @ " + ShortDecimal(zone.depth_db) + " dB";
  }
  return text;
}

namespace {

// `text` without its blanks, in lower case: what both parsers read.
std::string Compact(std::string_view text) {
  std::string compact;
  for (const char character : text) {
    if (character != ' ' && character != '\t') {
      compact.push_back(static_cast<char>(
          std::tolower(static_cast<unsigned char>(character))));
    }
  }
  return compact;
}

// A unit after a number, which says nothing the position does not: taken off
// `text` when it is there.
void SkipUnit(std::string_view& text, std::string_view unit) {
  if (text.starts_with(unit)) {
    text.remove_prefix(unit.size());
  }
}

// A plain decimal at the front of `text` — digits, and optionally a point and
// more digits — taken off it. By hand rather than through strtod, which reads
// "13.5" as 13 in a locale whose decimal separator is a comma.
std::optional<double> TakeDecimal(std::string_view& text) {
  const auto is_digit = [&text](size_t index) {
    return index < text.size() && text[index] >= '0' && text[index] <= '9';
  };

  size_t length = 0;
  bool any_digit = false;
  double value = 0.0;
  while (is_digit(length)) {
    value = (value * 10.0) + static_cast<double>(text[length] - '0');
    any_digit = true;
    ++length;
  }
  if (length < text.size() && text[length] == '.') {
    ++length;
    double scale = 0.1;
    while (is_digit(length)) {
      value += scale * static_cast<double>(text[length] - '0');
      scale /= 10.0;
      any_digit = true;
      ++length;
    }
  }
  if (!any_digit) {
    return std::nullopt;
  }
  text.remove_prefix(length);
  return value;
}

}  // namespace

std::vector<FrequencyBand> ParseBands(std::string_view text) {
  std::string compact = Compact(text);
  if (compact.ends_with("mhz")) {
    compact.resize(compact.size() - 3);
  }

  std::string_view rest = compact;
  std::vector<FrequencyBand> bands;
  for (;;) {
    const std::optional<double> low = TakeDecimal(rest);
    if (!low.has_value() || rest.empty() ||
        (rest.front() != '-' && rest.front() != ':')) {
      return {};
    }
    rest.remove_prefix(1);
    const std::optional<double> high = TakeDecimal(rest);
    if (!high.has_value() || *high <= *low) {
      return {};
    }
    bands.push_back({*low, *high});

    if (rest.empty()) {
      return bands;
    }
    if (rest.front() != ',') {
      return {};
    }
    rest.remove_prefix(1);
  }
}

std::vector<FrequencyBand> BandsWithin(const std::vector<FrequencyBand>& bands,
                                       double sample_rate_mhz) {
  const double nyquist = sample_rate_mhz / 2.0;
  std::vector<FrequencyBand> usable;
  for (const FrequencyBand& band : bands) {
    const double high = std::min(band.high_mhz, nyquist);
    if (band.low_mhz < high) {
      usable.push_back({band.low_mhz, high});
    }
  }
  return usable;
}

std::vector<double> ParseShapingDepths(std::string_view text) {
  const std::string compact = Compact(text);
  std::string_view rest = compact;
  std::vector<double> depths;
  for (;;) {
    const std::optional<double> depth = TakeDecimal(rest);
    if (!depth.has_value() || *depth > kMaximumShapingDepthDb) {
      return {};
    }
    SkipUnit(rest, "db");
    depths.push_back(*depth);

    if (rest.empty()) {
      return depths;
    }
    if (rest.front() != ',') {
      return {};
    }
    rest.remove_prefix(1);
  }
}

std::string DescribeShapingDepths(const std::vector<double>& depths) {
  std::string text;
  for (const double depth : depths) {
    if (!text.empty()) {
      text += ", ";
    }
    text += ShortDecimal(depth);
  }
  return text.empty() ? text : text + " dB";
}

std::vector<ShapingZone> GapShapingZones(
    const std::vector<FrequencyBand>& bands, const std::vector<double>& depths,
    double sample_rate_mhz) {
  if (depths.empty()) {
    return {};
  }
  std::vector<FrequencyBand> sorted = BandsWithin(bands, sample_rate_mhz);
  std::sort(sorted.begin(), sorted.end(),
            [](const FrequencyBand& left, const FrequencyBand& right) {
              return left.low_mhz < right.low_mhz;
            });

  // Each stretch nothing protects, from DC up, takes the next depth.
  std::vector<ShapingZone> zones;
  const auto add = [&zones, &depths](double low, double high) {
    if (high > low) {
      const size_t index = std::min(zones.size(), depths.size() - 1);
      zones.push_back(
          {low, high, std::clamp(depths[index], 0.0, kMaximumShapingDepthDb)});
    }
  };
  double edge = 0.0;
  for (const FrequencyBand& band : sorted) {
    add(edge, band.low_mhz);
    edge = std::max(edge, band.high_mhz);
  }
  add(edge, sample_rate_mhz / 2.0);
  return zones;
}

std::vector<FrequencyBand> RequestedBands(const RequantizerRequest& request) {
  std::vector<FrequencyBand> bands =
      BandsWithin(request.bands, request.sample_rate_mhz);
  return bands.empty() ? DefaultProtectedBands(request.sample_rate_mhz) : bands;
}

RequantizerSettings SettingsFor(const RequantizerRequest& request) {
  RequantizerSettings settings;
  settings.sample_rate_mhz = request.sample_rate_mhz;
  settings.margin_level = request.margin_level;
  settings.protected_bands = RequestedBands(request);
  settings.input_bits = request.input_bits;
  if (request.adaptive) {
    UseAdaptiveShaping(settings);
  }

  // A depth or an order asked for, in place of the mode's own. One depth is
  // the depth everywhere outside the bands; several are one for each stretch
  // between them, from DC up, the last of them standing for the rest.
  const std::vector<double>& depths = request.depths_db;
  if (!depths.empty()) {
    settings.shaping_depth_db =
        std::clamp(depths.back(), 0.0, kMaximumShapingDepthDb);
  }
  if (depths.size() > 1) {
    settings.shaping_zones = GapShapingZones(settings.protected_bands, depths,
                                             settings.sample_rate_mhz);
  }
  if (request.order > 0) {
    settings.shaping_order =
        std::clamp(request.order, kMinimumShapingOrder, kMaximumShapingOrder);
  }
  return settings;
}

RequantizationRecord RecordFor(const RequantizerSettings& settings) {
  RequantizationRecord record;
  record.enabled = true;
  record.margin_level = settings.margin_level;
  record.protected_bands = settings.protected_bands;
  record.input_bits = settings.input_bits;
  record.adaptive_shaping =
      settings.shaping == RequantizerSettings::Shaping::kAdaptive;
  record.shaping_order = settings.shaping_order;
  record.shaping_depth_db = settings.shaping_depth_db;
  record.shaping_zones = settings.shaping_zones;
  return record;
}

std::string ShapingDepthsProblem(const RequantizerRequest& request) {
  const size_t depths = request.depths_db.size();
  if (depths <= 1) {
    return {};
  }
  const std::vector<ShapingZone> stretches =
      GapShapingZones(RequestedBands(request), {0.0}, request.sample_rate_mhz);
  if (depths <= stretches.size()) {
    return {};
  }

  const std::string rate = ShortDecimal(request.sample_rate_mhz);
  if (stretches.empty()) {
    return std::to_string(depths) + " shaping depths, but at " + rate +
           " Msps the protected bands leave nothing outside them: give one "
           "depth.";
  }
  std::vector<FrequencyBand> where;
  for (const ShapingZone& stretch : stretches) {
    where.push_back({stretch.low_mhz, stretch.high_mhz});
  }
  const std::string most = std::to_string(stretches.size());
  return std::to_string(depths) + " shaping depths, but at " + rate +
         " Msps the protected bands leave " + most +
         (stretches.size() == 1 ? " stretch" : " stretches") +
         " outside them (" + DescribeBands(where) + "): give at most " + most +
         ".";
}

std::vector<double> DesignNoiseTransferFunction(
    const std::vector<FrequencyBand>& bands, double sample_rate_mhz,
    double depth_db, int order) {
  return DesignNoiseTransferFunction(bands, {}, sample_rate_mhz, depth_db,
                                     order);
}

std::vector<double> DesignNoiseTransferFunction(
    const std::vector<FrequencyBand>& bands,
    const std::vector<ShapingZone>& zones, double sample_rate_mhz,
    double depth_db, int order) {
  const auto size = static_cast<size_t>(std::max(order, 0));

  // The weighting on the design grid: 1 in the bands, the depth below it
  // outside them — a zone's own, or the one given.
  std::vector<double> weights(kDesignGrid);
  for (size_t index = 0; index < kDesignGrid; ++index) {
    const double frequency = (static_cast<double>(index) + 0.5) *
                             (sample_rate_mhz / 2.0) /
                             static_cast<double>(kDesignGrid);
    weights[index] =
        InBands(bands, frequency)
            ? 1.0
            : std::pow(10.0, -DepthAt(zones, depth_db, frequency) / 10.0);
  }

  // Its autocorrelation.
  std::vector<double> correlation(size + 1, 0.0);
  for (size_t lag = 0; lag <= size; ++lag) {
    double sum = 0.0;
    for (size_t index = 0; index < kDesignGrid; ++index) {
      const double frequency = (static_cast<double>(index) + 0.5) *
                               (sample_rate_mhz / 2.0) /
                               static_cast<double>(kDesignGrid);
      const double weight = weights[index];
      sum += weight * std::cos(2.0 * std::numbers::pi * frequency /
                               sample_rate_mhz * static_cast<double>(lag));
    }
    correlation[lag] = sum / static_cast<double>(kDesignGrid);
  }
  return LevinsonDurbin(correlation);
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

void ShapingQuantizer::SetCoefficients(
    const std::vector<double>& coefficients) {
  const size_t order = coefficients.empty() ? 0 : coefficients.size() - 1;
  if (order != feedback_.size()) {
    feedback_.assign(order, 0.0);
    history_.assign(2 * order, 0.0);
    position_ = 0;
  }
  std::copy(coefficients.begin() + (coefficients.empty() ? 0 : 1),
            coefficients.end(), feedback_.begin());
}

std::vector<double> ShapingQuantizer::coefficients() const {
  std::vector<double> all{1.0};
  all.insert(all.end(), feedback_.begin(), feedback_.end());
  return all;
}

// --- RfRequantizer -------------------------------------------------------

ShapingQuantizer DecisionQuantizer(const RequantizerSettings& settings,
                                   const RequantizerDecision& decision) {
  const int input_bits =
      std::clamp(settings.input_bits, kMinimumAdaptiveBits + 1, kSampleBits);
  const int bits = std::clamp(input_bits - std::max(decision.lsb_drop, 0),
                              kMinimumAdaptiveBits, input_bits);
  return ShapingQuantizer(bits, decision.coefficients.empty()
                                    ? std::vector<double>{1.0}
                                    : decision.coefficients);
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
      bands_, settings_.shaping_zones, settings_.sample_rate_mhz,
      settings_.shaping_depth_db, settings_.shaping_order);
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

  // Adaptive shaping starts from the fixed design above, which stands until
  // there is a floor to design from.
  if (settings_.shaping == RequantizerSettings::Shaping::kAdaptive) {
    const auto lags = static_cast<size_t>(std::max(settings_.shaping_order, 0));
    lag_cos_.resize((lags + 1) * kBinCount);
    lag_sin_.resize((lags + 1) * kBinCount);
    for (size_t lag = 0; lag <= lags; ++lag) {
      for (size_t bin = 0; bin < kBinCount; ++bin) {
        const double angle = 2.0 * std::numbers::pi *
                             static_cast<double>(bin * lag) /
                             static_cast<double>(kTransformSize);
        lag_cos_[(lag * kBinCount) + bin] = std::cos(angle);
        lag_sin_[(lag * kBinCount) + bin] = std::sin(angle);
      }
    }
    outside_scale_.resize(kBinCount);
    for (size_t bin = 0; bin < kBinCount; ++bin) {
      outside_scale_[bin] = std::pow(
          10.0, DepthAt(settings_.shaping_zones, settings_.shaping_depth_db,
                        static_cast<double>(bin) * bin_width) /
                    10.0);
    }
  }

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

  const auto bins = static_cast<std::ptrdiff_t>(kBinCount);

  // Adaptive shaping smooths the estimates first, within the bands, so that
  // the envelope below takes the minimum of a floor rather than of its noise.
  if (settings_.shaping == RequantizerSettings::Shaping::kAdaptive) {
    const auto reach = static_cast<std::ptrdiff_t>(
        std::ceil(kFloorSmoothingHalfWidthMhz / bin_width));
    smoothed_floor_.resize(kBinCount);
    for (std::ptrdiff_t bin = 0; bin < bins; ++bin) {
      const bool inside = InBand(static_cast<double>(bin) * bin_width);
      double sum = 0.0;
      size_t count = 0;
      for (std::ptrdiff_t other = std::max<std::ptrdiff_t>(0, bin - reach);
           other <= std::min(bins - 1, bin + reach); ++other) {
        const auto index = static_cast<size_t>(other);
        if (!inside || InBand(static_cast<double>(index) * bin_width)) {
          sum += raw_floor_[index];
          ++count;
        }
      }
      smoothed_floor_[static_cast<size_t>(bin)] =
          sum / static_cast<double>(count);
    }
    raw_floor_.swap(smoothed_floor_);
  }

  // The low envelope, within the bands.
  const auto half_width =
      static_cast<std::ptrdiff_t>(std::ceil(kEnvelopeHalfWidthMhz / bin_width));
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

RfRequantizer::SliceCost RfRequantizer::Degradation(
    const Candidate& candidate) const {
  const double bin_width =
      settings_.sample_rate_mhz / static_cast<double>(kTransformSize);
  SliceCost worst;
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
      size_t bins = 0;
      for (size_t bin = 0; bin < kBinCount; ++bin) {
        const double frequency = static_cast<double>(bin) * bin_width;
        if (frequency >= low && frequency < high) {
          added += candidate.variance * candidate.gain[bin];
          floor += floor_[bin];
          ++bins;
        }
      }
      if (floor > 0.0) {
        const double cost = 10.0 * std::log10(1.0 + (added / floor));
        if (cost > worst.db) {
          worst = {cost, low, high, floor / static_cast<double>(bins)};
        }
      }
    }
  }
  return worst;
}

void RfRequantizer::RedesignShaping() {
  const double bin_width =
      settings_.sample_rate_mhz / static_cast<double>(kTransformSize);

  // The wanted noise: the floor itself in the bands, so that every slice of
  // them is raised by the same proportion; and outside them, the depth above
  // the bands' highest floor — a zone's own where one is set — so that as
  // much as can go there does.
  double highest = 0.0;
  for (size_t bin = 0; bin < kBinCount; ++bin) {
    if (InBand(static_cast<double>(bin) * bin_width)) {
      highest = std::max(highest, floor_[bin]);
    }
  }
  if (highest <= kSmallestPower) {
    return;
  }

  // Its weighting's autocorrelation, and A(z) from that as the fixed design
  // does it — over the analysis bins rather than a finer grid, because the
  // floor is known at no finer a resolution.
  const size_t lags = lag_cos_.size() / kBinCount;
  std::vector<double> correlation(lags, 0.0);
  for (size_t bin = 0; bin < kBinCount; ++bin) {
    const double target = InBand(static_cast<double>(bin) * bin_width)
                              ? std::max(floor_[bin], kSmallestPower)
                              : highest * outside_scale_[bin];
    const double weight = 1.0 / target;
    for (size_t lag = 0; lag < lags; ++lag) {
      correlation[lag] += weight * lag_cos_[(lag * kBinCount) + bin];
    }
  }
  const std::vector<double> shaping = LevinsonDurbin(correlation);

  // |A|^2 in every bin, once for all the shaped candidates.
  std::vector<double> gain(kBinCount);
  for (size_t bin = 0; bin < kBinCount; ++bin) {
    double real = 0.0;
    double imaginary = 0.0;
    for (size_t k = 0; k < shaping.size(); ++k) {
      real += shaping[k] * lag_cos_[(k * kBinCount) + bin];
      imaginary -= shaping[k] * lag_sin_[(k * kBinCount) + bin];
    }
    gain[bin] = (real * real) + (imaginary * imaginary);
  }

  for (Candidate& candidate : candidates_) {
    if (candidate.shaped) {
      candidate.gain = gain;
      candidate.quantizer.SetCoefficients(shaping);
    }
  }
}

RequantizerDecision RfRequantizer::Process(int16_t* samples, size_t count,
                                           bool apply) {
  Analyse(samples, count);
  if (settings_.shaping == RequantizerSettings::Shaping::kAdaptive &&
      have_floor_) {
    RedesignShaping();
  }

  const size_t none = candidates_.size();
  size_t wanted = none;
  if (have_floor_) {
    for (size_t index = 0; index < none; ++index) {
      if (Degradation(candidates_[index]).db <= limit_db_) {
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
  if (applied_ < none && Degradation(candidates_[applied_]).db > limit_db_) {
    applied_ = wanted;
  }

  RequantizerDecision decision;

  // Why it went no further: the slice the next more aggressive candidate
  // would have raised most. Candidates run most aggressive first, so that is
  // the one just before the decision — or the last of all when nothing passed.
  if (have_floor_ && applied_ > 0) {
    const SliceCost limit = Degradation(candidates_[applied_ - 1]);
    decision.limit_low_mhz = limit.low_mhz;
    decision.limit_high_mhz = limit.high_mhz;
    decision.limit_floor_lsb = std::sqrt(limit.floor_power) / input_lsb_;
  }

  if (applied_ < none) {
    Candidate& candidate = candidates_[applied_];
    decision.lsb_drop = candidate.lsb_drop;
    decision.shaped = candidate.shaped;
    decision.degradation_db = Degradation(candidate).db;
    decision.coefficients = candidate.quantizer.coefficients();

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
