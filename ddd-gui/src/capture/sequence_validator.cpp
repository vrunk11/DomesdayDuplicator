/************************************************************************

    sequence_validator.cpp

    Proving a capture is bit-perfect, one buffer at a time
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "sequence_validator.h"

#include <algorithm>

#include "sample_format.h"

namespace ddd::capture {

const char* SequenceStateName(SequenceState state) {
  switch (state) {
    case SequenceState::kSynchronising:
      return "synchronising";
    case SequenceState::kRunning:
      return "running";
    case SequenceState::kDisabled:
      return "disabled";
    case SequenceState::kFailed:
      return "failed";
  }
  return "unknown";
}

namespace {

// The value that follows this one in the cycle, 0..62 and back to 0.
constexpr uint8_t NextCounterValue(uint8_t value) {
  const uint32_t next = static_cast<uint32_t>(value) + 1;
  return static_cast<uint8_t>(next >= kSequenceCounterValues ? 0 : next);
}

}  // namespace

void SequenceValidator::Reset() {
  state_ = SequenceState::kSynchronising;
  counter_value_ = 0;
  run_length_ = 0;
  samples_per_counter_ = 0;
}

SequenceValidator::Outcome SequenceValidator::Process(uint8_t* buffer,
                                                      size_t byte_count) {
  Outcome outcome;

  const size_t sample_count = byte_count / kBytesPerSample;
  if (sample_count == 0) {
    return outcome;
  }

  // Already failed: a mismatch aborts the capture, so there is no second
  // buffer to look at. Reported rather than asserted, because the orchestrator
  // shutting down may still drain one more buffer.
  if (state_ == SequenceState::kFailed) {
    outcome.ok = false;
    return outcome;
  }

  // Where validation starts. Everything before it is still stripped and
  // measured — only the counter check is skipped, and only while acquiring
  // lock.
  size_t validate_from = 0;

  if (state_ == SequenceState::kSynchronising) {
    // Each counter value covers a whole block of consecutive samples, so a
    // change must appear within one block plus one wherever the buffer happens
    // to start. The longest block any gateware emits is the bound to use: a
    // search sized for the shorter one would call a legacy stream markerless
    // whenever the buffer opened just after a boundary. Not finding a change in
    // that span means the stream carries no markers.
    const uint8_t first_counter =
        static_cast<uint8_t>(buffer[1] >> kSequenceCounterHighByteShift);
    outcome.first_counter = first_counter;
    const size_t search_limit =
        std::min<size_t>(sample_count, kMaximumSamplesPerSequenceCounter + 1);

    bool found = false;
    for (size_t index = 1; index < search_limit; ++index) {
      const uint8_t counter =
          static_cast<uint8_t>(buffer[(index * kBytesPerSample) + 1] >>
                               kSequenceCounterHighByteShift);
      if (counter != first_counter) {
        // This sample is the first carrying the new value, so the phase is
        // known exactly from here on. Deriving it forwards from the change,
        // rather than back-calculating what the counter must have been at the
        // start of the buffer, is the same answer with one fewer step to get
        // wrong.
        counter_value_ = counter;
        run_length_ = 0;
        validate_from = index;
        state_ = SequenceState::kRunning;
        outcome.synchronised_here = true;
        outcome.synchronisation_sample_index = index;
        found = true;
        break;
      }
    }

    if (!found) {
      state_ = SequenceState::kDisabled;
    }
  }

  const bool checking = (state_ == SequenceState::kRunning);

  // How long the current run may get before the missing change is itself the
  // fault. The block length once it is known; until then the longer of the two
  // this project has shipped, because assuming the shorter one would fail a
  // legacy stream at its very first boundary.
  uint32_t run_limit = samples_per_counter_ != 0
                           ? samples_per_counter_
                           : kMaximumSamplesPerSequenceCounter;

  uint16_t minimum_value = UINT16_MAX;
  uint16_t maximum_value = 0;
  uint64_t clipped_low = 0;
  uint64_t clipped_high = 0;
  uint64_t sum_of_squares = 0;
  uint64_t measured = 0;

  for (size_t index = 0; index < sample_count; ++index) {
    uint8_t* word = buffer + (index * kBytesPerSample);
    const uint8_t high_byte = word[1];

    if (checking && index >= validate_from) {
      const uint8_t counter =
          static_cast<uint8_t>(high_byte >> kSequenceCounterHighByteShift);

      if (counter == counter_value_) {
        ++run_length_;
        if (run_length_ > run_limit) {
          // The run outlasted the block length, so the change that should have
          // ended it never arrived. A whole number of blocks went missing,
          // which is the direction a lost transfer usually goes.
          state_ = SequenceState::kFailed;
          outcome.ok = false;
          outcome.mismatch_sample_index = index;
          outcome.expected_counter = NextCounterValue(counter_value_);
          outcome.actual_counter = counter;
          outcome.samples_expected_remaining = 0;
          break;
        }
      } else if (counter == NextCounterValue(counter_value_) &&
                 (samples_per_counter_ != 0
                      ? run_length_ == samples_per_counter_
                      : IsKnownSamplesPerSequenceCounter(run_length_))) {
        // A complete run, ended by the right value. The first one is also what
        // measures the block length: from here the check is against a number
        // this stream has demonstrated rather than one of two it might use.
        if (samples_per_counter_ == 0) {
          samples_per_counter_ = run_length_;
          run_limit = run_length_;
        }
        counter_value_ = counter;
        run_length_ = 1;
      } else {
        // Either the counter jumped, or it changed before the block was over.
        // Both mean samples between here and the last boundary are gone.
        state_ = SequenceState::kFailed;
        outcome.ok = false;
        outcome.mismatch_sample_index = index;
        outcome.expected_counter = counter_value_;
        outcome.actual_counter = counter;
        outcome.samples_expected_remaining = run_limit - run_length_;
        break;
      }
    }

    // Strip the marker in place, so what reaches the sink is sample data only
    word[1] = static_cast<uint8_t>(high_byte & kSampleValueHighByteMask);

    const uint16_t value = static_cast<uint16_t>(
        static_cast<uint16_t>(word[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(word[1]) << 8));

    minimum_value = std::min(minimum_value, value);
    maximum_value = std::max(maximum_value, value);

    if (value == kMinimumSampleValue) {
      ++clipped_low;
    } else if (value == kMaximumSampleValue) {
      ++clipped_high;
    }

    const int32_t centred = static_cast<int32_t>(value) - kSampleZeroOffset;
    sum_of_squares += static_cast<uint64_t>(centred * centred);
    ++measured;
  }

  outcome.tally.sample_count = measured;
  outcome.tally.minimum_value = minimum_value;
  outcome.tally.maximum_value = maximum_value;
  outcome.tally.clipped_low_count = clipped_low;
  outcome.tally.clipped_high_count = clipped_high;
  outcome.tally.sum_of_squares = sum_of_squares;
  return outcome;
}

}  // namespace ddd::capture
