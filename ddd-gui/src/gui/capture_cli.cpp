/************************************************************************

    capture_cli.cpp

    The capture options the command line accepts, and what they mean
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "capture_cli.h"

#include <QCommandLineParser>
#include <QFileInfo>
#include <QLatin1String>
#include <QStringList>

#include "wire_protocol.h"

namespace ddd::gui {
namespace {

// The switch names, in one place: the parser is given them here, and
// WantsCoreApplication() below has to recognise two of them without a parser.
constexpr const char* kStartCaptureName = "start-capture";
constexpr const char* kStopCaptureName = "stop-capture";
constexpr const char* kHeadlessName = "headless";
constexpr const char* kCaptureDirectoryName = "capture-directory";
constexpr const char* kCaptureNameName = "capture-name";
constexpr const char* kDecimationName = "decimation";
constexpr const char* kAdcRateName = "adc-rate";
constexpr const char* kInputRangeName = "input-range";
constexpr const char* kDurationLimitName = "duration-limit";
constexpr const char* kOutputFormatName = "output-format";

// The format words, spelled as the settings file spells them, so that a script
// and a settings file name the same format the same way. The reading of them
// here is strict where the settings loader's is forgiving: a settings file
// naming a format this build does not have should still produce a working
// capture, but a command line naming one is a typo, and a script that silently
// wrote FLAC when it asked for raw samples would be found out much later.
constexpr const char* kFlacFormatWord = "flac";
constexpr const char* kSigned16BitFormatWord = "s16";

// The input-range words. "2vpp"/"1vpp" rather than true/false, on the same
// reasoning as the format words above: what somebody actually knows at the
// command line is the range printed on the source they are capturing, not
// which boolean the setting happens to be.
constexpr const char* k2VppWord = "2vpp";
constexpr const char* k1VppWord = "1vpp";

// The decimation choices this build offers, each a word rather than a rate:
// this option divides the ADC rate (--adc-rate, a separate setting) further,
// and is not a rate itself. Spelling a choice as an absolute number of
// samples per second was tried and was wrong the moment the ADC rate became
// selectable — "20" stopped meaning "half of 40" and started meaning "half
// of whatever --adc-rate was", which nothing at parse time can resolve: the
// board's own default rate is not known until a device answers, which has
// not happened yet when a command line is parsed. A word for the divisor
// keeps the two settings independent, which is what they actually are.
constexpr const char* kFullDecimationWord = "full";
constexpr const char* kHalfDecimationWord = "half";
constexpr const char* kQuarterDecimationWord = "quarter";

struct DecimationChoice {
  const char* word;
  int factor;
};

// A factor added to capture_format.h appears on the command line by adding
// its word here — nothing else has to be kept in step.
constexpr DecimationChoice kSupportedDecimationChoices[] = {
    {kFullDecimationWord, capture::kUndecimatedFactor},
    {kHalfDecimationWord, capture::kTapeDecimationFactor},
    {kQuarterDecimationWord, capture::kQuarterDecimationFactor},
};

std::optional<int> DecimationFactorForWord(const QString& word) {
  for (const DecimationChoice& choice : kSupportedDecimationChoices) {
    if (word == QLatin1String(choice.word)) {
      return choice.factor;
    }
  }
  return std::nullopt;
}

QString SupportedDecimationWords() {
  QStringList words;
  for (const DecimationChoice& choice : kSupportedDecimationChoices) {
    words.append(QLatin1String(choice.word));
  }
  return words.join(QStringLiteral(", "));
}

// The ADC rates PLL_PRESET can ask for - see kPllPreset40Mhz..kPllPreset75Mhz
// in wire_protocol.h, which this mirrors, on the same terms
// kSupportedDecimationChoices mirrors capture_format.h above: named here
// rather than derived from IsSupportedPllPreset, which answers "is this one
// of them" rather than "what are they" - this list is only ever walked in
// the direction of listing them.
constexpr uint8_t kKnownPllPresets[] = {
    capture::kPllPreset40Mhz, capture::kPllPreset45Mhz,
    capture::kPllPreset50Mhz, capture::kPllPreset55Mhz,
    capture::kPllPreset60Mhz, capture::kPllPreset65Mhz,
    capture::kPllPreset70Mhz, capture::kPllPreset75Mhz};

QString SupportedPllPresetWords() {
  QStringList rates;
  for (const uint8_t preset : kKnownPllPresets) {
    rates.append(QString::number(preset));
  }
  return rates.join(QStringLiteral(" or "));
}

// Whether a raw argument is this option, in either of the spellings Qt's parser
// accepts for a long name.
bool IsOptionToken(const QString& token, const char* name) {
  const QString long_form = QLatin1String("--") + QLatin1String(name);
  const QString short_form = QLatin1String("-") + QLatin1String(name);
  return token == long_form || token == short_form;
}

}  // namespace

bool CaptureCliOptions::HasAttributeOverrides() const {
  return capture_directory.has_value() || capture_name.has_value() ||
         decimation_factor.has_value() || pll_preset_mhz.has_value() ||
         range_select_2vpp.has_value() || duration_limit_seconds.has_value() ||
         output_format.has_value();
}

CaptureCliOptionSet AddCaptureCliOptions(QCommandLineParser& parser) {
  CaptureCliOptionSet set{
      QCommandLineOption(
          QLatin1String(kStartCaptureName),
          QStringLiteral("Start capturing as soon as a device is found. The "
                         "window still opens unless --headless is given.")),
      QCommandLineOption(
          QLatin1String(kStopCaptureName),
          QStringLiteral("Stop the capture a running instance is taking, wait "
                         "for its file to be finished, and exit.")),
      QCommandLineOption(
          QLatin1String(kHeadlessName),
          QStringLiteral("Run with no window. Requires --start-capture, and "
                         "runs until the duration limit, --stop-capture, or an "
                         "interrupt.")),
      QCommandLineOption(
          QLatin1String(kCaptureDirectoryName),
          QStringLiteral("Write the capture here instead of the configured "
                         "folder. Created if it does not exist."),
          QStringLiteral("directory")),
      QCommandLineOption(
          QLatin1String(kCaptureNameName),
          QStringLiteral("Name the capture this, without a suffix, instead of "
                         "the configured or generated name."),
          QStringLiteral("name")),
      QCommandLineOption(
          QLatin1String(kDecimationName),
          QStringLiteral("Divide the ADC's own rate by this much in the "
                         "device: %1. Independent of --adc-rate below - this "
                         "is how much further to divide whatever rate that "
                         "is, not a rate of its own.")
              .arg(SupportedDecimationWords()),
          QStringLiteral("choice")),
      QCommandLineOption(
          QLatin1String(kAdcRateName),
          QStringLiteral(
              "Ask the device's PLL to run the converter itself at this rate "
              "in MHz, before --decimation divides it further: %1. Only "
              "takes effect on gateware built with a reconfigurable PLL; "
              "refused by the device if it is above what the board can do.")
              .arg(SupportedPllPresetWords()),
          QStringLiteral("mhz")),
      QCommandLineOption(
          QLatin1String(kInputRangeName),
          QStringLiteral("Set the ADC's input range to %1 or %2. 2Vpp is the "
                         "safe default: clipping the input loses signal "
                         "irrecoverably, where a range wider than the "
                         "source's own level only costs resolution.")
              .arg(QLatin1String(k2VppWord), QLatin1String(k1VppWord)),
          QStringLiteral("range")),
      QCommandLineOption(
          QLatin1String(kDurationLimitName),
          QStringLiteral("Stop automatically after this many seconds. Omit it "
                         "to run until stopped."),
          QStringLiteral("seconds")),
      QCommandLineOption(QLatin1String(kOutputFormatName),
                         QStringLiteral("Write the capture as %1 or %2.")
                             .arg(QLatin1String(kFlacFormatWord),
                                  QLatin1String(kSigned16BitFormatWord)),
                         QStringLiteral("format")),
  };

  parser.addOption(set.start_capture);
  parser.addOption(set.stop_capture);
  parser.addOption(set.headless);
  parser.addOption(set.capture_directory);
  parser.addOption(set.capture_name);
  parser.addOption(set.decimation);
  parser.addOption(set.adc_rate);
  parser.addOption(set.input_range);
  parser.addOption(set.duration_limit);
  parser.addOption(set.output_format);

  return set;
}

CaptureCliParseResult ParseCaptureCliOptions(const QCommandLineParser& parser,
                                             const CaptureCliOptionSet& set) {
  CaptureCliParseResult result;
  CaptureCliOptions& options = result.options;

  options.start_capture = parser.isSet(set.start_capture);
  options.stop_capture = parser.isSet(set.stop_capture);
  options.headless = parser.isSet(set.headless);

  if (parser.isSet(set.capture_directory)) {
    const QString directory = parser.value(set.capture_directory).trimmed();
    if (directory.isEmpty()) {
      result.error = QStringLiteral(
          "--capture-directory needs a folder. Leave it out to use the "
          "configured one.");
      return result;
    }

    // Existing and not a folder is the only case worth refusing. A folder that
    // is not there yet is made when the capture is opened, exactly as it is for
    // a capture started from the window, so a script that names a folder per
    // disc works without creating it first.
    const QFileInfo info(directory);
    if (info.exists() && !info.isDir()) {
      result.error = QStringLiteral("--capture-directory '%1' is not a folder.")
                         .arg(directory);
      return result;
    }
    options.capture_directory = directory;
  }

  if (parser.isSet(set.capture_name)) {
    const QString name = parser.value(set.capture_name).trimmed();
    if (name.isEmpty()) {
      result.error = QStringLiteral(
          "--capture-name needs a name. Leave it out for the generated one.");
      return result;
    }

    // A name, not a path. The window's name field would accept a separator and
    // fail much later when the file was opened; a script gets told now, which
    // is the whole point of checking a command line rather than a text box.
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
      result.error =
          QStringLiteral(
              "--capture-name '%1' contains a path separator. Name the folder "
              "with --capture-directory.")
              .arg(name);
      return result;
    }
    options.capture_name = name;
  }

  if (parser.isSet(set.decimation)) {
    const QString word = parser.value(set.decimation).trimmed().toLower();
    const std::optional<int> factor = DecimationFactorForWord(word);
    if (!factor.has_value()) {
      result.error =
          QStringLiteral("Unknown --decimation '%1'. Use %2.")
              .arg(parser.value(set.decimation), SupportedDecimationWords());
      return result;
    }
    options.decimation_factor = factor;
  }

  if (parser.isSet(set.adc_rate)) {
    const QString text = parser.value(set.adc_rate).trimmed();
    bool numeric = false;
    const int mhz = text.toInt(&numeric);
    if (!numeric || mhz < 0 || mhz > 0xFF ||
        !capture::IsSupportedPllPreset(static_cast<uint8_t>(mhz))) {
      result.error = QStringLiteral("Unknown --adc-rate '%1'. Use %2, in MHz.")
                         .arg(text, SupportedPllPresetWords());
      return result;
    }
    options.pll_preset_mhz = static_cast<uint8_t>(mhz);
  }

  if (parser.isSet(set.input_range)) {
    const QString word = parser.value(set.input_range).trimmed().toLower();
    if (word == QLatin1String(k2VppWord)) {
      options.range_select_2vpp = true;
    } else if (word == QLatin1String(k1VppWord)) {
      options.range_select_2vpp = false;
    } else {
      result.error =
          QStringLiteral("Unknown --input-range '%1'. Use %2 or %3.")
              .arg(parser.value(set.input_range), QLatin1String(k2VppWord),
                   QLatin1String(k1VppWord));
      return result;
    }
  }

  if (parser.isSet(set.duration_limit)) {
    const QString text = parser.value(set.duration_limit).trimmed();
    bool numeric = false;
    const int seconds = text.toInt(&numeric);

    // Zero means "no limit" in the settings, and is refused here rather than
    // accepted as one: a script that computed a limit of zero has a bug, and
    // silently capturing until something else stopped it would hide it.
    if (!numeric || seconds < 1 ||
        seconds > CaptureSettings::kMaximumDurationLimitSeconds) {
      result.error =
          QStringLiteral(
              "Unknown --duration-limit '%1'. Use 1 to %2 seconds (%3 hours), "
              "or leave it out to capture until stopped.")
              .arg(text)
              .arg(CaptureSettings::kMaximumDurationLimitSeconds)
              .arg(CaptureSettings::kMaximumDurationLimitMinutes / 60);
      return result;
    }
    options.duration_limit_seconds = seconds;
  }

  if (parser.isSet(set.output_format)) {
    const QString word = parser.value(set.output_format).trimmed().toLower();
    if (word == QLatin1String(kFlacFormatWord)) {
      options.output_format = capture::CaptureOutputFormat::kFlac;
    } else if (word == QLatin1String(kSigned16BitFormatWord)) {
      options.output_format = capture::CaptureOutputFormat::kSigned16Bit;
    } else {
      result.error =
          QStringLiteral("Unknown --output-format '%1'. Use %2 or %3.")
              .arg(parser.value(set.output_format),
                   QLatin1String(kFlacFormatWord),
                   QLatin1String(kSigned16BitFormatWord));
      return result;
    }
  }

  // --stop-capture is a message to a process that is already running and has
  // already been told what to capture. Anything else on the line is an
  // instruction with nowhere to go, so it is refused rather than dropped.
  if (options.stop_capture && (options.start_capture || options.headless ||
                               options.HasAttributeOverrides())) {
    result.error = QStringLiteral(
        "--stop-capture stops a capture that is already running, so it cannot "
        "be given with the options that set one up.");
    return result;
  }

  if (options.headless && !options.start_capture) {
    result.error = QStringLiteral(
        "--headless needs --start-capture. Without a window and without a "
        "capture there would be nothing for the application to do.");
    return result;
  }

  return result;
}

void ApplyCliOverrides(CaptureSettings& settings,
                       const CaptureCliOptions& options) {
  if (options.capture_directory.has_value()) {
    settings.capture_directory = *options.capture_directory;
  }
  if (options.capture_name.has_value()) {
    settings.capture_name = *options.capture_name;
  }
  if (options.decimation_factor.has_value()) {
    settings.decimation_factor = *options.decimation_factor;
  }
  if (options.pll_preset_mhz.has_value()) {
    settings.pll_preset_mhz = *options.pll_preset_mhz;
  }
  if (options.range_select_2vpp.has_value()) {
    settings.range_select_2vpp = *options.range_select_2vpp;
  }
  if (options.duration_limit_seconds.has_value()) {
    settings.duration_limit_seconds = *options.duration_limit_seconds;
  }
  if (options.output_format.has_value()) {
    settings.output_format = *options.output_format;
  }
}

bool WantsCoreApplication(int argc, char* argv[]) {
  for (int index = 1; index < argc; ++index) {
    const QString token = QString::fromLocal8Bit(argv[index]);

    // Everything after a bare -- is an argument rather than an option, and this
    // application has none. Stopping here anyway costs nothing and keeps the
    // scan honest about what an option is.
    if (token == QLatin1String("--")) {
      break;
    }

    if (IsOptionToken(token, kHeadlessName) ||
        IsOptionToken(token, kStopCaptureName)) {
      return true;
    }
  }
  return false;
}

}  // namespace ddd::gui
