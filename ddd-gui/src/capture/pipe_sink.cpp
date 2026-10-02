/************************************************************************

    pipe_sink.cpp

    A capture that reaches another program, with or without a file
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "pipe_sink.h"

#include <utility>

#include "sample_format.h"

namespace ddd::capture {

PipeSink::PipeSink(std::shared_ptr<PipeWriter> pipe)
    : pipe_(std::move(pipe)), name_("pipe") {}

PipeSink::PipeSink(std::unique_ptr<ISampleSink> file,
                   std::shared_ptr<PipeWriter> pipe)
    : file_(std::move(file)), pipe_(std::move(pipe)) {
  name_ = file_ != nullptr ? std::string(file_->Name()) + "+pipe" : "pipe";
}

PipeSink::~PipeSink() { Finish(); }

bool PipeSink::Write(const uint8_t* wire_data, size_t sample_count) {
  // The file first. It is the recording, and it gets the buffer before
  // anything else has had a chance to spend time on it.
  if (file_ != nullptr && !file_->Write(wire_data, sample_count)) {
    last_error_ = file_->LastError();
    return false;
  }

  if (pipe_ != nullptr && !pipe_->Offer(wire_data, sample_count)) {
    // Only possible for a writer made to fail when full, which is the pipe
    // alone: the samples that did not fit exist nowhere else.
    last_error_ =
        "The program reading standard output did not keep up, and samples "
        "that existed nowhere else would have been lost";
    return false;
  }

  samples_written_ += sample_count;
  return true;
}

bool PipeSink::Finish() {
  if (finished_) {
    return true;
  }
  finished_ = true;

  bool succeeded = true;
  if (file_ != nullptr && !file_->Finish()) {
    last_error_ = file_->LastError();
    succeeded = false;
  }

  if (pipe_ != nullptr) {
    pipe_->EndOfInput();
  }
  return succeeded;
}

uint64_t PipeSink::BytesWritten() const {
  if (file_ != nullptr) {
    return file_->BytesWritten();
  }
  return pipe_ != nullptr ? pipe_->samples_delivered() * kSigned16BytesPerSample
                          : 0;
}

uint64_t PipeSink::SamplesWritten() const {
  return file_ != nullptr ? file_->SamplesWritten() : samples_written_;
}

uint64_t PipeSink::SamplesPending() const {
  if (file_ != nullptr) {
    return file_->SamplesPending();
  }
  return pipe_ != nullptr ? pipe_->samples_queued() : 0;
}

}  // namespace ddd::capture
