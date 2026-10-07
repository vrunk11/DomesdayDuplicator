/************************************************************************

    main.cpp

    ddd-requantize: requantising a capture offline, as a capture would be
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "byte_stream.h"
#include "requantize_cli.h"

// A main() and nothing else. Everything worth testing is in
// ddd::capture::RunRequantizeCli, which links no Qt and drives the same
// requantiser the capture application does — so what this writes is what a
// capture would have written, rather than something that resembles it.
int main(int argc, char* argv[]) {
  const std::vector<std::string> args(argv + 1, argv + argc);

  // Samples, not text: on Windows standard input would otherwise turn a
  // 0x0D 0x0A pair inside a sample into 0x0A. Standard output is written below
  // the runtime by StandardOutputStream, which has no text mode to worry about.
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
#endif

  // A reader that closes the pipe early is an error to report, not a signal
  // that ends the process before it can say so.
  ddd::capture::IgnoreBrokenPipeSignal();

  ddd::capture::StandardOutputStream standard_output;
  return ddd::capture::RunRequantizeCli(args, std::cin, standard_output,
                                        std::cout, std::cerr);
}
