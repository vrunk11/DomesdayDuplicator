/************************************************************************

    raw_sink.cpp

    Writing a capture as uncompressed signed 16-bit
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "raw_sink.h"

#include <algorithm>
#include <string>

#include "capture_format.h"
#include "sample_format.h"

namespace ddd::capture {
namespace {

// Samples per write to the file. The same figure the encoder uses, and for the
// same reason: large enough that the per-call overhead disappears, small enough
// that the scratch buffer stays cache friendly.
constexpr size_t kWriteChunkSamples = 65'536;

}  // namespace

RawSink::RawSink() = default;

RawSink::~RawSink() { Finish(); }

bool RawSink::Open(const std::filesystem::path& file_path,
                   const SampleConversion& conversion) {
  file_path_ = file_path;
  conversion_ = conversion;

  file_.open(file_path, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!file_.is_open()) {
    last_error_ = "RawSink::Open(): Failed to create the capture file";
    return false;
  }

  bytes_written_ = 0;
  samples_written_ = 0;
  finished_ = false;
  scratch_.resize(kWriteChunkSamples * kSigned16BytesPerSample);
  return true;
}

bool RawSink::Write(const uint8_t* wire_data, size_t sample_count) {
  if (!file_.is_open()) {
    last_error_ = "RawSink::Write(): The capture file is not open";
    return false;
  }

  // A chunk at a time through the scratch buffer, which is sized once and never
  // grows on the capture path. The conversion is the one the pipe writer uses
  // too — see WireToSigned16LittleEndian — and the file is little-endian
  // wherever it was written.
  size_t done = 0;
  while (done < sample_count) {
    const size_t count = std::min(kWriteChunkSamples, sample_count - done);
    WireToSigned16LittleEndian(wire_data + (done * kBytesPerSample), count,
                               conversion_, scratch_.data());

    const size_t bytes = count * kSigned16BytesPerSample;
    file_.write(reinterpret_cast<const char*>(scratch_.data()),
                static_cast<std::streamsize>(bytes));
    if (!file_.good()) {
      last_error_ = "RawSink::Write(): Failed to write to the capture file";
      return false;
    }

    bytes_written_ += bytes;
    samples_written_ += count;
    done += count;
  }

  return true;
}

bool RawSink::WriteConverted(const int16_t* samples, size_t sample_count) {
  if (!file_.is_open()) {
    last_error_ = "RawSink::WriteConverted(): The capture file is not open";
    return false;
  }

  // Already what the file holds; only laid out little-endian, byte by byte,
  // whatever this machine is.
  size_t done = 0;
  while (done < sample_count) {
    const size_t count = std::min(kWriteChunkSamples, sample_count - done);
    for (size_t index = 0; index < count; ++index) {
      const auto sample = static_cast<uint16_t>(samples[done + index]);
      scratch_[index * kSigned16BytesPerSample] = static_cast<uint8_t>(sample);
      scratch_[(index * kSigned16BytesPerSample) + 1] =
          static_cast<uint8_t>(sample >> 8);
    }

    const size_t bytes = count * kSigned16BytesPerSample;
    file_.write(reinterpret_cast<const char*>(scratch_.data()),
                static_cast<std::streamsize>(bytes));
    if (!file_.good()) {
      last_error_ =
          "RawSink::WriteConverted(): Failed to write to the capture file";
      return false;
    }

    bytes_written_ += bytes;
    samples_written_ += count;
    done += count;
  }
  return true;
}

bool RawSink::Finish() {
  if (finished_ || !file_.is_open()) {
    return true;
  }

  finished_ = true;

  // Flushed and closed explicitly rather than left to the destructor, because
  // this is the call whose failure a user has to be told about: a capture that
  // could not be flushed is a file that is short, and the stream ending quietly
  // is how that goes unnoticed.
  file_.flush();
  const bool good = file_.good();
  file_.close();

  if (!good) {
    last_error_ = "RawSink::Finish(): Failed to flush the capture file";
    return false;
  }
  return true;
}

}  // namespace ddd::capture
