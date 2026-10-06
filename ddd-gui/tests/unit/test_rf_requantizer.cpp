/************************************************************************

    test_rf_requantizer.cpp

    T1 tests for re-quantising a capture to what its noise allows
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

#include "rf_requantizer.h"

namespace ddd::capture {
namespace {

constexpr double kRateMhz = 30.0;
constexpr double kInputLsb = 64.0;

// Fixed seeds, so that every run tests the same signal.
std::mt19937 Seeded(uint32_t seed) {
  // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
  return std::mt19937(seed);
}

// One segment of a 10-bit converter's codes, left-aligned: Gaussian noise of
// `sigma` codes about mid-scale.
std::vector<int16_t> NoiseSegment(
    std::mt19937& generator, double sigma,
    size_t count = RfRequantizer::kSegmentSamples) {
  std::normal_distribution<double> noise(0.0, sigma);
  std::vector<int16_t> samples(count);
  for (int16_t& sample : samples) {
    const double code = std::clamp(std::round(noise(generator)), -512.0, 511.0);
    sample = static_cast<int16_t>(code * kInputLsb);
  }
  return samples;
}

RequantizerSettings Settings(int margin_level = kDefaultMarginLevel) {
  RequantizerSettings settings;
  settings.sample_rate_mhz = kRateMhz;
  settings.margin_level = margin_level;
  settings.input_bits = 10;
  return settings;
}

TEST(RfRequantizerTest, TheMarginLevelsRunFromAggressiveToUltraSafe) {
  EXPECT_DOUBLE_EQ(MarginLimitDb(0), 1.0);
  EXPECT_DOUBLE_EQ(MarginLimitDb(kDefaultMarginLevel), 0.2);
  EXPECT_DOUBLE_EQ(MarginLimitDb(4), 0.05);
  EXPECT_DOUBLE_EQ(MarginHoldSeconds(0), 0.0);
  EXPECT_DOUBLE_EQ(MarginHoldSeconds(4), 1.0);
  EXPECT_STREQ(MarginLevelName(2), "safe");

  // Out of range is held to the nearest level rather than read past the end.
  EXPECT_DOUBLE_EQ(MarginLimitDb(-3), MarginLimitDb(0));
  EXPECT_DOUBLE_EQ(MarginLimitDb(9), MarginLimitDb(4));
}

TEST(RfRequantizerTest, TheDefaultBandIsTheLaserDiscsRf) {
  const std::vector<FrequencyBand> wide = DefaultProtectedBands(40.0);
  ASSERT_EQ(wide.size(), 1U);
  EXPECT_DOUBLE_EQ(wide[0].low_mhz, 0.0);
  EXPECT_DOUBLE_EQ(wide[0].high_mhz, 14.0);

  // At a rate that cannot hold all of it, short of the Nyquist limit.
  EXPECT_DOUBLE_EQ(DefaultProtectedBands(20.0)[0].high_mhz, 8.5);
}

TEST(RfRequantizerTest, BandsAreDescribedAsACaptureRecordsThem) {
  EXPECT_EQ(DescribeBands(DefaultProtectedBands(30.0)), "0-13.5 MHz");
  EXPECT_EQ(DescribeBands({{0.0, 3.0}, {4.25, 14.0}}), "0-3, 4.25-14 MHz");
  EXPECT_EQ(DescribeBands({}), "");
}

// Typed as a person types them, and read the same in any locale.
TEST(RfRequantizerTest, BandsAreReadAsTyped) {
  const std::vector<FrequencyBand> one = ParseBands("0-12");
  ASSERT_EQ(one.size(), 1U);
  EXPECT_DOUBLE_EQ(one[0].low_mhz, 0.0);
  EXPECT_DOUBLE_EQ(one[0].high_mhz, 12.0);

  const std::vector<FrequencyBand> two = ParseBands(" 0:1.9, 2.1-13.5 MHz");
  ASSERT_EQ(two.size(), 2U);
  EXPECT_DOUBLE_EQ(two[0].high_mhz, 1.9);
  EXPECT_DOUBLE_EQ(two[1].low_mhz, 2.1);
  EXPECT_DOUBLE_EQ(two[1].high_mhz, 13.5);

  // What a capture records reads back as the same bands.
  EXPECT_EQ(ParseBands(DescribeBands(two)), two);
}

TEST(RfRequantizerTest, BandsThatAreNotBandsAreRefused) {
  for (const char* text : {"", "12", "0-", "-12", "12-0", "5-5", "0-12,",
                           "0-12;13-14", "0,5-12", "a-b", "0-1e1"}) {
    EXPECT_TRUE(ParseBands(text).empty()) << text;
  }
}

// Asked for above the Nyquist limit, a band stops there; one entirely above
// it is gone.
TEST(RfRequantizerTest, BandsAreCutAtTheNyquistLimit) {
  const std::vector<FrequencyBand> usable =
      BandsWithin({{0.0, 12.0}, {13.0, 20.0}, {16.0, 18.0}}, 30.0);
  ASSERT_EQ(usable.size(), 2U);
  EXPECT_DOUBLE_EQ(usable[0].high_mhz, 12.0);
  EXPECT_DOUBLE_EQ(usable[1].low_mhz, 13.0);
  EXPECT_DOUBLE_EQ(usable[1].high_mhz, 15.0);
  EXPECT_TRUE(BandsWithin({{16.0, 18.0}}, 30.0).empty());
}

// The shaping filter puts the noise where the bands are not.
TEST(RfRequantizerTest, TheShapingFilterIsQuietInTheBand) {
  const std::vector<FrequencyBand> bands = {{0.0, 12.0}};
  const std::vector<double> a =
      DesignNoiseTransferFunction(bands, kRateMhz, 10.0, 16);
  ASSERT_EQ(a.size(), 17U);
  EXPECT_DOUBLE_EQ(a[0], 1.0);

  double inside = 0.0;
  double outside = 0.0;
  int inside_count = 0;
  int outside_count = 0;
  for (int quarter = 1; quarter < 60; ++quarter) {
    const double frequency = 0.25 * static_cast<double>(quarter);
    const double gain = NoiseTransferGainSquared(a, frequency, kRateMhz);
    if (frequency < 11.0) {
      inside += gain;
      ++inside_count;
    } else if (frequency > 13.0) {
      outside += gain;
      ++outside_count;
    }
  }
  EXPECT_LT(
      10.0 * std::log10((inside / inside_count) / (outside / outside_count)),
      -6.0);
}

TEST(RfRequantizerTest, PlainRoundingIsToNearestTiesToEven) {
  ShapingQuantizer quantizer(8, {1.0});
  EXPECT_DOUBLE_EQ(quantizer.step(), 256.0);

  EXPECT_EQ(quantizer.Quantize(127), 0);
  EXPECT_EQ(quantizer.Quantize(129), 256);
  EXPECT_EQ(quantizer.Quantize(128), 0);    // tie, to even
  EXPECT_EQ(quantizer.Quantize(384), 512);  // tie, to even
  EXPECT_EQ(quantizer.Quantize(-128), 0);
  EXPECT_EQ(quantizer.Quantize(-384), -512);
}

TEST(RfRequantizerTest, TheTopIsHeldOnAStepAndCounted) {
  ShapingQuantizer quantizer(8, {1.0});
  EXPECT_EQ(quantizer.Quantize(32767), 32768 - 256);
  EXPECT_EQ(quantizer.Quantize(-32768), -32768);
  EXPECT_EQ(quantizer.clipped(), 1U);
}

// The error's power at one frequency, summed over 1,024-sample blocks.
double ErrorPower(const std::vector<double>& errors, double frequency_mhz) {
  constexpr size_t kBlock = 1024;
  double total = 0.0;
  for (size_t start = 0; start + kBlock <= errors.size(); start += kBlock) {
    std::complex<double> sum = 0.0;
    for (size_t i = 0; i < kBlock; ++i) {
      sum += errors[start + i] *
             std::polar(1.0, -2.0 * std::numbers::pi * frequency_mhz /
                                 kRateMhz * static_cast<double>(i));
    }
    total += std::norm(sum);
  }
  return total;
}

// Shaped, the output is still on the step, and the error has moved: quieter
// than plain rounding leaves it everywhere in the band, louder above it. With
// the band four-fifths of the spectrum there is little room to move it to,
// so the gain inside is a couple of dB — which is what a 30 Msps capture of a
// 14 MHz signal has to work with.
TEST(RfRequantizerTest, ShapingMovesTheErrorOutOfTheBand) {
  const std::vector<double> a =
      DesignNoiseTransferFunction({{0.0, 12.0}}, kRateMhz, 10.0, 16);
  ShapingQuantizer shaped(7, a);
  ShapingQuantizer plain(7, {1.0});

  std::mt19937 generator = Seeded(3);
  const std::vector<int16_t> input = NoiseSegment(generator, 40.0, 65536);

  std::vector<double> shaped_errors;
  std::vector<double> plain_errors;
  for (const int16_t sample : input) {
    const int16_t s = shaped.Quantize(sample);
    const int16_t p = plain.Quantize(sample);
    ASSERT_EQ(s % 512, 0);
    ASSERT_EQ(p % 512, 0);
    shaped_errors.push_back(static_cast<double>(s - sample));
    plain_errors.push_back(static_cast<double>(p - sample));
  }

  const auto ratio_db = [&](double frequency) {
    return 10.0 * std::log10(ErrorPower(shaped_errors, frequency) /
                             ErrorPower(plain_errors, frequency));
  };
  for (const double inside : {1.0, 4.0, 8.0, 11.0}) {
    EXPECT_LT(ratio_db(inside), -1.0) << inside << " MHz";
  }
  for (const double outside : {13.0, 14.5}) {
    EXPECT_GT(ratio_db(outside), 5.0) << outside << " MHz";
  }
}

// A quiet capture keeps every bit: dropping one would be audible against a
// floor this low.
TEST(RfRequantizerTest, AQuietCaptureIsLeftAlone) {
  RfRequantizer requantizer(Settings());
  std::mt19937 generator = Seeded(1);

  RequantizerDecision decision;
  for (int segment = 0; segment < 4; ++segment) {
    std::vector<int16_t> samples = NoiseSegment(generator, 0.6);
    const std::vector<int16_t> original = samples;
    decision = requantizer.Process(samples.data(), samples.size(), true);
    EXPECT_EQ(decision.lsb_drop, 0);
    EXPECT_EQ(samples, original);
  }
}

// A noisy one gives up bits its noise buries, and only within the limit — and
// every sample sits on the step that leaves.
TEST(RfRequantizerTest, ANoisyCaptureDropsWhatItsNoiseBuries) {
  RfRequantizer requantizer(Settings(0));
  std::mt19937 generator = Seeded(2);

  RequantizerDecision decision;
  std::vector<int16_t> samples;
  for (int segment = 0; segment < 4; ++segment) {
    samples = NoiseSegment(generator, 12.0);
    decision = requantizer.Process(samples.data(), samples.size(), true);
  }
  EXPECT_GE(decision.lsb_drop, 2);
  EXPECT_LE(decision.degradation_db, MarginLimitDb(0));
  EXPECT_NEAR(requantizer.noise_floor_lsb(), 12.0, 3.0);

  const auto step = static_cast<int>(kInputLsb) << decision.lsb_drop;
  for (const int16_t sample : samples) {
    ASSERT_EQ(sample % step, 0) << sample;
  }
}

// More careful at once when the noise falls; more aggressive only once the
// history agrees and the hold has passed.
TEST(RfRequantizerTest, CarefulAtOnceAggressiveOnlyAfterTheHold) {
  RfRequantizer requantizer(Settings(1));
  std::mt19937 generator = Seeded(4);

  std::vector<int16_t> samples = NoiseSegment(generator, 0.6);
  requantizer.Process(samples.data(), samples.size(), false);

  samples = NoiseSegment(generator, 12.0);
  EXPECT_EQ(requantizer.Process(samples.data(), samples.size(), false).lsb_drop,
            0);

  RequantizerDecision decision;
  for (int segment = 0; segment < 30; ++segment) {
    samples = NoiseSegment(generator, 12.0);
    decision = requantizer.Process(samples.data(), samples.size(), false);
  }
  EXPECT_GT(decision.lsb_drop, 0);

  // The noise falls away, and the next segment is protected at once.
  samples = NoiseSegment(generator, 0.6);
  decision = requantizer.Process(samples.data(), samples.size(), false);
  EXPECT_EQ(decision.lsb_drop, 0);
}

// A preview decides exactly as a capture would, and leaves the samples alone.
TEST(RfRequantizerTest, APreviewDecidesAsACaptureWouldAndChangesNothing) {
  RfRequantizer capturing(Settings(0));
  RfRequantizer previewing(Settings(0));
  std::mt19937 generator = Seeded(5);

  for (int segment = 0; segment < 6; ++segment) {
    std::vector<int16_t> applied =
        NoiseSegment(generator, segment < 3 ? 1 : 12);
    std::vector<int16_t> previewed = applied;
    const std::vector<int16_t> original = applied;

    EXPECT_EQ(previewing.Process(previewed.data(), previewed.size(), false),
              capturing.Process(applied.data(), applied.size(), true))
        << segment;
    EXPECT_EQ(previewed, original);
  }
}

// What the signal panels show of a decision is what the file got: the first
// segment, requantised from a fresh start, matches a quantiser built for the
// decision alone, sample for sample. And nothing dropped is nothing changed.
TEST(RfRequantizerTest, ADecisionsQuantiserIsTheOneTheFileGot) {
  // 2.77 and 10.8 are shaped here, the others plain; equality is what is
  // checked, whatever a library's random numbers make of them.
  // Adaptive shaping designs its filter from the floor, segment by segment,
  // and the decision carries the one it used.
  for (const bool adaptive : {false, true}) {
    for (const double sigma : {2.77, 6.0, 10.8, 30.0}) {
      RequantizerSettings settings = Settings(0);
      if (adaptive) {
        UseAdaptiveShaping(settings);
      }
      RfRequantizer requantizer(settings);
      std::mt19937 generator = Seeded(8);
      std::vector<int16_t> written = NoiseSegment(generator, sigma);
      std::vector<int16_t> shown = written;

      const RequantizerDecision decision =
          requantizer.Process(written.data(), written.size(), true);
      ShapingQuantizer quantizer = DecisionQuantizer(settings, decision);
      quantizer.QuantizeInPlace(shown.data(), shown.size());
      EXPECT_EQ(shown, written)
          << (adaptive ? "adaptive" : "fixed") << ", sigma " << sigma << ", "
          << decision.lsb_drop << (decision.shaped ? " shaped" : "");
    }
  }

  std::mt19937 generator = Seeded(9);
  const std::vector<int16_t> original = NoiseSegment(generator, 12.0, 4096);
  for (const bool shaped : {false, true}) {
    RequantizerDecision nothing;
    nothing.coefficients =
        shaped ? DesignNoiseTransferFunction(DefaultProtectedBands(kRateMhz),
                                             kRateMhz, 10.0, 16)
               : std::vector<double>{1.0};
    std::vector<int16_t> untouched = original;
    ShapingQuantizer quantizer = DecisionQuantizer(Settings(), nothing);
    quantizer.QuantizeInPlace(untouched.data(), untouched.size());
    EXPECT_EQ(untouched, original) << shaped;
  }
}

// A floor that falls towards the top of the band, as a player's RF does: flat
// noise through a low-pass that starts to cut at about 9 MHz. The fixed filter
// adds the same noise everywhere in the band, so the quiet top decides; the
// adaptive one puts the noise where the floor can take it, and drops more at
// the same margin, without raising the quiet top more than the margin allows.
std::vector<int16_t> SlopedNoise(std::mt19937& generator, size_t count) {
  // A 15-tap windowed-sinc low-pass at 10.5 MHz, over noise loud enough that
  // the floor below it is several codes, plus a little flat noise so the top
  // of the band is quiet but not empty.
  constexpr int kTaps = 15;
  constexpr double kCutoff = 10.5 / kRateMhz;
  std::array<double, kTaps> taps{};
  for (int t = 0; t < kTaps; ++t) {
    constexpr int kCentre = kTaps / 2;
    const double centred = static_cast<double>(t - kCentre);
    const double sinc =
        centred == 0.0 ? 2.0 * kCutoff
                       : std::sin(2.0 * std::numbers::pi * kCutoff * centred) /
                             (std::numbers::pi * centred);
    const double window =
        0.54 - 0.46 * std::cos(2.0 * std::numbers::pi * static_cast<double>(t) /
                               static_cast<double>(kTaps - 1));
    taps[static_cast<size_t>(t)] = sinc * window;
  }
  std::normal_distribution<double> loud(0.0, 4.0);
  std::normal_distribution<double> quiet(0.0, 0.9);
  std::vector<double> white(count + kTaps);
  for (double& value : white) {
    value = loud(generator);
  }
  std::vector<int16_t> samples(count);
  for (size_t index = 0; index < count; ++index) {
    double sum = quiet(generator);
    for (size_t t = 0; t < kTaps; ++t) {
      sum += taps[t] * white[index + t];
    }
    samples[index] = static_cast<int16_t>(
        std::clamp(std::round(sum), -512.0, 511.0) * kInputLsb);
  }
  return samples;
}

TEST(RfRequantizerTest, AdaptiveShapingFollowsASlopingFloor) {
  RequantizerSettings fixed_settings = Settings(0);
  RequantizerSettings adaptive_settings = Settings(0);
  UseAdaptiveShaping(adaptive_settings);
  RfRequantizer fixed(fixed_settings);
  RfRequantizer adaptive(adaptive_settings);

  std::mt19937 generator = Seeded(11);
  RequantizerDecision fixed_decision;
  RequantizerDecision adaptive_decision;
  std::vector<int16_t> original;
  std::vector<int16_t> shaped;
  for (int segment = 0; segment < 4; ++segment) {
    original = SlopedNoise(generator, RfRequantizer::kSegmentSamples);
    std::vector<int16_t> copy = original;
    fixed_decision = fixed.Process(copy.data(), copy.size(), false);
    shaped = original;
    adaptive_decision = adaptive.Process(shaped.data(), shaped.size(), true);
  }

  EXPECT_GT(adaptive_decision.lsb_drop, fixed_decision.lsb_drop)
      << "adaptive " << adaptive_decision.lsb_drop << ", fixed "
      << fixed_decision.lsb_drop;
  EXPECT_LE(adaptive_decision.degradation_db, MarginLimitDb(0));

  // The added noise follows the floor: well below it at the quiet top of the
  // band, where plain rounding would be as loud as anywhere.
  std::vector<double> errors(shaped.size());
  for (size_t index = 0; index < shaped.size(); ++index) {
    errors[index] = static_cast<double>(shaped[index] - original[index]);
  }
  EXPECT_LT(ErrorPower(errors, 12.5), ErrorPower(errors, 5.0) / 2.0);
}

// A quantiser handed new coefficients of the same order keeps the errors it
// has made, which belong to the stream; of another order, it starts again.
TEST(RfRequantizerTest, NewCoefficientsKeepTheHistoryOfTheSameOrder) {
  const std::vector<double> first = {1.0, -0.5};
  const std::vector<double> second = {1.0, 0.25};

  ShapingQuantizer changed(8, first);
  ShapingQuantizer fresh(8, second);
  changed.Quantize(1000);
  changed.SetCoefficients(second);
  EXPECT_EQ(changed.coefficients(), second);
  // The history carried over makes the next sample differ from a fresh start:
  // 1000 rounds to 1024 at a step of 256, and the error of +24 fed back at
  // 0.25 takes 1150 past the half-way point to 1280, where alone it rounds to
  // 1024.
  EXPECT_EQ(changed.Quantize(1150), 1280);
  EXPECT_EQ(fresh.Quantize(1150), 1024);

  ShapingQuantizer reordered(8, first);
  reordered.Quantize(1000);
  reordered.SetCoefficients({1.0, 0.25, 0.1});
  ShapingQuantizer reordered_fresh(8, {1.0, 0.25, 0.1});
  EXPECT_EQ(reordered.Quantize(1000), reordered_fresh.Quantize(1000));
}

// A noisy capture with one quiet notch in the band — x[n] + x[n-2] has a zero
// at a quarter of the rate, 7.5 MHz at 30 Msps — is held back by that notch
// alone, and the decision says so: a slice within the floor's 2 MHz low
// envelope of it, and a floor there far below the band's median. Which slice
// of those depends on the candidate: shaped noise rises with frequency.
TEST(RfRequantizerTest, TheDecisionNamesTheSliceThatHeldItBack) {
  RfRequantizer requantizer(Settings(0));
  std::mt19937 generator = Seeded(10);
  const std::vector<int16_t> white =
      NoiseSegment(generator, 10.0, RfRequantizer::kSegmentSamples + 2);
  std::vector<int16_t> notched(RfRequantizer::kSegmentSamples);
  for (size_t index = 0; index < notched.size(); ++index) {
    notched[index] = static_cast<int16_t>(
        (static_cast<int32_t>(white[index + 2]) + white[index]) / 2);
  }

  const RequantizerDecision decision =
      requantizer.Process(notched.data(), notched.size(), false);
  EXPECT_DOUBLE_EQ(decision.limit_high_mhz - decision.limit_low_mhz, 1.0);
  EXPECT_GE(decision.limit_high_mhz, 7.5 - 2.0);
  EXPECT_LE(decision.limit_low_mhz, 7.5 + 2.0);
  EXPECT_LT(decision.limit_floor_lsb, requantizer.noise_floor_lsb() / 2.0)
      << decision.limit_floor_lsb << " against "
      << requantizer.noise_floor_lsb();

  // White noise as loud has no such slice to stop at, and goes further.
  RfRequantizer flat(Settings(0));
  std::vector<int16_t> plain(white.begin(),
                             white.begin() + RfRequantizer::kSegmentSamples);
  EXPECT_GT(flat.Process(plain.data(), plain.size(), false).lsb_drop,
            decision.lsb_drop);
}

// The end of a stream is shorter than a segment, and too short to analyse: the
// decision stands on the floor already known.
TEST(RfRequantizerTest, AShortLastSegmentIsDecidedOnTheFloorAlreadyKnown) {
  RfRequantizer requantizer(Settings(0));
  std::mt19937 generator = Seeded(6);

  RequantizerDecision decision;
  for (int segment = 0; segment < 4; ++segment) {
    std::vector<int16_t> samples = NoiseSegment(generator, 12.0);
    decision = requantizer.Process(samples.data(), samples.size(), true);
  }
  std::vector<int16_t> tail = NoiseSegment(generator, 12.0, 1000);
  EXPECT_EQ(requantizer.Process(tail.data(), tail.size(), true).lsb_drop,
            decision.lsb_drop);
}

}  // namespace
}  // namespace ddd::capture
