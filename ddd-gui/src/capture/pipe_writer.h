/************************************************************************

    pipe_writer.h

    Streaming a capture to another program without waiting for it
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include "byte_stream.h"

namespace ddd::capture {

// Hands samples to another program through a stream, on a thread of its own.
//
// The program at the other end of a pipe reads at whatever rate it reads, and
// nothing about that may reach the capture. So the processing thread never
// writes to the stream: it copies each buffer into a bounded queue and goes
// back to its own work, and a writer thread here converts the samples to signed
// 16-bit and does the writing — and does the waiting, if there is any. The copy
// is the only cost the capture sees, and it costs the same whether the reader
// is keeping up, is a second behind, or has stopped reading altogether.
//
// What happens when the queue is full is the caller's choice, because it means
// two different things:
//
//  - kFail: the pipe is the capture. A reader that cannot keep up is losing
//    samples that exist nowhere else, and saying so is the only honest thing
//    to do. Offer() refuses, and the capture fails the way a full disk fails
//    it.
//
//  - kDrop: a file is being written as well, and the pipe is a copy of it for
//    something like a preview. The file is the recording; the copy losing a
//    block is regrettable and nothing more. Offer() drops what does not fit,
//    counts it, and the file never knows.
//
// A reader that closes its end is the third case, and it is neither a failure
// nor a reason to wait: the writer stops writing, closed() becomes true, and
// whoever owns the run decides what that means for it.
//
// Thread-safety: Offer() and EndOfInput() from one thread — the processing
// thread. The counters and the waits may be read from any thread.
class PipeWriter {
 public:
  enum class WhenFull {
    kFail,
    kDrop,
  };

  // Samples per queue slot: 1 MiB on the wire. A pipeline buffer is split
  // across as many slots as it needs.
  static constexpr size_t kSlotSamples = size_t{1} << 19;

  // The queue's size when nobody asks for another. 128 MiB is most of a second
  // at 75 MSPS, which is far more stall than a reader that can keep up at all
  // ever has, and little enough beside the ring to allocate without a thought.
  static constexpr size_t kDefaultQueueBytes = size_t{128} << 20;

  // The fewest slots a queue is given, whatever was asked for, so that one
  // pipeline buffer always fits in an empty queue.
  static constexpr size_t kMinimumSlots = 4;

  // dc_offset is taken out of every sample written, as RawSink takes it out —
  // the two write the same conversion of the same samples.
  PipeWriter(std::shared_ptr<IByteStream> stream, int32_t dc_offset,
             WhenFull when_full, size_t queue_bytes = kDefaultQueueBytes);

  // Ends the input and gives the writer a moment to empty the queue. A writer
  // still blocked on its reader after that is left to finish on its own rather
  // than waited for: nothing it holds belongs to this object, and a reader that
  // never reads again must not be able to hang whatever is tearing this down.
  ~PipeWriter();

  PipeWriter(const PipeWriter&) = delete;
  PipeWriter& operator=(const PipeWriter&) = delete;
  PipeWriter(PipeWriter&&) = delete;
  PipeWriter& operator=(PipeWriter&&) = delete;

  // Queue a buffer of wire words, sequence markers already stripped. Never
  // blocks. False only under kFail, when the whole buffer does not fit.
  bool Offer(const uint8_t* wire_data, size_t sample_count);

  // Nothing more is coming. The writer empties the queue and then stops.
  void EndOfInput();

  // Wait for the writer to stop, which it does once the queue is empty after
  // EndOfInput() — delivered, or thrown away if the reader has gone. True if it
  // stopped within the timeout.
  bool WaitUntilFinished(std::chrono::milliseconds timeout) const;

  // The reader closed its end, or the stream failed. Final.
  bool closed() const;

  // The writer has stopped. See WaitUntilFinished().
  bool finished() const;

  WhenFull when_full() const;

  // Samples the queue took.
  uint64_t samples_accepted() const;

  // Samples the stream took.
  uint64_t samples_delivered() const;

  // Samples refused for want of room. Always zero under kFail, where a buffer
  // that does not fit is refused whole and reported by Offer() instead.
  uint64_t samples_dropped() const;

  // Taken but not yet delivered.
  uint64_t samples_queued() const;

 private:
  struct State;

  static void Run(const std::shared_ptr<State>& state);

  // Shared with the writer thread, which may outlive this object — see the
  // destructor.
  std::shared_ptr<State> state_;
  std::thread thread_;
};

}  // namespace ddd::capture
