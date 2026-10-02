/************************************************************************

    pipe_sink.h

    A capture that reaches another program, with or without a file
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <memory>
#include <string>

#include "pipe_writer.h"
#include "sample_sink.h"

namespace ddd::capture {

// The capture-mode sink for --pipe.
//
// Two shapes, one for each way the option is used:
//
//  - The pipe alone. The pipe is the capture: every sample goes to the
//    PipeWriter, which should have been made with WhenFull::kFail, so that a
//    reader falling behind fails the capture rather than thinning it out
//    without a word.
//
//  - A file and a pipe. The file is the capture and the pipe is a copy of it,
//    for watching a quick decode while the original is archived. Each buffer
//    goes to the file first and to the pipe second, and the PipeWriter should
//    have been made with WhenFull::kDrop: the copy can lose a block to a slow
//    reader and the file cannot, so nothing the pipe does is allowed to stand
//    in the file's way. The two do not even share a thread — the file is
//    written here, on the processing thread, exactly as it would be without a
//    pipe, and the pipe by the writer's own thread.
//
// The PipeWriter is shared rather than owned so that whoever started the
// capture can watch it — whether the reader is still there, how much it has
// lost — and wait for it to empty at the end. This object is gone into the
// pipeline by then.
//
// A reader that closes its end does not make a write fail. The pipe-only shape
// is then capturing for nobody, and the file-and-pipe shape is still capturing
// for the file; which of those ends the capture is the owner's decision, made
// from PipeWriter::closed().
//
// Thread-safety: as ISampleSink.
class PipeSink : public ISampleSink {
 public:
  // The pipe alone.
  explicit PipeSink(std::shared_ptr<PipeWriter> pipe);

  // A file, and a copy of it through the pipe. `file` is opened already.
  PipeSink(std::unique_ptr<ISampleSink> file, std::shared_ptr<PipeWriter> pipe);

  ~PipeSink() override;

  const char* Name() const override { return name_.c_str(); }

  bool Write(const uint8_t* wire_data, size_t sample_count) override;

  // Finishes the file, if there is one, and tells the pipe that nothing more is
  // coming. Does not wait for the pipe to empty: that would hold up the
  // processing thread on a reader, which is the one thing this class exists to
  // prevent. PipeWriter::WaitUntilFinished() is how the owner waits for it.
  bool Finish() override;

  // The file's figures when there is a file, and the pipe's when there is not.
  // A file's are what a capture is measured by; the pipe's own account is on
  // the PipeWriter.
  uint64_t BytesWritten() const override;
  uint64_t SamplesWritten() const override;
  uint64_t SamplesPending() const override;

  const std::string& LastError() const override { return last_error_; }

  // The file, or null for the pipe alone.
  const ISampleSink* file() const { return file_.get(); }

 private:
  std::unique_ptr<ISampleSink> file_;
  std::shared_ptr<PipeWriter> pipe_;
  std::string name_;
  std::string last_error_;
  uint64_t samples_written_ = 0;
  bool finished_ = false;
};

}  // namespace ddd::capture
