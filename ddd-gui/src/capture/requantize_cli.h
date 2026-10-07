/************************************************************************

    requantize_cli.h

    ddd-requantize: requantising a capture offline, as a capture would be
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "byte_stream.h"
#include "rf_requantizer.h"

namespace ddd::capture {

// The whole of `ddd-requantize`, apart from main().
//
// It takes a capture that was written without requantisation and requantises
// it as the capture application would have: the same RfRequantizer, given its
// settings by the same translation (SettingsFor), fed the same segments in the
// same order as RequantizingSink feeds it during a capture, and stamping the
// same tags. A file processed here holds exactly the samples the capture
// application would have written with those settings, which is what makes it
// the way to compare settings: capture once, requantise the one capture as
// many ways as are worth trying, and compare the files — their size, and what
// ld-decode makes of them.
//
// Without an output it decides and writes nothing, as the capture
// application's preview does, and reports what a capture would have done.
//
// In a function rather than in main(), as RunJtagCli is: the exit codes, the
// messages and the samples written are the whole interface, and all three can
// be checked without a built binary or a shell.

// What ddd-requantize returns to the shell.
enum RequantizeCliExit {
  kRequantizeCliSuccess = 0,

  // The command line itself was wrong — including settings that cannot be
  // used together, such as more shaping depths than the bands leave
  // stretches for. Usage or the reason was printed.
  kRequantizeCliUsage = 2,

  // The input could not be opened or read.
  kRequantizeCliInput = 3,

  // The output could not be created or written.
  kRequantizeCliOutput = 4,
};

// Parsed arguments, so that the parsing can be tested apart from the work.
struct RequantizeCliOptions {
  // A .flac or .s16 capture, or "-" for signed 16-bit samples on standard
  // input.
  std::string input_path;

  // A .flac or .s16 file, "-" for signed 16-bit samples on standard output,
  // or empty to write nothing and only report.
  std::string output_path;

  // The samples' rate in MHz. A FLAC capture says what it is; anything else
  // has to be told.
  std::optional<double> rate_mhz;

  // Off: the samples go through untouched, which is how the reference a
  // comparison is made against is written — the same capture through the
  // same encoder, with nothing dropped.
  bool requantize = true;
  int margin_level = kDefaultMarginLevel;
  std::vector<FrequencyBand> bands;
  bool adaptive = false;
  std::vector<double> depths_db;
  int order = 0;

  // The bits the input carries. A FLAC capture says what its bit shift was;
  // otherwise they are found from the samples.
  std::optional<int> input_bits;

  // For a .flac output, 0-8 as flac's -0 .. -8.
  int compression_level = 8;

  // A CSV file to write one line per segment to: what was decided and why.
  std::string log_path;

  // Hand the samples on no faster than their rate, as a capture does, for
  // whatever downstream expects a live stream rather than a file's worth as
  // fast as it can be read.
  bool realtime = false;

  bool show_help = false;

  // Set when parsing failed; already written for a human.
  std::string problem;
};

RequantizeCliOptions ParseRequantizeCliOptions(
    const std::vector<std::string>& args);

// What ddd-requantize prints when asked how to use it.
std::string RequantizeCliUsage();

// Run it. `standard_input` is read for an input of "-", and
// `standard_output` written for an output of "-". `out` takes the report and
// `error` the problems — and the report as well when standard output is
// carrying the samples, so that nothing but samples ever reaches it.
int RunRequantizeCli(const std::vector<std::string>& args,
                     std::istream& standard_input, IByteStream& standard_output,
                     std::ostream& out, std::ostream& error);

}  // namespace ddd::capture
