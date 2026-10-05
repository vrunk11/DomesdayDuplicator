/************************************************************************

    pipe_writer.cpp

    Streaming a capture to another program without waiting for it
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "pipe_writer.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "sample_format.h"

namespace ddd::capture {
namespace {

// How long the writer sleeps between looks at an empty queue when nothing has
// woken it. The producer does wake it, but without taking the lock — taking one
// would be a way for the writer to make the capture wait — so a wake-up can
// land between the writer's look and its sleep. This is the bound on what that
// costs: a few milliseconds of latency, never a sample.
constexpr std::chrono::milliseconds kIdlePoll{5};

// How long the destructor lets the writer empty its queue before leaving it to
// finish alone. Long enough for a reader that keeps up to take a full queue.
constexpr std::chrono::milliseconds kTeardownWait{2000};

}  // namespace

struct PipeWriter::State {
  struct Slot {
    std::vector<uint8_t> wire;
    size_t samples = 0;

    // Already signed 16-bit little-endian, as the requantiser hands samples
    // on, rather than wire words for the writer to convert.
    bool converted = false;
  };

  std::shared_ptr<IByteStream> stream;
  SampleConversion conversion;
  WhenFull when_full = WhenFull::kFail;

  std::vector<Slot> slots;

  // A single-producer, single-consumer ring over `slots`. head counts slots the
  // producer has filled and tail slots the writer has emptied; head - tail is
  // how many are full, and neither ever goes backwards.
  std::atomic<uint64_t> head{0};
  std::atomic<uint64_t> tail{0};

  std::atomic<bool> ended{false};
  std::atomic<bool> closed{false};
  std::atomic<bool> finished{false};

  std::atomic<uint64_t> accepted{0};
  std::atomic<uint64_t> delivered{0};
  std::atomic<uint64_t> dropped{0};

  // Samples taken into the queue and then thrown away because the reader had
  // gone by the time they reached the front of it. Only so that queued
  // arithmetic comes out at zero once the writer has stopped.
  std::atomic<uint64_t> discarded{0};

  // Only for sleeping on. Nothing the producer does takes it.
  mutable std::mutex mutex;
  std::condition_variable wake;
  mutable std::condition_variable stopped;
};

PipeWriter::PipeWriter(std::shared_ptr<IByteStream> stream,
                       const SampleConversion& conversion, WhenFull when_full,
                       size_t queue_bytes)
    : state_(std::make_shared<State>()) {
  state_->stream = std::move(stream);
  state_->conversion = conversion;
  state_->when_full = when_full;

  const size_t slot_bytes = kSlotSamples * kBytesPerSample;
  const size_t slot_count = std::max(kMinimumSlots, queue_bytes / slot_bytes);

  // Every slot sized now. Nothing on the capture path allocates.
  state_->slots.resize(slot_count);
  for (State::Slot& slot : state_->slots) {
    slot.wire.resize(slot_bytes);
  }

  // A copy of the shared pointer rather than this: the writer may outlive the
  // object, and must not be left holding a pointer into it when it does.
  thread_ = std::thread(&PipeWriter::Run, state_);
}

PipeWriter::~PipeWriter() {
  EndOfInput();
  if (thread_.joinable()) {
    if (WaitUntilFinished(kTeardownWait)) {
      thread_.join();
    } else {
      thread_.detach();
    }
  }
}

bool PipeWriter::Offer(const uint8_t* wire_data, size_t sample_count) {
  return Enqueue(sample_count, false,
                 [wire_data](uint8_t* destination, size_t first, size_t count) {
                   std::memcpy(destination,
                               wire_data + (first * kBytesPerSample),
                               count * kBytesPerSample);
                 });
}

bool PipeWriter::OfferConverted(const int16_t* samples, size_t sample_count) {
  return Enqueue(sample_count, true,
                 [samples](uint8_t* destination, size_t first, size_t count) {
                   for (size_t index = 0; index < count; ++index) {
                     const auto sample =
                         static_cast<uint16_t>(samples[first + index]);
                     destination[index * kSigned16BytesPerSample] =
                         static_cast<uint8_t>(sample);
                     destination[(index * kSigned16BytesPerSample) + 1] =
                         static_cast<uint8_t>(sample >> 8);
                   }
                 });
}

template <typename Copy>
bool PipeWriter::Enqueue(size_t sample_count, bool converted, Copy copy) {
  State& state = *state_;

  // Nobody to give it to. Not a failure — what a reader leaving means is the
  // owner's call, made by looking at closed().
  if (state.closed.load(std::memory_order_acquire) ||
      state.ended.load(std::memory_order_acquire) || sample_count == 0) {
    return true;
  }

  const uint64_t slot_count = state.slots.size();
  uint64_t head = state.head.load(std::memory_order_relaxed);

  if (state.when_full == WhenFull::kFail) {
    const uint64_t needed = (sample_count + kSlotSamples - 1) / kSlotSamples;
    const uint64_t in_use = head - state.tail.load(std::memory_order_acquire);
    if (needed > slot_count - in_use) {
      return false;
    }
  }

  size_t done = 0;
  while (done < sample_count) {
    if (head - state.tail.load(std::memory_order_acquire) >= slot_count) {
      // Only reachable under kDrop: kFail made sure of the room above, and the
      // writer only ever makes more of it.
      state.dropped.fetch_add(sample_count - done, std::memory_order_relaxed);
      break;
    }

    const size_t count = std::min(kSlotSamples, sample_count - done);
    State::Slot& slot = state.slots[head % slot_count];
    copy(slot.wire.data(), done, count);
    slot.samples = count;
    slot.converted = converted;

    ++head;
    state.head.store(head, std::memory_order_release);
    state.accepted.fetch_add(count, std::memory_order_relaxed);
    done += count;
  }

  state.wake.notify_one();
  return true;
}

void PipeWriter::EndOfInput() {
  state_->ended.store(true, std::memory_order_release);
  state_->wake.notify_one();
}

bool PipeWriter::WaitUntilFinished(std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(state_->mutex);
  return state_->stopped.wait_for(lock, timeout, [this] {
    return state_->finished.load(std::memory_order_acquire);
  });
}

bool PipeWriter::closed() const {
  return state_->closed.load(std::memory_order_acquire);
}

bool PipeWriter::finished() const {
  return state_->finished.load(std::memory_order_acquire);
}

PipeWriter::WhenFull PipeWriter::when_full() const { return state_->when_full; }

uint64_t PipeWriter::samples_accepted() const {
  return state_->accepted.load(std::memory_order_relaxed);
}

uint64_t PipeWriter::samples_delivered() const {
  return state_->delivered.load(std::memory_order_relaxed);
}

uint64_t PipeWriter::samples_dropped() const {
  return state_->dropped.load(std::memory_order_relaxed);
}

uint64_t PipeWriter::samples_queued() const {
  const uint64_t accepted = samples_accepted();
  const uint64_t gone =
      samples_delivered() + state_->discarded.load(std::memory_order_relaxed);
  return accepted > gone ? accepted - gone : 0;
}

void PipeWriter::Run(const std::shared_ptr<State>& shared) {
  State& state = *shared;
  const uint64_t slot_count = state.slots.size();
  std::vector<uint8_t> converted(kSlotSamples * kSigned16BytesPerSample);

  for (;;) {
    const uint64_t tail = state.tail.load(std::memory_order_relaxed);

    if (tail == state.head.load(std::memory_order_acquire)) {
      // Read in this order: the producer publishes its last slot before it
      // ends the input, so once the end is seen a second look at head is
      // certain to include everything that was ever offered.
      if (state.ended.load(std::memory_order_acquire) &&
          tail == state.head.load(std::memory_order_acquire)) {
        break;
      }

      std::unique_lock<std::mutex> lock(state.mutex);
      state.wake.wait_for(lock, kIdlePoll, [&state, tail] {
        return tail != state.head.load(std::memory_order_acquire) ||
               state.ended.load(std::memory_order_acquire);
      });
      continue;
    }

    const State::Slot& slot = state.slots[tail % slot_count];

    if (state.closed.load(std::memory_order_relaxed)) {
      state.discarded.fetch_add(slot.samples, std::memory_order_relaxed);
    } else {
      const uint8_t* bytes = slot.wire.data();
      if (!slot.converted) {
        WireToSigned16LittleEndian(slot.wire.data(), slot.samples,
                                   state.conversion, converted.data());
        bytes = converted.data();
      }
      if (state.stream->Write(bytes, slot.samples * kSigned16BytesPerSample)) {
        state.delivered.fetch_add(slot.samples, std::memory_order_relaxed);
      } else {
        state.discarded.fetch_add(slot.samples, std::memory_order_relaxed);
        state.closed.store(true, std::memory_order_release);
      }
    }

    // Released only now, so the producer cannot refill the slot while it is
    // still being read.
    state.tail.store(tail + 1, std::memory_order_release);
  }

  {
    const std::lock_guard<std::mutex> lock(state.mutex);
    state.finished.store(true, std::memory_order_release);
  }
  state.stopped.notify_all();
}

}  // namespace ddd::capture
