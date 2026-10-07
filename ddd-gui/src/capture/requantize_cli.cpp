/************************************************************************

    requantize_cli.cpp

    ddd-requantize: requantising a capture offline, as a capture would be
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "requantize_cli.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "capture_format.h"
#include "capture_provenance.h"
#include "capture_reader.h"
#include "flac_writer.h"
#include "log_format.h"
#include "requantizing_sink.h"
#include "sample_format.h"

namespace ddd::capture {
namespace {

constexpr const char* kStandardStream = "-";

// The widest input the requantiser takes, and the narrowest: what
// RfRequantizer clamps input_bits to.
constexpr int kFewestInputBits = 5;
constexpr int kMostInputBits = 16;

constexpr double kMegahertz = 1.0e6;

// The width of a sample as a capture stores it.
constexpr int kStoredSampleBits = 16;

// A decimal as a person types it, all of it or nothing.
std::optional<double> ParseNumber(const std::string& text) {
  if (text.empty()) {
    return std::nullopt;
  }
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<int> ParseWhole(const std::string& text) {
  int value = 0;
  const char* const last = text.data() + text.size();
  const auto [stopped, error] = std::from_chars(text.data(), last, value);
  if (text.empty() || error != std::errc() || stopped != last) {
    return std::nullopt;
  }
  return value;
}

std::string Percent(uint64_t part, uint64_t whole) {
  if (whole == 0) {
    return "0%";
  }
  return FormatDecimal(
             100.0 * static_cast<double>(part) / static_cast<double>(whole),
             1) +
         "%";
}

std::string BitsPerSample(uint64_t bytes, uint64_t samples) {
  if (samples == 0) {
    return "-";
  }
  return FormatDecimal(
      8.0 * static_cast<double>(bytes) / static_cast<double>(samples), 2);
}

// Where the samples come from: a capture file through CaptureReader, or
// signed 16-bit words on standard input.
class SampleInput {
 public:
  explicit SampleInput(std::istream& standard_input)
      : standard_input_(standard_input) {}

  // Open `path`, "-" for standard input. False with the reason in `problem`.
  bool Open(const std::string& path, std::string& problem) {
    if (path == kStandardStream) {
      from_standard_input_ = true;
      return true;
    }
    const std::optional<CaptureReader::Format> format =
        CaptureReader::FormatFromExtension(path);
    if (!format.has_value()) {
      problem = path + " is not a capture: give a .flac or .s16 file.";
      return false;
    }
    std::string reason;
    if (!reader_.Open(path, *format, reason)) {
      problem = "Could not open " + path + ": " + reason;
      return false;
    }
    return true;
  }

  // Fill `segment` up to its size, or with what is left. False on a read
  // error, with the reason in `problem`; an empty segment is the end.
  bool Fill(std::vector<int16_t>& segment, size_t size, std::string& problem) {
    segment.clear();
    if (from_standard_input_) {
      bytes_.resize(size * kBytesPerSample);
      standard_input_.read(reinterpret_cast<char*>(bytes_.data()),
                           static_cast<std::streamsize>(bytes_.size()));
      const auto got =
          static_cast<size_t>(standard_input_.gcount()) / kBytesPerSample;
      segment.resize(got);
      for (size_t index = 0; index < got; ++index) {
        segment[index] = static_cast<int16_t>(
            static_cast<uint16_t>(bytes_[index * kBytesPerSample]) |
            static_cast<uint16_t>(
                static_cast<uint16_t>(bytes_[(index * kBytesPerSample) + 1])
                << 8));
      }
      return true;
    }

    while (segment.size() < size && !ended_) {
      if (!reader_.ReadSigned(block_, size - segment.size(), ended_)) {
        problem = reader_.LastError().empty() ? "The input could not be read"
                                              : reader_.LastError();
        return false;
      }
      segment.insert(segment.end(), block_.begin(), block_.end());
      if (block_.empty() && !ended_) {
        ended_ = true;
      }
    }
    return true;
  }

  // The input's own tags, for a FLAC capture; empty otherwise.
  std::vector<std::pair<std::string, std::string>> Tags() const {
    return from_standard_input_
               ? std::vector<std::pair<std::string, std::string>>{}
               : reader_.Tags();
  }

  std::optional<uint64_t> TotalSamples() const {
    return from_standard_input_ ? std::nullopt : reader_.TotalSamples();
  }

 private:
  std::istream& standard_input_;
  bool from_standard_input_ = false;
  CaptureReader reader_;
  bool ended_ = false;
  std::vector<int16_t> block_;
  std::vector<uint8_t> bytes_;
};

// Where the requantised samples go: a FLAC file, an uncompressed one, or
// standard output.
class SampleOutput {
 public:
  explicit SampleOutput(IByteStream& standard_output)
      : standard_output_(standard_output) {}

  bool Open(const std::string& path, const FlacWriter::Options& flac_options,
            std::string& problem) {
    path_ = path;
    if (path == kStandardStream) {
      to_standard_output_ = true;
      return true;
    }
    const std::optional<CaptureReader::Format> format =
        CaptureReader::FormatFromExtension(path);
    if (!format.has_value()) {
      problem = path +
                " is not somewhere a capture can be written: give a "
                ".flac or .s16 file.";
      return false;
    }
    if (*format == CaptureReader::Format::kFlac) {
      flac_ = std::make_unique<FlacWriter>();
      std::string reason;
      if (!flac_->Open(path, flac_options, reason)) {
        problem = "Could not create " + path + ": " + reason;
        return false;
      }
      return true;
    }
    file_.open(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
      problem = "Could not create " + path;
      return false;
    }
    return true;
  }

  bool Write(const std::vector<int16_t>& samples) {
    if (flac_ != nullptr) {
      return flac_->WriteSigned16Samples(samples.data(), samples.size());
    }
    bytes_.resize(samples.size() * kBytesPerSample);
    for (size_t index = 0; index < samples.size(); ++index) {
      const auto word = static_cast<uint16_t>(samples[index]);
      bytes_[index * kBytesPerSample] = static_cast<uint8_t>(word & 0xFFU);
      bytes_[(index * kBytesPerSample) + 1] = static_cast<uint8_t>(word >> 8);
    }
    written_ += bytes_.size();
    if (to_standard_output_) {
      return standard_output_.Write(bytes_.data(), bytes_.size());
    }
    file_.write(reinterpret_cast<const char*>(bytes_.data()),
                static_cast<std::streamsize>(bytes_.size()));
    return file_.good();
  }

  bool Finish(std::string& problem) {
    if (flac_ != nullptr) {
      if (!flac_->Finish()) {
        problem = flac_->LastError();
        return false;
      }
      written_ = flac_->BytesWritten();
      return true;
    }
    if (file_.is_open()) {
      file_.close();
      if (file_.fail()) {
        problem = "Could not finish writing " + path_;
        return false;
      }
    }
    return true;
  }

  uint64_t BytesWritten() const { return written_; }

  std::string WriteProblem() const {
    if (flac_ != nullptr && !flac_->LastError().empty()) {
      return flac_->LastError();
    }
    return to_standard_output_ ? "Standard output stopped taking samples"
                               : "Could not write to " + path_;
  }

 private:
  IByteStream& standard_output_;
  std::string path_;
  bool to_standard_output_ = false;
  std::unique_ptr<FlacWriter> flac_;
  std::ofstream file_;
  std::vector<uint8_t> bytes_;
  uint64_t written_ = 0;
};

std::optional<std::string> TagValue(
    const std::vector<std::pair<std::string, std::string>>& tags,
    const std::string& name) {
  for (const auto& [tag, value] : tags) {
    if (tag == name) {
      return value;
    }
  }
  return std::nullopt;
}

// What a sample's low bits say about how many it carries: a converter's ten,
// left-aligned, leave six zero bits under every sample, and a bit shift one
// more for each bit shifted.
int BitsFoundIn(const std::vector<int16_t>& samples) {
  uint16_t any = 0;
  for (const int16_t sample : samples) {
    any = static_cast<uint16_t>(any | static_cast<uint16_t>(sample));
  }
  if (any == 0) {
    return kConverterBits;
  }
  int zeros = 0;
  while ((any & 1U) == 0) {
    any = static_cast<uint16_t>(any >> 1);
    ++zeros;
  }
  return kStoredSampleBits - zeros;
}

}  // namespace

std::string RequantizeCliUsage() {
  return "ddd-requantize — requantise a capture as the capture application "
         "would\n"
         "\n"
         "Usage:\n"
         "  ddd-requantize [options] <input> [<output>]\n"
         "\n"
         "  <input>    a capture written without requantisation: .flac or "
         ".s16,\n"
         "             or - for signed 16-bit little-endian samples on "
         "standard input\n"
         "  <output>   .flac or .s16, or - for signed 16-bit samples on "
         "standard output.\n"
         "             Without one nothing is written, and the report says "
         "what a\n"
         "             capture would have done.\n"
         "\n"
         "Options:\n"
         "  --rate <MHz>             The samples' rate. A FLAC capture says "
         "what it is;\n"
         "                           anything else has to be told: 30 for 60 "
         "MHz halved\n"
         "  --margin <0-4|off>       How far the noise floor may rise in any 1 "
         "MHz slice\n"
         "                           of the protected bands: 0 aggressive (1 "
         "dB), 1 (0.5),\n"
         "                           2 safe (0.2, the default), 3 (0.1), 4 "
         "(0.05).\n"
         "                           off writes the samples as they are: the "
         "reference\n"
         "                           to compare the others with\n"
         "  --band <bands>           The protected bands in MHz: 0-12, 2-14, "
         "or several,\n"
         "                           0-1.9,2.1-13.5. Default: DC to 14 MHz, "
         "or 1.5 MHz\n"
         "                           short of the Nyquist limit\n"
         "  --shaping <shaping>      fixed (the default) or adaptive\n"
         "  --depth <dB>[,<dB>...]   How far above the bands the shaped noise "
         "may go, 0 to\n"
         "                           40: one depth for everywhere outside "
         "them, or one for\n"
         "                           each stretch between them from DC up — "
         "with bands\n"
         "                           2-14, 10,40 spares the EFM and puts the "
         "rest above\n"
         "                           14 MHz. Default 10 fixed, 20 adaptive\n"
         "  --order <2-64>           The shaping filter's order. Default 16 "
         "fixed, 32\n"
         "                           adaptive\n"
         "  --input-bits <5-16>      The bits the input carries. A FLAC "
         "capture says;\n"
         "                           otherwise they are found from the "
         "samples\n"
         "  --compression <0-8>      FLAC level for a .flac output. Default 8\n"
         "  --log <file.csv>         One line per segment: what was decided, "
         "and the\n"
         "                           slice that held it back\n"
         "  --help                   Show this text\n"
         "\n"
         "The samples written are those the capture application writes with "
         "the same\n"
         "settings: the same requantiser, decided segment by segment from the "
         "start of\n"
         "the file. Capture once with requantisation off, then requantise that "
         "one capture\n"
         "each way worth trying and compare the files.\n"
         "\n"
         "Exit codes: 0 success, 2 usage, 3 input, 4 output.\n";
}

RequantizeCliOptions ParseRequantizeCliOptions(
    const std::vector<std::string>& args) {
  RequantizeCliOptions options;
  std::vector<std::string> positional;

  for (size_t index = 0; index < args.size(); ++index) {
    std::string argument = args[index];

    if (argument == "--help" || argument == "-h") {
      options.show_help = true;
      return options;
    }
    if (argument.rfind("--", 0) != 0) {
      positional.push_back(argument);
      continue;
    }

    // --name value, or --name=value.
    std::string value;
    bool has_value = false;
    const size_t equals = argument.find('=');
    if (equals != std::string::npos) {
      value = argument.substr(equals + 1);
      argument.resize(equals);
      has_value = true;
    }
    const auto take = [&]() -> bool {
      if (has_value) {
        return true;
      }
      if (index + 1 >= args.size()) {
        options.problem = argument + " needs a value.";
        return false;
      }
      value = args[++index];
      return true;
    };

    if (argument == "--rate") {
      if (!take()) {
        return options;
      }
      const std::optional<double> rate = ParseNumber(value);
      if (!rate.has_value() || *rate <= 0.0) {
        options.problem = "Unknown --rate '" + value +
                          "'. Give the samples' rate in MHz: 30, or 32.5.";
        return options;
      }
      options.rate_mhz = rate;
    } else if (argument == "--margin") {
      if (!take()) {
        return options;
      }
      if (value == "off") {
        options.requantize = false;
        continue;
      }
      const std::optional<int> margin = ParseWhole(value);
      if (!margin.has_value() || *margin < kMinimumMarginLevel ||
          *margin > kMaximumMarginLevel) {
        options.problem =
            "Unknown --margin '" + value + "'. Use 0 to 4, or off.";
        return options;
      }
      options.requantize = true;
      options.margin_level = *margin;
    } else if (argument == "--band" || argument == "--bands") {
      if (!take()) {
        return options;
      }
      options.bands = ParseBands(value);
      if (options.bands.empty()) {
        options.problem = "Unknown --band '" + value +
                          "'. Give bands in MHz, low to high: 0-12, or "
                          "0-1.9,2.1-13.5.";
        return options;
      }
    } else if (argument == "--shaping") {
      if (!take()) {
        return options;
      }
      if (value == "adaptive") {
        options.adaptive = true;
      } else if (value == "fixed") {
        options.adaptive = false;
      } else {
        options.problem =
            "Unknown --shaping '" + value + "'. Use fixed or adaptive.";
        return options;
      }
    } else if (argument == "--depth") {
      if (!take()) {
        return options;
      }
      options.depths_db = ParseShapingDepths(value);
      if (options.depths_db.empty()) {
        options.problem = "Unknown --depth '" + value +
                          "'. Give a depth in dB, 0 to 40, or one for each "
                          "stretch between the bands: 10,40.";
        return options;
      }
    } else if (argument == "--order") {
      if (!take()) {
        return options;
      }
      const std::optional<int> order = ParseWhole(value);
      if (!order.has_value() || *order < kMinimumShapingOrder ||
          *order > kMaximumShapingOrder) {
        options.problem = "Unknown --order '" + value + "'. Use 2 to 64.";
        return options;
      }
      options.order = *order;
    } else if (argument == "--input-bits") {
      if (!take()) {
        return options;
      }
      const std::optional<int> bits = ParseWhole(value);
      if (!bits.has_value() || *bits < kFewestInputBits ||
          *bits > kMostInputBits) {
        options.problem = "Unknown --input-bits '" + value + "'. Use 5 to 16.";
        return options;
      }
      options.input_bits = bits;
    } else if (argument == "--compression") {
      if (!take()) {
        return options;
      }
      const std::optional<int> level = ParseWhole(value);
      if (!level.has_value() || *level < 0 || *level > 8) {
        options.problem = "Unknown --compression '" + value + "'. Use 0 to 8.";
        return options;
      }
      options.compression_level = *level;
    } else if (argument == "--log") {
      if (!take()) {
        return options;
      }
      if (value.empty()) {
        options.problem = "--log needs a file to write to.";
        return options;
      }
      options.log_path = value;
    } else {
      options.problem = "Unknown option: " + argument;
      return options;
    }
  }

  if (positional.empty()) {
    options.problem = "No capture was given to requantise.";
    return options;
  }
  if (positional.size() > 2) {
    options.problem = "One capture in and at most one out.";
    return options;
  }
  options.input_path = positional[0];
  if (positional.size() == 2) {
    options.output_path = positional[1];
  }
  if (!options.output_path.empty() && options.output_path != kStandardStream &&
      options.output_path == options.input_path) {
    options.problem = "The output would overwrite the capture it is made from.";
  }
  return options;
}

int RunRequantizeCli(const std::vector<std::string>& args,
                     std::istream& standard_input, IByteStream& standard_output,
                     std::ostream& out, std::ostream& error) {
  const RequantizeCliOptions options = ParseRequantizeCliOptions(args);

  if (options.show_help) {
    out << RequantizeCliUsage();
    return kRequantizeCliSuccess;
  }
  if (!options.problem.empty()) {
    error << options.problem << "\n\n" << RequantizeCliUsage();
    return kRequantizeCliUsage;
  }

  // Nothing but samples reaches standard output when it is carrying them.
  std::ostream& report = options.output_path == kStandardStream ? error : out;

  // Two names for one file would truncate the capture before it was read.
  std::error_code same_error;
  if (!options.output_path.empty() && options.output_path != kStandardStream &&
      options.input_path != kStandardStream &&
      std::filesystem::exists(options.output_path, same_error) &&
      std::filesystem::equivalent(options.input_path, options.output_path,
                                  same_error)) {
    error << "The output would overwrite the capture it is made from.\n";
    return kRequantizeCliUsage;
  }

  SampleInput input(standard_input);
  std::string problem;
  if (!input.Open(options.input_path, problem)) {
    error << problem << "\n";
    return kRequantizeCliInput;
  }
  const std::vector<std::pair<std::string, std::string>> input_tags =
      input.Tags();

  // The rate: as given, or as the capture says.
  std::optional<double> rate_mhz = options.rate_mhz;
  if (!rate_mhz.has_value()) {
    const std::optional<std::string> tag = TagValue(input_tags, kTagSampleRate);
    const std::optional<double> hertz =
        tag.has_value() ? ParseNumber(*tag) : std::nullopt;
    if (hertz.has_value() && *hertz > 0.0) {
      rate_mhz = *hertz / kMegahertz;
    }
  }
  if (!rate_mhz.has_value()) {
    error << "The samples' rate is not known: give it with --rate, in MHz.\n";
    return kRequantizeCliUsage;
  }

  if (TagValue(input_tags, kTagRequantization).value_or("") == "dynamic") {
    report << "Warning: " << options.input_path
           << " was requantised when it was captured. Requantising it again "
              "adds to the noise already added; start from a capture written "
              "with requantisation off.\n";
  }

  std::vector<int16_t> segment;
  if (!input.Fill(segment, RfRequantizer::kSegmentSamples, problem)) {
    error << problem << "\n";
    return kRequantizeCliInput;
  }
  if (segment.empty()) {
    error << options.input_path << " holds no samples.\n";
    return kRequantizeCliInput;
  }

  // The bits it carries: as given, from the capture's bit shift, or from the
  // samples themselves.
  const std::optional<std::string> shift_tag =
      TagValue(input_tags, kTagBitShift);
  const std::optional<int> shift =
      shift_tag.has_value() ? ParseWhole(*shift_tag) : std::nullopt;
  int input_bits = 0;
  if (options.input_bits.has_value()) {
    input_bits = *options.input_bits;
  } else if (shift.has_value()) {
    input_bits = kConverterBits - *shift;
  } else {
    input_bits = BitsFoundIn(segment);
  }
  input_bits = std::clamp(input_bits, kFewestInputBits, kMostInputBits);

  RequantizerRequest request;
  request.sample_rate_mhz = *rate_mhz;
  request.margin_level = options.margin_level;
  request.bands = options.bands;
  request.input_bits = input_bits;
  request.adaptive = options.adaptive;
  request.depths_db = options.depths_db;
  request.order = options.order;
  const std::string depths_problem =
      options.requantize ? ShapingDepthsProblem(request) : std::string();
  if (!depths_problem.empty()) {
    error << "--depth: " << depths_problem << "\n";
    return kRequantizeCliUsage;
  }
  const RequantizerSettings settings = SettingsFor(request);
  // Off is recorded as off, as a capture taken without requantisation is.
  const RequantizationRecord record =
      options.requantize ? RecordFor(settings) : RequantizationRecord{};
  const auto rate_hz =
      static_cast<uint32_t>(std::llround(*rate_mhz * kMegahertz));

  // The capture's own tags, less what this replaces, and the requantisation
  // stamped exactly as the capture application stamps it.
  FlacWriter::Options flac_options;
  flac_options.compression_level = options.compression_level;
  flac_options.sample_rate_label =
      FlacSampleRateLabelFor(kUndecimatedFactor, rate_hz);
  for (const auto& [name, value] : input_tags) {
    if (name == kTagEncoder || name == kTagRequantization ||
        name == kTagRequantizationMargin || name == kTagRequantizationBands ||
        name == kTagRequantizationShaping) {
      continue;
    }
    flac_options.tags.push_back({name, value});
  }
  flac_options.tags.push_back({kTagEncoder, "ddd-requantize"});
  if (!TagValue(input_tags, kTagSampleRate).has_value()) {
    flac_options.tags.push_back({kTagSampleRate, std::to_string(rate_hz)});
  }
  AppendRequantizationTags(record, flac_options.tags);

  const bool writing = !options.output_path.empty();
  SampleOutput output(standard_output);
  if (writing && !output.Open(options.output_path, flac_options, problem)) {
    error << problem << "\n";
    return kRequantizeCliOutput;
  }

  std::ofstream log;
  if (!options.log_path.empty()) {
    log.open(options.log_path, std::ios::out | std::ios::trunc);
    if (!log.is_open()) {
      error << "Could not create " << options.log_path << "\n";
      return kRequantizeCliOutput;
    }
    log << "segment,start_seconds,samples,bits_kept,bits_dropped,shaped,"
           "rise_db,noise_floor_lsb,carrier_to_noise_db,held_back_low_mhz,"
           "held_back_high_mhz,held_back_floor_lsb\n";
  }

  std::vector<FlacWriter::Tag> stamped;
  AppendRequantizationTags(record, stamped);
  report << (options.requantize ? "Requantising " : "Re-encoding, as is, ")
         << options.input_path << " at " << FormatDecimal(*rate_mhz, 3)
         << " Msps, " << input_bits << " bits in\n";
  for (const FlacWriter::Tag& tag : stamped) {
    report << "  " << tag.name << ": " << tag.value << "\n";
  }

  // As RequantizingSink does it during a capture: segment after segment from
  // the start, the last whatever is left, each decided and — where it is
  // being kept — requantised in place before it is written.
  RfRequantizer requantizer(settings);
  RequantizationStatus status;
  uint64_t position = 0;
  uint64_t segments = 0;
  const std::optional<uint64_t> total = input.TotalSamples();
  int last_percent = -1;
  while (!segment.empty()) {
    const RequantizerDecision decision =
        options.requantize
            ? requantizer.Process(segment.data(), segment.size(), writing)
            : RequantizerDecision{};
    status.Record(decision, position, segment.size(),
                  requantizer.noise_floor_lsb(),
                  requantizer.carrier_to_noise_db(), requantizer.clipped());

    if (log.is_open()) {
      log << segments << ","
          << FormatDecimal(
                 static_cast<double>(position) / (*rate_mhz * kMegahertz), 4)
          << "," << segment.size() << ","
          << (input_bits - std::max(decision.lsb_drop, 0)) << ","
          << decision.lsb_drop << "," << (decision.shaped ? 1 : 0) << ","
          << FormatDecimal(decision.degradation_db, 3) << ","
          << FormatDecimal(requantizer.noise_floor_lsb(), 3) << ","
          << FormatDecimal(requantizer.carrier_to_noise_db(), 2) << ","
          << FormatDecimal(decision.limit_low_mhz, 2) << ","
          << FormatDecimal(decision.limit_high_mhz, 2) << ","
          << FormatDecimal(decision.limit_floor_lsb, 3) << "\n";
    }

    if (writing && !output.Write(segment)) {
      error << output.WriteProblem() << "\n";
      return kRequantizeCliOutput;
    }

    position += segment.size();
    ++segments;
    if (total.has_value() && *total > 0) {
      const auto percent = static_cast<int>((position * 100) / *total);
      if (percent != last_percent) {
        last_percent = percent;
        report << "  " << percent << "%\r" << std::flush;
      }
    }

    if (!input.Fill(segment, RfRequantizer::kSegmentSamples, problem)) {
      error << problem << "\n";
      return kRequantizeCliInput;
    }
  }

  if (writing && !output.Finish(problem)) {
    error << problem << "\n";
    return kRequantizeCliOutput;
  }
  if (log.is_open()) {
    log.close();
    if (log.fail()) {
      error << "Could not finish writing " << options.log_path << "\n";
      return kRequantizeCliOutput;
    }
  }

  // What it came to.
  const RequantizationStatus::Summary summary = status.Read();
  report << "Read " << summary.samples << " samples ("
         << FormatSampleDuration(summary.samples, rate_hz) << ") in "
         << summary.segments << " segments\n";
  if (options.requantize) {
    report << "  Bits dropped:";
    for (size_t drop = summary.samples_by_drop.size(); drop-- > 0;) {
      if (summary.samples_by_drop[drop] > 0) {
        report << " " << drop << " for "
               << Percent(summary.samples_by_drop[drop], summary.samples);
      }
    }
    report << "\n";
    report << "  Shaped: " << Percent(summary.shaped_samples, summary.samples)
           << "; worst rise in the protected bands "
           << FormatDecimal(summary.worst_degradation_db, 2) << " dB; "
           << summary.clipped << " samples clipped\n";
  } else {
    report << "  Not requantised: the reference to compare the others "
              "with.\n";
  }

  if (writing) {
    // Sizes say something only for FLAC, and a comparison with the input only
    // when that was FLAC too: an uncompressed file is two bytes a sample
    // whatever was dropped from it.
    const auto is_flac = [](const std::string& path) {
      return path != kStandardStream &&
             CaptureReader::FormatFromExtension(path) ==
                 CaptureReader::Format::kFlac;
    };
    report << "Wrote " << options.output_path << ": "
           << FormatBytes(output.BytesWritten());
    if (is_flac(options.output_path)) {
      report << ", " << BitsPerSample(output.BytesWritten(), summary.samples)
             << " bits a sample";
      std::error_code size_error;
      const uintmax_t input_bytes =
          is_flac(options.input_path)
              ? std::filesystem::file_size(options.input_path, size_error)
              : 0;
      if (!size_error && input_bytes > 0) {
        const double ratio = static_cast<double>(output.BytesWritten()) /
                             static_cast<double>(input_bytes);
        report << " (the input "
               << FormatBytes(static_cast<uint64_t>(input_bytes)) << ", "
               << BitsPerSample(static_cast<uint64_t>(input_bytes),
                                summary.samples)
               << " bits a sample: " << FormatDecimal(100.0 * (1.0 - ratio), 1)
               << "% smaller)";
      }
    } else {
      report << ", uncompressed: write a .flac to see what was saved";
    }
    report << "\n";
  } else {
    report << "Nothing written: give an output to keep the result.\n";
  }
  return kRequantizeCliSuccess;
}

}  // namespace ddd::capture
