/************************************************************************

    board_setup.h

    What the user has declared about the capture board, and the record the
    device keeps it in
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "wire_protocol.h"

namespace ddd::capture {

class IUsbDevice;

// A board setup is a declaration, not a capture setting.
//
// It says what the capture board *is*: which converter is soldered to it, how
// that converter's RSEL pin is wired, and the DC offset its analogue front end
// puts on the signal. None of that changes from one capture to the next, and
// none of it can be read off the hardware — the gateware drives RSEL whether
// or not anything is wired to it, and an ADC run past its rated speed keeps
// producing samples that nothing downstream can tell are wrong. So the user
// declares it, once per board, and the capture settings are bounded by it
// rather than containing it.
//
// It is kept on the device rather than on this machine, in the last page of
// the FX3's boot EEPROM, because the FX3 kit is a separate board that plugs
// into the capture board: a declaration kept by the application would follow
// the computer, and one kept by the kit follows the kit. That is still not the
// capture board itself, which is why the record carries a name the user
// chooses — moving the kit to another board and seeing the old name is the
// prompt to declare the new one.

// The converter fitted to the capture board. Declared by reading the part
// number off the chip; nothing on the board reports it.
enum class AdcPart : uint8_t {
  kAds825 = 0,  // 40 MSPS
  kAds828 = 1,  // 75 MSPS
};

// How the converter's RSEL pin is wired. RSEL selects the input range on both
// parts, and whether the board routes it to the FPGA is independent of which
// part is fitted.
enum class RselWiring : uint8_t {
  // Routed to the FPGA: the range is chosen per capture.
  kAuto = 0,
  // Tied low on the board: always 1Vpp, whatever the capture settings say.
  kLow = 1,
  // Tied high on the board: always 2Vpp. What a board is assumed to be when
  // nothing has been declared.
  kHigh = 2,
};

// The DC offset is a signed count of converter codes — how far above code 512
// the signal's mean sits with nothing connected to the input. Whole codes and
// not fractions of one, because the correction is applied to samples that are
// written as the code times 64: a fractional offset would fill the six low bits
// every sample otherwise leaves at zero, and FLAC would have to store them.
inline constexpr int kDcOffsetMinimum = -512;
inline constexpr int kDcOffsetMaximum = 511;

// The board name, in UTF-8, at most this many bytes.
inline constexpr size_t kBoardNameMaximumBytes = 32;

// The record layout this build writes, and the only one it reads. A later
// layout is recognised as a record and refused as one this build cannot
// interpret, rather than half read.
inline constexpr uint16_t kBoardSetupLayoutVersion = 1;

struct BoardSetup {
  std::string name;
  AdcPart adc = AdcPart::kAds825;
  RselWiring rsel_wiring = RselWiring::kHigh;

  // Per input range, because a DC error in the front end is a voltage and
  // 1Vpp spreads the same voltage over twice as many codes.
  int16_t dc_offset_1vpp = 0;
  int16_t dc_offset_2vpp = 0;

  // When each offset was last measured, in seconds since the Unix epoch, or 0
  // for one entered by hand or never set. Shown beside the offset so that a
  // figure measured on another board, months ago, looks like what it is.
  uint32_t measured_1vpp = 0;
  uint32_t measured_2vpp = 0;

  bool operator==(const BoardSetup&) const = default;
};

// "ADS825" or "ADS828".
const char* AdcPartName(AdcPart part);

// The fastest ADC rate, in MHz, the part is rated for.
uint8_t AdcPartMaxRateMhz(AdcPart part);

// "auto", "low" or "high" — the spelling the metadata records.
const char* RselWiringName(RselWiring wiring);

// Whether the capture settings may choose the input range, which is only when
// RSEL reaches the FPGA.
bool InputRangeIsSelectable(const BoardSetup& setup);

// The input range a capture actually runs at: the requested one when RSEL is
// routed to the FPGA, the wired one when it is not.
bool EffectiveRange2Vpp(const BoardSetup& setup, bool requested_2vpp);

// The declared offset for an input range.
int16_t DcOffsetFor(const BoardSetup& setup, bool range_2vpp);

// A name cut to what the record holds: at most kBoardNameMaximumBytes, never
// part of a UTF-8 sequence, and nothing from the first NUL on.
std::string TruncateBoardName(std::string_view name);

// The CRC-32 the record carries: the reflected one, polynomial 0xEDB88320,
// initial value all ones and final complement — the same one the FPGA boot
// block carries, and the one the firmware checks the record against.
uint32_t BoardSetupCrc32(std::span<const uint8_t> bytes);

using BoardSetupRecord = std::array<uint8_t, kBoardSetupRecordLength>;

// The record for a setup. Offsets outside kDcOffsetMinimum..kDcOffsetMaximum
// are clamped into it and the name is cut with TruncateBoardName.
BoardSetupRecord EncodeBoardSetup(const BoardSetup& setup);

// What a page read off the device turned out to be.
enum class BoardSetupRecordState {
  // A record this build understands.
  kValid,
  // Not a record at all — the page of an EEPROM nothing has declared on.
  kBlank,
  // A record, but damaged or holding values this build cannot interpret.
  kDamaged,
  // A record written by a newer application, in a layout this build does not
  // know.
  kNewerLayout,
};

struct DecodedBoardSetup {
  BoardSetupRecordState state = BoardSetupRecordState::kBlank;

  // The record's contents when valid, and the defaults otherwise.
  BoardSetup setup;
};

DecodedBoardSetup DecodeBoardSetup(std::span<const uint8_t> page);

// Where a board setup came from, as far as the device is concerned.
enum class BoardSetupSource {
  // A valid record was read off the device.
  kDeclared,
  // The device keeps a record, but nothing has been declared on it yet — the
  // defaults are in force.
  kBlank,
  // The device keeps a record, but what it holds cannot be used.
  kDamaged,
  kNewerLayout,
  // The device's firmware predates the record and refuses the request. The
  // defaults are in force and nothing can be stored.
  kUnsupported,
  // No device could be opened.
  kUnavailable,
};

struct BoardSetupReading {
  BoardSetupSource source = BoardSetupSource::kUnavailable;
  BoardSetup setup;
};

// Read the record off the device at `path`.
BoardSetupReading ReadBoardSetup(IUsbDevice& usb, const std::string& path);

enum class BoardSetupWriteResult {
  // Written and read back identical.
  kWritten,
  // The firmware refused both the write and the read that would confirm it:
  // it predates the record.
  kUnsupported,
  // The device answered, but what it holds afterwards is not what was sent.
  kNotConfirmed,
  // No device could be opened.
  kUnavailable,
};

// Write `setup` to the device at `path` and read it back.
BoardSetupWriteResult WriteBoardSetup(IUsbDevice& usb, const std::string& path,
                                      const BoardSetup& setup);

}  // namespace ddd::capture
