/************************************************************************

    byte_stream.cpp

    Somewhere bytes can be written that is not a file
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "byte_stream.h"

#include <algorithm>

#ifdef _WIN32
#include <io.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#else
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#endif

namespace ddd::capture {

#ifdef _WIN32

namespace {

// WriteFile takes a 32-bit length. Far larger than anything written here, and
// named so that the loop below does not depend on that staying true.
constexpr size_t kLargestWrite = size_t{1} << 30;

// Standard output as the process was started with it — the pipe or the file a
// command line redirected it to — rather than whatever GetStdHandle() says now.
//
// The two stop agreeing the moment this windowed application borrows the
// console it was started from (console_attach.cpp): AttachConsole() replaces
// the process's standard handles with the console's own, so GetStdHandle()
// answers with the console even though the caller redirected the stream. The
// C runtime bound its stdout to the redirection at startup and keeps it, and
// AttachParentConsole() leaves a redirected stream exactly as it found it — so
// the runtime's handle is the one that leads where the caller asked.
//
// Checked for a negative descriptor first, because there is none when nothing
// was redirected and no console was found, and handing that to
// _get_osfhandle() would end the process in the runtime's invalid-parameter
// handler rather than answer.
HANDLE StartupStandardOutput() {
  const int descriptor = _fileno(stdout);
  if (descriptor < 0) {
    return INVALID_HANDLE_VALUE;
  }
  const intptr_t handle = _get_osfhandle(descriptor);
  if (handle == -1 || handle == -2) {
    return INVALID_HANDLE_VALUE;
  }
  return reinterpret_cast<HANDLE>(handle);
}

}  // namespace

bool StandardOutputStream::Write(const uint8_t* data, size_t size) {
  // Asked on every write rather than once, because it costs nothing and a
  // handle cached at construction would outlive anything that replaced it.
  const HANDLE handle = StartupStandardOutput();
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return false;
  }

  while (size > 0) {
    const auto chunk = static_cast<DWORD>(std::min(size, kLargestWrite));
    DWORD written = 0;
    if (WriteFile(handle, data, chunk, &written, nullptr) == 0 ||
        written == 0) {
      return false;
    }
    data += written;
    size -= written;
  }
  return true;
}

bool StandardOutputIsRedirected() {
  const HANDLE handle = StartupStandardOutput();
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  const DWORD type =
      GetFileType(handle) & ~static_cast<DWORD>(FILE_TYPE_REMOTE);
  return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

void IgnoreBrokenPipeSignal() {}

#else

bool StandardOutputStream::Write(const uint8_t* data, size_t size) {
  while (size > 0) {
    const ssize_t written = ::write(STDOUT_FILENO, data, size);
    if (written < 0) {
      // A signal arriving mid-write, which an interrupt handler installed for
      // Ctrl+C makes ordinary rather than rare. Not a failure of the pipe.
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      return false;
    }
    data += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

bool StandardOutputIsRedirected() {
  // Closed altogether, which a shell does with >&-.
  if (::fcntl(STDOUT_FILENO, F_GETFD) == -1) {
    return false;
  }
  return ::isatty(STDOUT_FILENO) == 0;
}

void IgnoreBrokenPipeSignal() {
  // The previous disposition is of no interest: nothing here ever restores it.
  static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
}

#endif

}  // namespace ddd::capture
