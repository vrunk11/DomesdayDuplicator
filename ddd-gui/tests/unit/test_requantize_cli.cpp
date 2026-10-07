/************************************************************************

    test_requantize_cli.cpp

    T1 tests for ddd-requantize, requantising a capture offline
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "byte_stream.h"
#include "capture_provenance.h"
#include "capture_reader.h"
#include "requantize_cli.h"
#include "requantizing_sink.h"
#include "rf_requantizer.h"
#include "sample_format.h"
#include "sample_sink.h"

namespace ddd::capture {
namespace {

// Standard output, kept.
class KeptStream : public IByteStream {
 public:
  bool Write(const uint8_t* data, size_t size) override {
    bytes.insert(bytes.end(), data, data + size);
    return true;
  }
  std::vector<uint8_t> bytes;
};

// The writer behind the capture's own RequantizingSink: keeps what it is
// handed.
class KeepingSink : public ISampleSink {
 public:
  const char* Name() const override { return "keeping"; }
  bool Write(const uint8_t* /*wire_data*/, size_t /*sample_count*/) override {
    return false;
  }
  bool WriteConverted(const int16_t* samples, size_t sample_count) override {
    kept.insert(kept.end(), samples, samples + sample_count);
    return true;
  }
  bool Finish() override { return true; }
  uint64_t BytesWritten() const override { return 0; }
  uint64_t SamplesWritten() const override { return 0; }
  const std::string& LastError() const override { return error_; }

  std::vector<int16_t> kept;

 private:
  std::string error_;
};

std::mt19937 Seeded(uint32_t seed) {
  // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
  return std::mt19937(seed);
}

// Two and a half segments of converter codes: noise loud enough to drop bits,
// under a carrier, so that the decisions have something to decide.
std::vector<uint16_t> Codes() {
  std::mt19937 generator = Seeded(7);
  std::normal_distribution<double> noise(0.0, 6.0);
  const size_t count = (RfRequantizer::kSegmentSamples * 5) / 2;
  std::vector<uint16_t> codes(count);
  for (size_t index = 0; index < count; ++index) {
    const double carrier = 120.0 * std::sin(0.55 * static_cast<double>(index));
    codes[index] = static_cast<uint16_t>(std::clamp(
        std::round(512.0 + carrier + noise(generator)), 0.0, 1023.0));
  }
  return codes;
}

std::vector<uint8_t> Wire(const std::vector<uint16_t>& codes) {
  std::vector<uint8_t> wire(codes.size() * kBytesPerSample);
  for (size_t index = 0; index < codes.size(); ++index) {
    wire[index * 2] = static_cast<uint8_t>(codes[index] & 0xFFU);
    wire[(index * 2) + 1] = static_cast<uint8_t>(codes[index] >> 8);
  }
  return wire;
}

// The capture as an uncompressed file holds it: converted, no offset, no
// shift.
std::vector<uint8_t> StoredBytes(const std::vector<int16_t>& samples) {
  std::vector<uint8_t> bytes(samples.size() * kBytesPerSample);
  for (size_t index = 0; index < samples.size(); ++index) {
    const auto word = static_cast<uint16_t>(samples[index]);
    bytes[index * 2] = static_cast<uint8_t>(word & 0xFFU);
    bytes[(index * 2) + 1] = static_cast<uint8_t>(word >> 8);
  }
  return bytes;
}

std::vector<int16_t> Converted(const std::vector<uint16_t>& codes) {
  std::vector<int16_t> samples(codes.size());
  for (size_t index = 0; index < codes.size(); ++index) {
    samples[index] = ToConvertedSigned16Bit(static_cast<int32_t>(codes[index]),
                                            SampleConversion{});
  }
  return samples;
}

std::vector<uint8_t> ReadAll(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

void WriteAll(const std::filesystem::path& path,
              const std::vector<uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
}

class RequantizeCliTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const ::testing::TestInfo* const info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    directory_ = std::filesystem::temp_directory_path() /
                 (std::string("ddd-requantize-test-") + info->name());
    std::filesystem::remove_all(directory_);
    std::filesystem::create_directories(directory_);
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }

  std::string Path(const std::string& name) const {
    return (directory_ / name).string();
  }

  // Run the tool; what it said is kept in out_ and error_.
  int Run(const std::vector<std::string>& args,
          const std::string& standard_input = {}) {
    out_.str({});
    error_.str({});
    std::istringstream input(standard_input);
    return RunRequantizeCli(args, input, standard_output_, out_, error_);
  }

  std::filesystem::path directory_;
  KeptStream standard_output_;
  std::ostringstream out_;
  std::ostringstream error_;
};

// The settings these tests requantise with: aggressive, adaptive, with a depth
// for each of the two stretches bands 2-14 leave at 30 Msps.
std::vector<std::string> Choices() {
  return {"--rate",   "30",     "--margin", "0",       "--shaping",
          "adaptive", "--band", "2-14",     "--depth", "10,40"};
}

RequantizerRequest ChoicesRequest() {
  RequantizerRequest request;
  request.sample_rate_mhz = 30.0;
  request.margin_level = 0;
  request.bands = {{2.0, 14.0}};
  request.input_bits = kConverterBits;
  request.adaptive = true;
  request.depths_db = {10.0, 40.0};
  return request;
}

TEST(RequantizeCliParseTest, EveryOptionIsTakenAsGiven) {
  const RequantizeCliOptions options = ParseRequantizeCliOptions(
      {"--rate", "32.5", "--margin=1", "--band", "2-14", "--shaping",
       "adaptive", "--depth", "10,40", "--order", "48", "--input-bits", "9",
       "--compression", "5", "--log", "decisions.csv", "in.flac", "out.flac"});
  ASSERT_TRUE(options.problem.empty()) << options.problem;
  EXPECT_EQ(options.rate_mhz, std::optional<double>(32.5));
  EXPECT_EQ(options.margin_level, 1);
  EXPECT_EQ(options.bands, (std::vector<FrequencyBand>{{2.0, 14.0}}));
  EXPECT_TRUE(options.adaptive);
  EXPECT_EQ(options.depths_db, (std::vector<double>{10.0, 40.0}));
  EXPECT_EQ(options.order, 48);
  EXPECT_EQ(options.input_bits, std::optional<int>(9));
  EXPECT_EQ(options.compression_level, 5);
  EXPECT_EQ(options.log_path, "decisions.csv");
  EXPECT_EQ(options.input_path, "in.flac");
  EXPECT_EQ(options.output_path, "out.flac");
}

TEST(RequantizeCliParseTest, WhatIsNotAnOptionOrAValueIsRefused) {
  for (const std::vector<std::string>& args :
       std::vector<std::vector<std::string>>{
           {},
           {"a.s16", "b.s16", "c.s16"},
           {"--rate", "0", "a.s16"},
           {"--rate", "fast", "a.s16"},
           {"--margin", "5", "a.s16"},
           {"--band", "14-2", "a.s16"},
           {"--shaping", "sideways", "a.s16"},
           {"--depth", "41", "a.s16"},
           {"--order", "65", "a.s16"},
           {"--input-bits", "4", "a.s16"},
           {"--compression", "9", "a.s16"},
           {"--rate"},
           {"--loud", "a.s16"},
           {"a.s16", "a.s16"},
       }) {
    EXPECT_FALSE(ParseRequantizeCliOptions(args).problem.empty())
        << (args.empty() ? std::string("(nothing)") : args.front());
  }
}

// The property the tool exists for: a capture requantised offline holds
// exactly the samples the capture application writes when it requantises the
// same samples as they arrive — the same requantiser, given its settings by
// the same translation, fed the same segments.
TEST_F(RequantizeCliTest,
       AFileRequantisedHereIsTheOneACaptureWouldHaveWritten) {
  const std::vector<uint16_t> codes = Codes();

  auto keeping = std::make_unique<KeepingSink>();
  KeepingSink* const capture = keeping.get();
  RequantizingSink sink(std::move(keeping), SampleConversion{},
                        SettingsFor(ChoicesRequest()),
                        std::make_shared<RequantizationStatus>());
  const std::vector<uint8_t> wire = Wire(codes);
  // In buffers that do not line up with the segments, as a capture's do.
  constexpr size_t kBuffer = 300'000;
  for (size_t done = 0; done < codes.size(); done += kBuffer) {
    const size_t count = std::min(kBuffer, codes.size() - done);
    ASSERT_TRUE(sink.Write(wire.data() + (done * kBytesPerSample), count));
  }
  ASSERT_TRUE(sink.Finish());
  ASSERT_EQ(capture->kept.size(), codes.size());

  WriteAll(Path("in.s16"), StoredBytes(Converted(codes)));
  std::vector<std::string> args = Choices();
  args.push_back(Path("in.s16"));
  args.push_back(Path("out.s16"));
  ASSERT_EQ(Run(args), kRequantizeCliSuccess) << error_.str();

  EXPECT_EQ(ReadAll(Path("out.s16")), StoredBytes(capture->kept));

  // And it did something: bits went.
  EXPECT_NE(ReadAll(Path("out.s16")), ReadAll(Path("in.s16")));
  EXPECT_NE(out_.str().find("Bits dropped:"), std::string::npos) << out_.str();
}

// From a pipe to a pipe: the samples are the file's, and nothing but samples
// reaches standard output — the report goes to standard error instead.
TEST_F(RequantizeCliTest, StandardInputToStandardOutputIsTheSame) {
  const std::vector<uint8_t> stored = StoredBytes(Converted(Codes()));
  WriteAll(Path("in.s16"), stored);
  std::vector<std::string> to_file = Choices();
  to_file.push_back(Path("in.s16"));
  to_file.push_back(Path("out.s16"));
  ASSERT_EQ(Run(to_file), kRequantizeCliSuccess) << error_.str();

  std::vector<std::string> piped = Choices();
  piped.emplace_back("-");
  piped.emplace_back("-");
  ASSERT_EQ(Run(piped, std::string(stored.begin(), stored.end())),
            kRequantizeCliSuccess)
      << error_.str();
  EXPECT_EQ(standard_output_.bytes, ReadAll(Path("out.s16")));
  EXPECT_TRUE(out_.str().empty()) << out_.str();
  EXPECT_NE(error_.str().find("Bits dropped:"), std::string::npos);
}

// To FLAC, stamped as a capture is, and read back as the samples the
// uncompressed output holds. A FLAC capture then says its own rate.
TEST_F(RequantizeCliTest, AFlacOutputHoldsTheSameSamplesAndSaysHowItWasMade) {
  WriteAll(Path("in.s16"), StoredBytes(Converted(Codes())));
  std::vector<std::string> args = Choices();
  args.push_back(Path("in.s16"));
  args.push_back(Path("out.s16"));
  ASSERT_EQ(Run(args), kRequantizeCliSuccess) << error_.str();
  args.back() = Path("out.flac");
  ASSERT_EQ(Run(args), kRequantizeCliSuccess) << error_.str();

  CaptureReader reader;
  std::string problem;
  ASSERT_TRUE(
      reader.Open(Path("out.flac"), CaptureReader::Format::kFlac, problem))
      << problem;
  std::vector<int16_t> samples;
  std::vector<int16_t> block;
  bool ended = false;
  while (!ended) {
    ASSERT_TRUE(reader.ReadSigned(block, 1 << 16, ended));
    samples.insert(samples.end(), block.begin(), block.end());
  }
  EXPECT_EQ(StoredBytes(samples), ReadAll(Path("out.s16")));

  const auto tag = [&reader](const std::string& name) -> std::string {
    for (const auto& [key, value] : reader.Tags()) {
      if (key == name) {
        return value;
      }
    }
    return {};
  };
  EXPECT_EQ(tag(kTagRequantization), "dynamic");
  EXPECT_EQ(tag(kTagRequantizationBands), "2-14 MHz");
  EXPECT_EQ(tag(kTagSampleRate), "30000000");
  EXPECT_EQ(tag(kTagEncoder), "ddd-requantize");

  // Its rate is in its tags, so it needs no --rate; and having been
  // requantised already, it is said so.
  ASSERT_EQ(Run({Path("out.flac")}), kRequantizeCliSuccess) << error_.str();
  EXPECT_NE(out_.str().find("was requantised"), std::string::npos)
      << out_.str();
}

// Off writes the reference: every sample as it was, through the same writer,
// and says so.
TEST_F(RequantizeCliTest, OffWritesTheSamplesAsTheyAre) {
  EXPECT_FALSE(
      ParseRequantizeCliOptions({"--margin", "off", "in.s16"}).requantize);

  WriteAll(Path("in.s16"), StoredBytes(Converted(Codes())));
  ASSERT_EQ(
      Run({"--rate", "30", "--margin", "off", Path("in.s16"), Path("out.s16")}),
      kRequantizeCliSuccess)
      << error_.str();
  EXPECT_EQ(ReadAll(Path("out.s16")), ReadAll(Path("in.s16")));
  EXPECT_NE(out_.str().find("Not requantised"), std::string::npos)
      << out_.str();
  EXPECT_NE(out_.str().find("DDD_REQUANTIZATION: off"), std::string::npos)
      << out_.str();
}

// Paced, the samples are the same and take at least as long as they last:
// two and a half segments at 10 Msps are about a quarter of a second.
TEST_F(RequantizeCliTest, RealTimeHandsTheSameSamplesOnAtTheirRate) {
  EXPECT_TRUE(ParseRequantizeCliOptions({"--realtime", "in.s16"}).realtime);
  EXPECT_FALSE(
      ParseRequantizeCliOptions({"--realtime=yes", "in.s16"}).problem.empty());

  const std::vector<uint8_t> stored = StoredBytes(Converted(Codes()));
  WriteAll(Path("in.s16"), stored);
  ASSERT_EQ(Run({"--rate", "10", Path("in.s16"), Path("fast.s16")}),
            kRequantizeCliSuccess)
      << error_.str();

  const auto started = std::chrono::steady_clock::now();
  ASSERT_EQ(
      Run({"--rate", "10", "--realtime", Path("in.s16"), Path("paced.s16")}),
      kRequantizeCliSuccess)
      << error_.str();
  const double took =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();

  const size_t samples = stored.size() / kBytesPerSample;
  const double lasts = static_cast<double>(samples) / 10.0e6;
  EXPECT_GE(took, lasts * 0.95) << took << " s for " << lasts << " s";
  EXPECT_EQ(ReadAll(Path("paced.s16")), ReadAll(Path("fast.s16")));
}

// Without an output it decides and reports, and writes nothing.
TEST_F(RequantizeCliTest, WithoutAnOutputNothingIsWritten) {
  WriteAll(Path("in.s16"), StoredBytes(Converted(Codes())));
  std::vector<std::string> args = Choices();
  args.push_back(Path("in.s16"));
  args.emplace_back("--log");
  args.push_back(Path("decisions.csv"));
  ASSERT_EQ(Run(args), kRequantizeCliSuccess) << error_.str();
  EXPECT_NE(out_.str().find("Nothing written"), std::string::npos);
  EXPECT_TRUE(standard_output_.bytes.empty());

  // A header and a line for each of the three segments.
  std::ifstream log(Path("decisions.csv"));
  std::string line;
  int lines = 0;
  while (std::getline(log, line)) {
    ++lines;
  }
  EXPECT_EQ(lines, 4);
}

TEST_F(RequantizeCliTest, MoreDepthsThanStretchesIsAUsageError) {
  WriteAll(Path("in.s16"), StoredBytes(Converted(Codes())));
  EXPECT_EQ(Run({"--rate", "30", "--band", "2-14", "--depth", "10,20,40",
                 Path("in.s16")}),
            kRequantizeCliUsage);
  EXPECT_NE(error_.str().find("at most 2"), std::string::npos) << error_.str();
}

TEST_F(RequantizeCliTest, AnUncompressedCaptureNeedsItsRate) {
  WriteAll(Path("in.s16"), StoredBytes(Converted(Codes())));
  EXPECT_EQ(Run({Path("in.s16")}), kRequantizeCliUsage);
  EXPECT_NE(error_.str().find("--rate"), std::string::npos) << error_.str();
}

TEST_F(RequantizeCliTest, AnInputThatCannotBeReadIsSaidSo) {
  EXPECT_EQ(Run({"--rate", "30", Path("missing.s16")}), kRequantizeCliInput);
  WriteAll(Path("notes.txt"), {1, 2, 3, 4});
  EXPECT_EQ(Run({"--rate", "30", Path("notes.txt")}), kRequantizeCliInput);
  WriteAll(Path("empty.s16"), {});
  EXPECT_EQ(Run({"--rate", "30", Path("empty.s16")}), kRequantizeCliInput);
}

TEST_F(RequantizeCliTest, HelpIsTheUsage) {
  EXPECT_EQ(Run({"--help"}), kRequantizeCliSuccess);
  EXPECT_NE(out_.str().find("ddd-requantize"), std::string::npos);
}

}  // namespace
}  // namespace ddd::capture
