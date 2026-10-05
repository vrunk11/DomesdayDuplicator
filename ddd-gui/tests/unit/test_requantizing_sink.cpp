/************************************************************************

    test_requantizing_sink.cpp

    T1 tests for the requantiser in front of a writer
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "requantizing_sink.h"
#include "sample_format.h"
#include "sample_sink.h"

namespace ddd::capture {
namespace {

// A writer that keeps what it is handed, converted, and can refuse it.
class KeepingSink : public ISampleSink {
 public:
  explicit KeepingSink(bool stores = true) : stores_(stores) {}

  const char* Name() const override { return "keeping"; }
  bool StoresData() const override { return stores_; }

  bool Write(const uint8_t* /*wire_data*/, size_t /*sample_count*/) override {
    error_ = "wire words were not expected";
    return false;
  }

  bool WriteConverted(const int16_t* samples, size_t sample_count) override {
    if (refuse) {
      error_ = "the disk is full";
      return false;
    }
    kept.insert(kept.end(), samples, samples + sample_count);
    return true;
  }

  bool Finish() override {
    finished = true;
    return true;
  }

  uint64_t BytesWritten() const override { return 0; }
  uint64_t SamplesWritten() const override { return 0; }
  const std::string& LastError() const override { return error_; }

  std::vector<int16_t> kept;
  bool refuse = false;
  bool finished = false;

 private:
  bool stores_;
  std::string error_;
};

std::mt19937 Seeded(uint32_t seed) {
  // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
  return std::mt19937(seed);
}

// Converter codes of Gaussian noise about mid-scale, as wire words.
std::vector<uint8_t> NoiseWire(std::mt19937& generator, double sigma,
                               size_t count) {
  std::normal_distribution<double> noise(0.0, sigma);
  std::vector<uint8_t> wire(count * kBytesPerSample);
  for (size_t index = 0; index < count; ++index) {
    const auto code = static_cast<uint16_t>(
        std::clamp(std::round(512.0 + noise(generator)), 0.0, 1023.0));
    wire[index * 2] = static_cast<uint8_t>(code);
    wire[(index * 2) + 1] = static_cast<uint8_t>(code >> 8);
  }
  return wire;
}

RequantizerSettings Settings() {
  RequantizerSettings settings;
  settings.sample_rate_mhz = 30.0;
  settings.margin_level = 0;
  settings.input_bits = 10;
  return settings;
}

// Fed in buffers that do not line up with its segments, and finished part
// way through one: every sample arrives, in order, the last short segment
// included.
TEST(RequantizingSinkTest, EverySampleReachesTheWriterInOrder) {
  auto keeping = std::make_unique<KeepingSink>();
  KeepingSink* const view = keeping.get();
  auto status = std::make_shared<RequantizationStatus>();
  RequantizingSink sink(std::move(keeping), SampleConversion{}, Settings(),
                        status);
  EXPECT_STREQ(sink.Name(), "keeping+requantised");

  std::mt19937 generator = Seeded(1);
  constexpr size_t kBuffer = 300'000;
  constexpr size_t kBuffers = 9;
  std::vector<uint8_t> all;
  for (size_t buffer = 0; buffer < kBuffers; ++buffer) {
    const std::vector<uint8_t> wire = NoiseWire(generator, 0.6, kBuffer);
    ASSERT_TRUE(sink.Write(wire.data(), kBuffer));
    all.insert(all.end(), wire.begin(), wire.end());
  }
  ASSERT_TRUE(sink.Finish());
  EXPECT_TRUE(view->finished);

  // Quiet enough that nothing was dropped: the writer got exactly the
  // converted samples.
  std::vector<uint8_t> expected(all.size());
  WireToSigned16LittleEndian(all.data(), all.size() / 2, SampleConversion{},
                             expected.data());
  ASSERT_EQ(view->kept.size(), kBuffer * kBuffers);
  for (size_t index = 0; index < view->kept.size(); ++index) {
    const auto sample = static_cast<uint16_t>(view->kept[index]);
    ASSERT_EQ(static_cast<uint8_t>(sample), expected[index * 2]) << index;
    ASSERT_EQ(static_cast<uint8_t>(sample >> 8), expected[(index * 2) + 1])
        << index;
  }

  const RequantizationStatus::Summary summary = status->Read();
  EXPECT_EQ(summary.samples, kBuffer * kBuffers);
  EXPECT_EQ(summary.segments, 3U);
  ASSERT_FALSE(summary.changes.empty());
  EXPECT_EQ(summary.changes.front().first_sample, 0U);
}

// A noisy capture is requantised on its way through, and the record says
// what was done to which samples.
TEST(RequantizingSinkTest, ANoisyCaptureArrivesRequantisedAndRecorded) {
  auto keeping = std::make_unique<KeepingSink>();
  KeepingSink* const view = keeping.get();
  auto status = std::make_shared<RequantizationStatus>();
  RequantizingSink sink(std::move(keeping), SampleConversion{}, Settings(),
                        status);

  std::mt19937 generator = Seeded(2);
  constexpr size_t kSegments = 5;
  for (size_t segment = 0; segment < kSegments; ++segment) {
    const std::vector<uint8_t> wire =
        NoiseWire(generator, 12.0, RfRequantizer::kSegmentSamples);
    ASSERT_TRUE(sink.Write(wire.data(), RfRequantizer::kSegmentSamples));
  }
  ASSERT_TRUE(sink.Finish());

  const RequantizationStatus::Summary summary = status->Read();
  EXPECT_GE(summary.current.lsb_drop, 2);
  EXPECT_EQ(std::accumulate(summary.samples_by_drop.begin(),
                            summary.samples_by_drop.end(), uint64_t{0}),
            kSegments * RfRequantizer::kSegmentSamples);

  // The last segment's samples sit on the step its decision leaves.
  const int step = 64 << summary.current.lsb_drop;
  const size_t last = (kSegments - 1) * RfRequantizer::kSegmentSamples;
  for (size_t index = last; index < view->kept.size(); ++index) {
    ASSERT_EQ(view->kept[index] % step, 0) << index;
  }
}

// In front of a sink that stores nothing, the decisions are made and published
// and nothing is changed.
TEST(RequantizingSinkTest, APreviewDecidesAndChangesNothing) {
  auto keeping = std::make_unique<KeepingSink>(false);
  KeepingSink* const view = keeping.get();
  auto status = std::make_shared<RequantizationStatus>();
  RequantizingSink sink(std::move(keeping), SampleConversion{}, Settings(),
                        status);
  EXPECT_FALSE(sink.StoresData());

  std::mt19937 generator = Seeded(3);
  std::vector<uint8_t> all;
  for (int segment = 0; segment < 4; ++segment) {
    const std::vector<uint8_t> wire =
        NoiseWire(generator, 12.0, RfRequantizer::kSegmentSamples);
    ASSERT_TRUE(sink.Write(wire.data(), RfRequantizer::kSegmentSamples));
    all.insert(all.end(), wire.begin(), wire.end());
  }
  ASSERT_TRUE(sink.Finish());

  EXPECT_GE(status->Read().current.lsb_drop, 2);
  std::vector<uint8_t> expected(all.size());
  WireToSigned16LittleEndian(all.data(), all.size() / 2, SampleConversion{},
                             expected.data());
  ASSERT_EQ(view->kept.size(), all.size() / 2);
  for (size_t index = 0; index < view->kept.size(); index += 4099) {
    const auto sample = static_cast<uint16_t>(view->kept[index]);
    ASSERT_EQ(static_cast<uint8_t>(sample), expected[index * 2]) << index;
  }
}

// A writer that refuses is a capture that fails, with the writer's reason.
TEST(RequantizingSinkTest, AWriterThatRefusesFailsTheCapture) {
  auto keeping = std::make_unique<KeepingSink>();
  keeping->refuse = true;
  RequantizingSink sink(std::move(keeping), SampleConversion{}, Settings(),
                        nullptr);

  std::mt19937 generator = Seeded(4);
  const std::vector<uint8_t> wire =
      NoiseWire(generator, 1.0, RfRequantizer::kSegmentSamples);

  bool refused = false;
  for (int attempt = 0; attempt < 8 && !refused; ++attempt) {
    refused = !sink.Write(wire.data(), RfRequantizer::kSegmentSamples);
  }
  if (!refused) {
    refused = !sink.Finish();
  }
  EXPECT_TRUE(refused);
  EXPECT_EQ(sink.LastError(), "the disk is full");
}

// The DC offset and the bit shift are applied on the way in, so what is
// requantised is what the file would otherwise have held.
TEST(RequantizingSinkTest, TheConversionIsAppliedBeforeTheRequantiser) {
  auto keeping = std::make_unique<KeepingSink>();
  KeepingSink* const view = keeping.get();
  const SampleConversion conversion{5, 1};
  RequantizerSettings settings = Settings();
  settings.input_bits = 9;
  RequantizingSink sink(std::move(keeping), conversion, settings, nullptr);

  std::mt19937 generator = Seeded(5);
  const std::vector<uint8_t> wire = NoiseWire(generator, 0.6, 50'000);
  ASSERT_TRUE(sink.Write(wire.data(), 50'000));
  ASSERT_TRUE(sink.Finish());

  ASSERT_EQ(view->kept.size(), 50'000U);
  for (size_t index = 0; index < view->kept.size(); index += 997) {
    const auto code = static_cast<int32_t>(
        static_cast<uint16_t>(wire[index * 2]) |
        static_cast<uint16_t>(static_cast<uint16_t>(wire[(index * 2) + 1])
                              << 8));
    EXPECT_EQ(view->kept[index], ToConvertedSigned16Bit(code, conversion))
        << index;
  }
}

}  // namespace
}  // namespace ddd::capture
