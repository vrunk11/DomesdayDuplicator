/************************************************************************

    board_setup.cpp

    What the user has declared about the capture board, and the record the
    device keeps it in
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include "board_setup.h"

#include <algorithm>
#include <memory>

#include "usb_device.h"

namespace ddd::capture {
namespace {

// The record's layout. The firmware knows only the framing — the magic, the
// version and the CRC — and everything between them is this application's.
constexpr std::array<uint8_t, 4> kMagic = {'D', 'D', 'B', 'S'};
constexpr size_t kVersionOffset = 4;
constexpr size_t kAdcOffset = 6;
constexpr size_t kRselOffset = 7;
constexpr size_t kDcOffset1VppOffset = 8;
constexpr size_t kDcOffset2VppOffset = 10;
constexpr size_t kMeasured1VppOffset = 12;
constexpr size_t kMeasured2VppOffset = 16;
constexpr size_t kNameOffset = 20;
constexpr size_t kCrcOffset = kBoardSetupRecordLength - 4;

static_assert(kNameOffset + kBoardNameMaximumBytes <= kCrcOffset,
              "the name must fit before the checksum");

// Request types for a vendor request, host to device and device to host.
constexpr uint8_t kVendorWriteRequestType = 0x40;
constexpr uint8_t kVendorReadRequestType = 0xC0;

// A page write and a page read on the device's I2C bus take milliseconds; this
// is for a device that has gone away, and the expected failure — a stall from
// firmware that predates the record — returns at once.
constexpr unsigned int kTimeoutMilliseconds = 2000;

void PutLittleEndian16(BoardSetupRecord& record, size_t offset,
                       uint16_t value) {
  record[offset] = static_cast<uint8_t>(value & 0xFFU);
  record[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void PutLittleEndian32(BoardSetupRecord& record, size_t offset,
                       uint32_t value) {
  for (size_t index = 0; index < 4; ++index) {
    record[offset + index] =
        static_cast<uint8_t>((value >> (8 * index)) & 0xFFU);
  }
}

uint16_t GetLittleEndian16(std::span<const uint8_t> page, size_t offset) {
  return static_cast<uint16_t>(page[offset] |
                               (static_cast<uint16_t>(page[offset + 1]) << 8));
}

uint32_t GetLittleEndian32(std::span<const uint8_t> page, size_t offset) {
  uint32_t value = 0;
  for (size_t index = 0; index < 4; ++index) {
    value |= static_cast<uint32_t>(page[offset + index]) << (8 * index);
  }
  return value;
}

int16_t ClampDcOffset(int value) {
  return static_cast<int16_t>(
      std::clamp(value, kDcOffsetMinimum, kDcOffsetMaximum));
}

bool DcOffsetInRange(int16_t value) {
  return value >= kDcOffsetMinimum && value <= kDcOffsetMaximum;
}

}  // namespace

const char* AdcPartName(AdcPart part) {
  switch (part) {
    case AdcPart::kAds825:
      return "ADS825";
    case AdcPart::kAds828:
      return "ADS828";
  }
  return "ADS825";
}

uint8_t AdcPartMaxRateMhz(AdcPart part) {
  switch (part) {
    case AdcPart::kAds825:
      return 40;
    case AdcPart::kAds828:
      return 75;
  }
  return 40;
}

const char* RselWiringName(RselWiring wiring) {
  switch (wiring) {
    case RselWiring::kAuto:
      return "auto";
    case RselWiring::kLow:
      return "low";
    case RselWiring::kHigh:
      return "high";
  }
  return "high";
}

bool InputRangeIsSelectable(const BoardSetup& setup) {
  return setup.rsel_wiring == RselWiring::kAuto;
}

bool EffectiveRange2Vpp(const BoardSetup& setup, bool requested_2vpp) {
  switch (setup.rsel_wiring) {
    case RselWiring::kAuto:
      return requested_2vpp;
    case RselWiring::kLow:
      return false;
    case RselWiring::kHigh:
      return true;
  }
  return true;
}

int16_t DcOffsetFor(const BoardSetup& setup, bool range_2vpp) {
  return range_2vpp ? setup.dc_offset_2vpp : setup.dc_offset_1vpp;
}

std::string TruncateBoardName(std::string_view name) {
  const size_t nul = name.find('\0');
  if (nul != std::string_view::npos) {
    name = name.substr(0, nul);
  }
  if (name.size() <= kBoardNameMaximumBytes) {
    return std::string(name);
  }

  // Back off to the start of a code point, so the cut never leaves half of a
  // multi-byte character behind. A continuation byte is 10xxxxxx.
  size_t length = kBoardNameMaximumBytes;
  while (length > 0 && (static_cast<uint8_t>(name[length]) & 0xC0U) == 0x80U) {
    --length;
  }
  return std::string(name.substr(0, length));
}

uint32_t BoardSetupCrc32(std::span<const uint8_t> bytes) {
  uint32_t crc = 0xFFFFFFFFU;
  for (const uint8_t byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0 ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

BoardSetupRecord EncodeBoardSetup(const BoardSetup& setup) {
  BoardSetupRecord record{};

  std::copy(kMagic.begin(), kMagic.end(), record.begin());
  PutLittleEndian16(record, kVersionOffset, kBoardSetupLayoutVersion);

  record[kAdcOffset] = static_cast<uint8_t>(setup.adc);
  record[kRselOffset] = static_cast<uint8_t>(setup.rsel_wiring);
  PutLittleEndian16(record, kDcOffset1VppOffset,
                    static_cast<uint16_t>(ClampDcOffset(setup.dc_offset_1vpp)));
  PutLittleEndian16(record, kDcOffset2VppOffset,
                    static_cast<uint16_t>(ClampDcOffset(setup.dc_offset_2vpp)));
  PutLittleEndian32(record, kMeasured1VppOffset, setup.measured_1vpp);
  PutLittleEndian32(record, kMeasured2VppOffset, setup.measured_2vpp);

  const std::string name = TruncateBoardName(setup.name);
  std::copy(name.begin(), name.end(),
            record.begin() + static_cast<std::ptrdiff_t>(kNameOffset));

  PutLittleEndian32(
      record, kCrcOffset,
      BoardSetupCrc32(std::span<const uint8_t>(record.data(), kCrcOffset)));
  return record;
}

DecodedBoardSetup DecodeBoardSetup(std::span<const uint8_t> page) {
  DecodedBoardSetup decoded;

  if (page.size() != kBoardSetupRecordLength ||
      !std::equal(kMagic.begin(), kMagic.end(), page.begin())) {
    decoded.state = BoardSetupRecordState::kBlank;
    return decoded;
  }

  if (GetLittleEndian32(page, kCrcOffset) !=
      BoardSetupCrc32(page.first(kCrcOffset))) {
    decoded.state = BoardSetupRecordState::kDamaged;
    return decoded;
  }

  const uint16_t version = GetLittleEndian16(page, kVersionOffset);
  if (version == 0) {
    decoded.state = BoardSetupRecordState::kDamaged;
    return decoded;
  }
  if (version > kBoardSetupLayoutVersion) {
    decoded.state = BoardSetupRecordState::kNewerLayout;
    return decoded;
  }

  BoardSetup setup;

  const uint8_t adc = page[kAdcOffset];
  const uint8_t rsel = page[kRselOffset];
  if (adc > static_cast<uint8_t>(AdcPart::kAds828) ||
      rsel > static_cast<uint8_t>(RselWiring::kHigh)) {
    decoded.state = BoardSetupRecordState::kDamaged;
    return decoded;
  }
  setup.adc = static_cast<AdcPart>(adc);
  setup.rsel_wiring = static_cast<RselWiring>(rsel);

  setup.dc_offset_1vpp =
      static_cast<int16_t>(GetLittleEndian16(page, kDcOffset1VppOffset));
  setup.dc_offset_2vpp =
      static_cast<int16_t>(GetLittleEndian16(page, kDcOffset2VppOffset));
  if (!DcOffsetInRange(setup.dc_offset_1vpp) ||
      !DcOffsetInRange(setup.dc_offset_2vpp)) {
    decoded.state = BoardSetupRecordState::kDamaged;
    return decoded;
  }

  setup.measured_1vpp = GetLittleEndian32(page, kMeasured1VppOffset);
  setup.measured_2vpp = GetLittleEndian32(page, kMeasured2VppOffset);

  const auto name_begin =
      page.begin() + static_cast<std::ptrdiff_t>(kNameOffset);
  const auto name_end =
      name_begin + static_cast<std::ptrdiff_t>(kBoardNameMaximumBytes);
  setup.name = TruncateBoardName(std::string(name_begin, name_end));

  decoded.state = BoardSetupRecordState::kValid;
  decoded.setup = setup;
  return decoded;
}

BoardSetupReading ReadBoardSetup(IUsbDevice& usb, const std::string& path) {
  BoardSetupReading reading;

  const std::unique_ptr<IUsbControlChannel> channel =
      usb.OpenControlChannel(path);
  if (channel == nullptr) {
    reading.source = BoardSetupSource::kUnavailable;
    return reading;
  }

  BoardSetupRecord page{};
  const int read =
      channel->Transfer(kVendorReadRequestType, kBoardSetupReadRequest, 0, 0,
                        std::span<uint8_t>(page), kTimeoutMilliseconds);
  if (read != static_cast<int>(page.size())) {
    reading.source = BoardSetupSource::kUnsupported;
    return reading;
  }

  const DecodedBoardSetup decoded = DecodeBoardSetup(page);
  reading.setup = decoded.setup;
  switch (decoded.state) {
    case BoardSetupRecordState::kValid:
      reading.source = BoardSetupSource::kDeclared;
      break;
    case BoardSetupRecordState::kBlank:
      reading.source = BoardSetupSource::kBlank;
      break;
    case BoardSetupRecordState::kDamaged:
      reading.source = BoardSetupSource::kDamaged;
      break;
    case BoardSetupRecordState::kNewerLayout:
      reading.source = BoardSetupSource::kNewerLayout;
      break;
  }
  return reading;
}

BoardSetupWriteResult WriteBoardSetup(IUsbDevice& usb, const std::string& path,
                                      const BoardSetup& setup) {
  const std::unique_ptr<IUsbControlChannel> channel =
      usb.OpenControlChannel(path);
  if (channel == nullptr) {
    return BoardSetupWriteResult::kUnavailable;
  }

  BoardSetupRecord record = EncodeBoardSetup(setup);
  const BoardSetupRecord sent = record;
  const int written =
      channel->Transfer(kVendorWriteRequestType, kBoardSetupWriteRequest, 0, 0,
                        std::span<uint8_t>(record), kTimeoutMilliseconds);

  // The readback is the confirmation, whatever the write returned: a device
  // that took the data stage cannot refuse the record afterwards, so the only
  // way to know whether it kept it is to ask what it now holds.
  BoardSetupRecord readback{};
  const int read =
      channel->Transfer(kVendorReadRequestType, kBoardSetupReadRequest, 0, 0,
                        std::span<uint8_t>(readback), kTimeoutMilliseconds);

  if (read != static_cast<int>(readback.size())) {
    return written < 0 ? BoardSetupWriteResult::kUnsupported
                       : BoardSetupWriteResult::kNotConfirmed;
  }

  return readback == sent ? BoardSetupWriteResult::kWritten
                          : BoardSetupWriteResult::kNotConfirmed;
}

}  // namespace ddd::capture
