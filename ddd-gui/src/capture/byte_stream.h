/************************************************************************

    byte_stream.h

    Somewhere bytes can be written that is not a file
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

namespace ddd::capture {

// A destination that accepts bytes and can stop accepting them.
//
// What --pipe writes to. Abstract so that the writer in front of it can be
// tested against a reader that is slow, stalled or gone, none of which a real
// standard output can be made to be on demand.
//
// Thread-safety: one writer thread at a time. Write() may block for as long as
// the far end takes to read.
class IByteStream {
 public:
  IByteStream() = default;
  virtual ~IByteStream() = default;

  IByteStream(const IByteStream&) = delete;
  IByteStream& operator=(const IByteStream&) = delete;
  IByteStream(IByteStream&&) = delete;
  IByteStream& operator=(IByteStream&&) = delete;

  // Write all of it, or return false. False is final: for a pipe it means the
  // program reading it has gone, and nothing written afterwards can reach it.
  virtual bool Write(const uint8_t* data, size_t size) = 0;
};

// The process's standard output, written to directly.
//
// Below the C and C++ runtimes rather than through them, for three reasons.
// Neither buffers, so nothing written is held back in a library buffer while
// the reader waits. On Windows a handle written with WriteFile has no text mode
// to translate a 0x0A byte in a sample into 0x0D 0x0A. And a thread blocked
// here is not inside a runtime stream that the process's exit would try to
// flush.
class StandardOutputStream : public IByteStream {
 public:
  bool Write(const uint8_t* data, size_t size) override;
};

// Whether standard output leads to a pipe or a file — somewhere a program asked
// for it to go — rather than to a terminal or to nothing at all.
//
// Asked before --pipe is allowed to start: a terminal would be sent megabytes
// of binary a second, and with nothing attached every sample would be lost.
bool StandardOutputIsRedirected();

// Make a write to a pipe whose reader has gone fail with an error rather than
// end the process. Without it a POSIX system delivers SIGPIPE, whose default is
// to terminate — so the reader closing early would kill a capture that is also
// writing an archive. Nothing to do on Windows, where the write simply fails.
void IgnoreBrokenPipeSignal();

}  // namespace ddd::capture
