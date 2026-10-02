/************************************************************************

    test_pipe_sink.cpp

    T1 tests for streaming a capture to another program
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "byte_stream.h"
#include "pipe_sink.h"
#include "pipe_writer.h"
#include "sample_format.h"
#include "sample_sink.h"

namespace ddd::capture {
namespace {

using std::chrono::milliseconds;

// Long enough that a pass is never a matter of luck on a loaded CI runner, and
// short enough that a hang shows up as a failure rather than as a timeout.
constexpr milliseconds kGenerous{5000};

// A reader that can be told to stop reading, to start again, or to go away.
class FakeReader : public IByteStream {
 public:
  bool Write(const uint8_t* data, size_t size) override {
    std::unique_lock<std::mutex> lock(mutex_);
    ++writes_started_;
    changed_.notify_all();
    changed_.wait(lock, [this] { return reading_ || gone_; });
    if (gone_) {
      return false;
    }
    bytes_.insert(bytes_.end(), data, data + size);
    return true;
  }

  // Stop reading: the next write blocks, as it would on a full pipe.
  void Stall() {
    const std::lock_guard<std::mutex> lock(mutex_);
    reading_ = false;
  }

  void Resume() {
    const std::lock_guard<std::mutex> lock(mutex_);
    reading_ = true;
    changed_.notify_all();
  }

  // Close the read end: this write and every later one fails.
  void Leave() {
    const std::lock_guard<std::mutex> lock(mutex_);
    gone_ = true;
    changed_.notify_all();
  }

  // Wait until a write has reached the reader — blocked on it, if stalled.
  bool WaitForWrite() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, kGenerous,
                             [this] { return writes_started_ > 0; });
  }

  std::vector<uint8_t> bytes() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool reading_ = true;
  bool gone_ = false;
  int writes_started_ = 0;
  std::vector<uint8_t> bytes_;
};

// A file that keeps everything it is given, or refuses on request.
class FakeFile : public ISampleSink {
 public:
  const char* Name() const override { return "s16"; }

  bool Write(const uint8_t* wire_data, size_t sample_count) override {
    if (fail_writes) {
      error_ = "the disk is full";
      return false;
    }
    wire_.insert(wire_.end(), wire_data,
                 wire_data + (sample_count * kBytesPerSample));
    samples_ += sample_count;
    return true;
  }

  bool Finish() override {
    finished = true;
    return true;
  }

  uint64_t BytesWritten() const override { return wire_.size(); }
  uint64_t SamplesWritten() const override { return samples_; }
  const std::string& LastError() const override { return error_; }

  const std::vector<uint8_t>& wire() const { return wire_; }

  bool fail_writes = false;
  bool finished = false;

 private:
  std::vector<uint8_t> wire_;
  uint64_t samples_ = 0;
  std::string error_;
};

// Wire words carrying a recognisable ramp, sequence markers already stripped.
std::vector<uint8_t> Ramp(size_t samples, uint16_t start) {
  std::vector<uint8_t> wire(samples * kBytesPerSample);
  for (size_t index = 0; index < samples; ++index) {
    const auto value =
        static_cast<uint16_t>((start + index) % (kMaximumSampleValue + 1));
    wire[index * 2] = static_cast<uint8_t>(value);
    wire[(index * 2) + 1] = static_cast<uint8_t>(value >> 8);
  }
  return wire;
}

std::vector<uint8_t> Converted(const std::vector<uint8_t>& wire,
                               const SampleConversion& conversion) {
  const size_t samples = wire.size() / kBytesPerSample;
  std::vector<uint8_t> out(samples * kSigned16BytesPerSample);
  WireToSigned16LittleEndian(wire.data(), samples, conversion, out.data());
  return out;
}

// One slot's worth, so that the queue arithmetic in these tests is in whole
// slots and can be stated exactly.
constexpr size_t kSlot = PipeWriter::kSlotSamples;

// The smallest queue there is: kMinimumSlots slots.
constexpr size_t kSmallQueue = 1;

// An offset, a bit shift and a reduction together, so that what the pipe
// delivers is shown to be every step of the conversion and not just the offset.
constexpr SampleConversion kEveryStep{3, 1, 1};

TEST(WireToSigned16Test, MatchesTheSingleSampleConversionForEveryCode) {
  constexpr int32_t kOffset = -7;

  std::vector<uint8_t> wire = Ramp(kMaximumSampleValue + 1, 0);
  const std::vector<uint8_t> out = Converted(wire, SampleConversion{kOffset});

  for (int32_t code = 0; code <= kMaximumSampleValue; ++code) {
    const auto expected =
        static_cast<uint16_t>(ToCorrectedSigned16Bit(code, kOffset));
    const auto index = static_cast<size_t>(code);
    // Little-endian, whatever the host is.
    EXPECT_EQ(out[index * 2], static_cast<uint8_t>(expected)) << code;
    EXPECT_EQ(out[(index * 2) + 1], static_cast<uint8_t>(expected >> 8))
        << code;
  }
}

TEST(PipeWriterTest, DeliversEverythingInOrderAndThenStops) {
  auto reader = std::make_shared<FakeReader>();
  PipeWriter pipe(reader, kEveryStep, PipeWriter::WhenFull::kFail, kSmallQueue);

  const std::vector<uint8_t> first = Ramp(1000, 0);
  const std::vector<uint8_t> second = Ramp(kSlot + 17, 500);
  ASSERT_TRUE(pipe.Offer(first.data(), 1000));
  ASSERT_TRUE(pipe.Offer(second.data(), kSlot + 17));
  pipe.EndOfInput();

  ASSERT_TRUE(pipe.WaitUntilFinished(kGenerous));

  std::vector<uint8_t> expected = Converted(first, kEveryStep);
  const std::vector<uint8_t> rest = Converted(second, kEveryStep);
  expected.insert(expected.end(), rest.begin(), rest.end());
  EXPECT_EQ(reader->bytes(), expected);

  EXPECT_EQ(pipe.samples_accepted(), 1000 + kSlot + 17);
  EXPECT_EQ(pipe.samples_delivered(), 1000 + kSlot + 17);
  EXPECT_EQ(pipe.samples_dropped(), 0U);
  EXPECT_EQ(pipe.samples_queued(), 0U);
  EXPECT_FALSE(pipe.closed());
}

TEST(PipeWriterTest, AloneItRefusesWhatAStalledReaderHasNoRoomFor) {
  auto reader = std::make_shared<FakeReader>();
  reader->Stall();
  PipeWriter pipe(reader, SampleConversion{}, PipeWriter::WhenFull::kFail,
                  kSmallQueue);

  const std::vector<uint8_t> wire = Ramp(kSlot, 0);
  for (size_t slot = 0; slot < PipeWriter::kMinimumSlots; ++slot) {
    ASSERT_TRUE(pipe.Offer(wire.data(), kSlot)) << slot;
  }

  // Refused whole, rather than half taken: there is no partial success for
  // samples that exist nowhere else.
  EXPECT_FALSE(pipe.Offer(wire.data(), kSlot));
  EXPECT_EQ(pipe.samples_accepted(), PipeWriter::kMinimumSlots * kSlot);
  EXPECT_EQ(pipe.samples_dropped(), 0U);

  reader->Resume();
  pipe.EndOfInput();
  ASSERT_TRUE(pipe.WaitUntilFinished(kGenerous));
  EXPECT_EQ(pipe.samples_delivered(), PipeWriter::kMinimumSlots * kSlot);
}

TEST(PipeWriterTest, BesideAFileItDropsRatherThanWaits) {
  auto reader = std::make_shared<FakeReader>();
  reader->Stall();
  PipeWriter pipe(reader, SampleConversion{}, PipeWriter::WhenFull::kDrop,
                  kSmallQueue);

  const std::vector<uint8_t> wire = Ramp(kSlot, 0);
  constexpr size_t kOffers = PipeWriter::kMinimumSlots + 6;

  const auto started = std::chrono::steady_clock::now();
  for (size_t offer = 0; offer < kOffers; ++offer) {
    EXPECT_TRUE(pipe.Offer(wire.data(), kSlot)) << offer;
  }
  // Every offer came straight back, with the reader taking nothing.
  EXPECT_LT(std::chrono::steady_clock::now() - started, kGenerous);

  EXPECT_EQ(pipe.samples_accepted(), PipeWriter::kMinimumSlots * kSlot);
  EXPECT_EQ(pipe.samples_dropped(),
            (kOffers - PipeWriter::kMinimumSlots) * kSlot);

  reader->Resume();
  pipe.EndOfInput();
  ASSERT_TRUE(pipe.WaitUntilFinished(kGenerous));

  // What was kept arrives whole and in order: whole slots are lost, never
  // parts of one.
  EXPECT_EQ(pipe.samples_delivered(), PipeWriter::kMinimumSlots * kSlot);
  EXPECT_EQ(reader->bytes().size(),
            PipeWriter::kMinimumSlots * kSlot * kSigned16BytesPerSample);
}

TEST(PipeWriterTest, AReaderThatLeavesClosesThePipeWithoutFailingAnOffer) {
  auto reader = std::make_shared<FakeReader>();
  PipeWriter pipe(reader, SampleConversion{}, PipeWriter::WhenFull::kFail,
                  kSmallQueue);

  reader->Leave();
  const std::vector<uint8_t> wire = Ramp(100, 0);
  ASSERT_TRUE(pipe.Offer(wire.data(), 100));
  ASSERT_TRUE(reader->WaitForWrite());

  const auto deadline = std::chrono::steady_clock::now() + kGenerous;
  while (!pipe.closed() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(milliseconds(1));
  }
  ASSERT_TRUE(pipe.closed());

  // Nobody to give it to is not a failure of the offer. What the reader
  // leaving means is the owner's decision.
  EXPECT_TRUE(pipe.Offer(wire.data(), 100));

  pipe.EndOfInput();
  ASSERT_TRUE(pipe.WaitUntilFinished(kGenerous));
  EXPECT_EQ(pipe.samples_delivered(), 0U);
  EXPECT_EQ(pipe.samples_queued(), 0U);
}

TEST(PipeWriterTest, TeardownIsNotHeldUpByAReaderThatNeverReadsAgain) {
  auto reader = std::make_shared<FakeReader>();
  reader->Stall();

  const auto started = std::chrono::steady_clock::now();
  {
    PipeWriter pipe(reader, SampleConversion{}, PipeWriter::WhenFull::kDrop,
                    kSmallQueue);
    const std::vector<uint8_t> wire = Ramp(100, 0);
    ASSERT_TRUE(pipe.Offer(wire.data(), 100));
    ASSERT_TRUE(reader->WaitForWrite());
  }
  EXPECT_LT(std::chrono::steady_clock::now() - started, kGenerous);

  // Let the abandoned writer finish, so the process ends with nothing blocked.
  // The reader it holds is its own reference, which is why this is safe.
  reader->Resume();
}

TEST(PipeSinkTest, AFileGetsEveryBufferWhileTheReaderIsStalled) {
  auto reader = std::make_shared<FakeReader>();
  reader->Stall();
  auto pipe = std::make_shared<PipeWriter>(
      reader, SampleConversion{}, PipeWriter::WhenFull::kDrop, kSmallQueue);

  auto file = std::make_unique<FakeFile>();
  FakeFile* const file_view = file.get();
  PipeSink sink(std::move(file), pipe);

  EXPECT_STREQ(sink.Name(), "s16+pipe");

  std::vector<uint8_t> all;
  constexpr size_t kBuffers = PipeWriter::kMinimumSlots + 3;
  for (size_t buffer = 0; buffer < kBuffers; ++buffer) {
    const std::vector<uint8_t> wire =
        Ramp(kSlot, static_cast<uint16_t>(buffer));
    ASSERT_TRUE(sink.Write(wire.data(), kSlot)) << buffer;
    all.insert(all.end(), wire.begin(), wire.end());
  }

  // The recording is complete. The copy lost what it had no room for.
  EXPECT_EQ(file_view->wire(), all);
  EXPECT_EQ(sink.SamplesWritten(), kBuffers * kSlot);
  EXPECT_EQ(sink.BytesWritten(), all.size());
  EXPECT_GT(pipe->samples_dropped(), 0U);

  reader->Resume();
  EXPECT_TRUE(sink.Finish());
  EXPECT_TRUE(file_view->finished);
  EXPECT_TRUE(pipe->WaitUntilFinished(kGenerous));
}

TEST(PipeSinkTest, AFileThatFailsFailsTheCapture) {
  auto reader = std::make_shared<FakeReader>();
  auto pipe = std::make_shared<PipeWriter>(
      reader, SampleConversion{}, PipeWriter::WhenFull::kDrop, kSmallQueue);
  auto file = std::make_unique<FakeFile>();
  file->fail_writes = true;
  PipeSink sink(std::move(file), pipe);

  const std::vector<uint8_t> wire = Ramp(100, 0);
  EXPECT_FALSE(sink.Write(wire.data(), 100));
  EXPECT_EQ(sink.LastError(), "the disk is full");

  // Not offered to the pipe either: the copy does not get ahead of the
  // recording it is a copy of.
  EXPECT_EQ(pipe->samples_accepted(), 0U);
}

TEST(PipeSinkTest, AloneAReaderThatFallsBehindFailsTheCapture) {
  auto reader = std::make_shared<FakeReader>();
  reader->Stall();
  auto pipe = std::make_shared<PipeWriter>(
      reader, SampleConversion{}, PipeWriter::WhenFull::kFail, kSmallQueue);
  PipeSink sink(pipe);

  EXPECT_STREQ(sink.Name(), "pipe");

  const std::vector<uint8_t> wire = Ramp(kSlot, 0);
  for (size_t buffer = 0; buffer < PipeWriter::kMinimumSlots; ++buffer) {
    ASSERT_TRUE(sink.Write(wire.data(), kSlot)) << buffer;
  }
  EXPECT_FALSE(sink.Write(wire.data(), kSlot));
  EXPECT_NE(sink.LastError().find("standard output"), std::string::npos);

  // Counted by what reached the pipe, since there is no file to count by.
  EXPECT_EQ(sink.SamplesWritten(), PipeWriter::kMinimumSlots * kSlot);
  EXPECT_EQ(sink.BytesWritten(), 0U);
  EXPECT_EQ(sink.SamplesPending(), PipeWriter::kMinimumSlots * kSlot);

  reader->Resume();
  EXPECT_TRUE(sink.Finish());
  ASSERT_TRUE(pipe->WaitUntilFinished(kGenerous));
  EXPECT_EQ(sink.BytesWritten(),
            PipeWriter::kMinimumSlots * kSlot * kSigned16BytesPerSample);
  EXPECT_EQ(sink.SamplesPending(), 0U);
}

TEST(PipeSinkTest, FinishingDoesNotWaitForTheReader) {
  auto reader = std::make_shared<FakeReader>();
  reader->Stall();
  auto pipe = std::make_shared<PipeWriter>(
      reader, SampleConversion{}, PipeWriter::WhenFull::kFail, kSmallQueue);
  PipeSink sink(pipe);

  const std::vector<uint8_t> wire = Ramp(100, 0);
  ASSERT_TRUE(sink.Write(wire.data(), 100));
  ASSERT_TRUE(reader->WaitForWrite());

  // On the processing thread this is between two buffers. It must not become
  // a wait on another program.
  const auto started = std::chrono::steady_clock::now();
  EXPECT_TRUE(sink.Finish());
  EXPECT_LT(std::chrono::steady_clock::now() - started, kGenerous);
  EXPECT_FALSE(pipe->finished());

  reader->Resume();
  EXPECT_TRUE(pipe->WaitUntilFinished(kGenerous));
  EXPECT_EQ(pipe->samples_delivered(), 100U);
}

}  // namespace
}  // namespace ddd::capture
