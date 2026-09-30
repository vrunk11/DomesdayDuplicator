/************************************************************************

    test_board_setup.cpp

    T1 tests for the board setup declaration and the record the device
    keeps it in
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

#include "board_setup.h"
#include "fake_usb_device.h"
#include "wire_protocol.h"

namespace ddd::capture {
namespace {

// A setup with every field away from its default, so a field that is lost or
// swapped on the way through the record shows.
BoardSetup BenchBoard() {
  BoardSetup setup;
  setup.name = "Bench #2";
  setup.adc = AdcPart::kAds828;
  setup.rsel_wiring = RselWiring::kAuto;
  setup.dc_offset_1vpp = -12;
  setup.dc_offset_2vpp = 5;
  setup.measured_1vpp = 1790000000U;
  setup.measured_2vpp = 1790000060U;
  return setup;
}

// BenchBoard(), encoded. The layout is a wire format — the firmware checks its
// framing and a later build of this application has to read it — so it is
// pinned byte for byte rather than only round-tripped. The same 64 bytes are in
// fx3/firmware/tests/board-setup-test.c, where the firmware is checked to
// accept them.
constexpr BoardSetupRecord kBenchBoardRecord = {
    0x44, 0x44, 0x42, 0x53, 0x01, 0x00, 0x01, 0x00,  //
    0xF4, 0xFF, 0x05, 0x00, 0x80, 0x3B, 0xB1, 0x6A,  //
    0xBC, 0x3B, 0xB1, 0x6A, 0x42, 0x65, 0x6E, 0x63,  //
    0x68, 0x20, 0x23, 0x32, 0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00, 0xAF, 0xE9, 0x77, 0xE5,  //
};

// Recompute the checksum after a test has changed a byte on purpose, so that
// what is being tested is the byte and not the checksum.
BoardSetupRecord Sealed(BoardSetupRecord record) {
  const uint32_t crc = BoardSetupCrc32(
      std::span<const uint8_t>(record.data(), record.size() - 4));
  for (size_t index = 0; index < 4; ++index) {
    record[record.size() - 4 + index] =
        static_cast<uint8_t>((crc >> (8 * index)) & 0xFFU);
  }
  return record;
}

// --- The declaration --------------------------------------------------------

// What a board is assumed to be when nothing has been declared: the slower
// converter, RSEL tied high, no offset. Every board ever built can run at
// 40 MHz, and a board with RSEL unrouted captures at 2Vpp.
TEST(BoardSetupTest, TheDefaultsAreTheConservativeBoard) {
  const BoardSetup setup;
  EXPECT_EQ(setup.adc, AdcPart::kAds825);
  EXPECT_EQ(setup.rsel_wiring, RselWiring::kHigh);
  EXPECT_EQ(setup.dc_offset_1vpp, 0);
  EXPECT_EQ(setup.dc_offset_2vpp, 0);
  EXPECT_TRUE(setup.name.empty());
}

TEST(BoardSetupTest, EachConverterIsNamedAndRatedAsItsDatasheetSays) {
  EXPECT_EQ(std::string(AdcPartName(AdcPart::kAds825)), "ADS825");
  EXPECT_EQ(std::string(AdcPartName(AdcPart::kAds828)), "ADS828");
  EXPECT_EQ(AdcPartMaxRateMhz(AdcPart::kAds825), 40);
  EXPECT_EQ(AdcPartMaxRateMhz(AdcPart::kAds828), 75);
}

TEST(BoardSetupTest, TheWiringIsSpelledAsTheMetadataRecordsIt) {
  EXPECT_EQ(std::string(RselWiringName(RselWiring::kAuto)), "auto");
  EXPECT_EQ(std::string(RselWiringName(RselWiring::kLow)), "low");
  EXPECT_EQ(std::string(RselWiringName(RselWiring::kHigh)), "high");
}

// Only a board whose RSEL reaches the FPGA lets a capture choose the range;
// on any other the pin is soldered to one level and asking for the other one
// changes nothing but the label.
TEST(BoardSetupTest, TheRangeFollowsTheRequestOnlyWhenRselIsRouted) {
  BoardSetup setup;

  setup.rsel_wiring = RselWiring::kAuto;
  EXPECT_TRUE(InputRangeIsSelectable(setup));
  EXPECT_TRUE(EffectiveRange2Vpp(setup, true));
  EXPECT_FALSE(EffectiveRange2Vpp(setup, false));

  setup.rsel_wiring = RselWiring::kLow;
  EXPECT_FALSE(InputRangeIsSelectable(setup));
  EXPECT_FALSE(EffectiveRange2Vpp(setup, true));
  EXPECT_FALSE(EffectiveRange2Vpp(setup, false));

  setup.rsel_wiring = RselWiring::kHigh;
  EXPECT_FALSE(InputRangeIsSelectable(setup));
  EXPECT_TRUE(EffectiveRange2Vpp(setup, true));
  EXPECT_TRUE(EffectiveRange2Vpp(setup, false));
}

// RSEL exists on both converters, so the wiring does not depend on the part.
TEST(BoardSetupTest, TheWiringIsIndependentOfTheConverter) {
  BoardSetup setup;
  setup.adc = AdcPart::kAds825;
  setup.rsel_wiring = RselWiring::kAuto;
  EXPECT_TRUE(InputRangeIsSelectable(setup));
  EXPECT_FALSE(EffectiveRange2Vpp(setup, false));
}

TEST(BoardSetupTest, EachRangeHasItsOwnOffset) {
  const BoardSetup setup = BenchBoard();
  EXPECT_EQ(DcOffsetFor(setup, false), -12);
  EXPECT_EQ(DcOffsetFor(setup, true), 5);
}

// --- The board name ---------------------------------------------------------

TEST(BoardSetupTest, AShortNameIsKeptAsItIs) {
  EXPECT_EQ(TruncateBoardName("Bench #2"), "Bench #2");
  EXPECT_EQ(TruncateBoardName(""), "");
}

TEST(BoardSetupTest, ALongNameIsCutToWhatTheRecordHolds) {
  const std::string long_name(40, 'x');
  EXPECT_EQ(TruncateBoardName(long_name).size(), kBoardNameMaximumBytes);
}

// A cut through the middle of a multi-byte character would leave the record
// holding bytes no UTF-8 reader accepts.
TEST(BoardSetupTest, ANameIsNeverCutInsideACharacter) {
  // 31 ASCII bytes and then "é" (two bytes): the second byte would be byte 33.
  const std::string name = std::string(31, 'a') + "\xC3\xA9";
  const std::string cut = TruncateBoardName(name);
  EXPECT_EQ(cut, std::string(31, 'a'));
}

TEST(BoardSetupTest, ANameEndsAtItsFirstNul) {
  EXPECT_EQ(TruncateBoardName(std::string("ab\0cd", 5)), "ab");
}

// --- The record -------------------------------------------------------------

// The published check value for this CRC-32, which is what pins it to the
// firmware's and to the FPGA boot block's.
TEST(BoardSetupTest, TheChecksumIsTheStandardCrc32) {
  const std::string check = "123456789";
  EXPECT_EQ(BoardSetupCrc32(std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(check.data()), check.size())),
            0xCBF43926U);
}

TEST(BoardSetupTest, TheRecordLayoutIsPinned) {
  EXPECT_EQ(EncodeBoardSetup(BenchBoard()), kBenchBoardRecord);
}

TEST(BoardSetupTest, ARecordReadsBackAsTheSetupItWasMadeFrom) {
  const DecodedBoardSetup decoded = DecodeBoardSetup(kBenchBoardRecord);
  EXPECT_EQ(decoded.state, BoardSetupRecordState::kValid);
  EXPECT_EQ(decoded.setup, BenchBoard());
}

TEST(BoardSetupTest, TheDefaultsRoundTripToo) {
  const BoardSetup defaults;
  const DecodedBoardSetup decoded =
      DecodeBoardSetup(EncodeBoardSetup(defaults));
  EXPECT_EQ(decoded.state, BoardSetupRecordState::kValid);
  EXPECT_EQ(decoded.setup, defaults);
}

TEST(BoardSetupTest, TheOffsetsAtTheEndsOfTheirRangeRoundTrip) {
  BoardSetup setup;
  setup.dc_offset_1vpp = static_cast<int16_t>(kDcOffsetMinimum);
  setup.dc_offset_2vpp = static_cast<int16_t>(kDcOffsetMaximum);
  EXPECT_EQ(DecodeBoardSetup(EncodeBoardSetup(setup)).setup, setup);
}

// The record cannot hold more than a converter's worth of offset; the encoder
// clamps rather than writing a figure the decoder would call damaged.
TEST(BoardSetupTest, AnOffsetBeyondTheConverterIsClampedWhenEncoded) {
  BoardSetup setup;
  setup.dc_offset_1vpp = 2000;
  setup.dc_offset_2vpp = -2000;
  const DecodedBoardSetup decoded = DecodeBoardSetup(EncodeBoardSetup(setup));
  EXPECT_EQ(decoded.state, BoardSetupRecordState::kValid);
  EXPECT_EQ(decoded.setup.dc_offset_1vpp, kDcOffsetMaximum);
  EXPECT_EQ(decoded.setup.dc_offset_2vpp, kDcOffsetMinimum);
}

// What an EEPROM nothing has declared on reads as. Not a damaged record — no
// record at all — and the defaults are what apply.
TEST(BoardSetupTest, AnErasedPageIsBlank) {
  BoardSetupRecord page{};
  page.fill(0xFF);
  const DecodedBoardSetup decoded = DecodeBoardSetup(page);
  EXPECT_EQ(decoded.state, BoardSetupRecordState::kBlank);
  EXPECT_EQ(decoded.setup, BoardSetup{});

  page.fill(0x00);
  EXPECT_EQ(DecodeBoardSetup(page).state, BoardSetupRecordState::kBlank);
}

TEST(BoardSetupTest, APageOfTheWrongLengthIsBlank) {
  const std::array<uint8_t, 4> short_page = {'D', 'D', 'B', 'S'};
  EXPECT_EQ(DecodeBoardSetup(short_page).state, BoardSetupRecordState::kBlank);
}

TEST(BoardSetupTest, AFlippedBitAnywhereIsDamage) {
  for (size_t index = 4; index < kBoardSetupRecordLength; ++index) {
    BoardSetupRecord page = kBenchBoardRecord;
    page[index] ^= 0x01;
    const DecodedBoardSetup decoded = DecodeBoardSetup(page);
    EXPECT_EQ(decoded.state, BoardSetupRecordState::kDamaged) << index;
    EXPECT_EQ(decoded.setup, BoardSetup{}) << index;
  }
}

TEST(BoardSetupTest, LayoutVersionZeroIsDamage) {
  BoardSetupRecord page = kBenchBoardRecord;
  page[4] = 0;
  EXPECT_EQ(DecodeBoardSetup(Sealed(page)).state,
            BoardSetupRecordState::kDamaged);
}

// A newer application may have written fields this one does not know. It is a
// record, so it is not blank, and it is not read as if it were layout 1.
TEST(BoardSetupTest, ALaterLayoutIsRecognisedAndNotInterpreted) {
  BoardSetupRecord page = kBenchBoardRecord;
  page[4] = 2;
  const DecodedBoardSetup decoded = DecodeBoardSetup(Sealed(page));
  EXPECT_EQ(decoded.state, BoardSetupRecordState::kNewerLayout);
  EXPECT_EQ(decoded.setup, BoardSetup{});
}

TEST(BoardSetupTest, AConverterOrWiringThisBuildDoesNotKnowIsDamage) {
  BoardSetupRecord page = kBenchBoardRecord;
  page[6] = 2;
  EXPECT_EQ(DecodeBoardSetup(Sealed(page)).state,
            BoardSetupRecordState::kDamaged);

  page = kBenchBoardRecord;
  page[7] = 3;
  EXPECT_EQ(DecodeBoardSetup(Sealed(page)).state,
            BoardSetupRecordState::kDamaged);
}

TEST(BoardSetupTest, AnOffsetBeyondTheConverterInARecordIsDamage) {
  BoardSetupRecord page = kBenchBoardRecord;
  page[8] = 0x00;  // 512, one past the largest
  page[9] = 0x02;
  EXPECT_EQ(DecodeBoardSetup(Sealed(page)).state,
            BoardSetupRecordState::kDamaged);
}

// --- Reading and writing it on a device
// ---------------------------------------

class BoardSetupDeviceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    usb_.SetSingleDevice("device", DeviceSpeed::kSuper, "Domesday Duplicator");
  }

  FakeUsbDevice usb_;
};

TEST_F(BoardSetupDeviceTest, AMissingDeviceIsUnavailable) {
  EXPECT_EQ(ReadBoardSetup(usb_, "elsewhere").source,
            BoardSetupSource::kUnavailable);
  EXPECT_EQ(WriteBoardSetup(usb_, "elsewhere", BenchBoard()),
            BoardSetupWriteResult::kUnavailable);
}

// Firmware older than the record stalls both requests. The defaults apply and
// nothing can be stored — which is a state to say out loud, not a failure.
TEST_F(BoardSetupDeviceTest, FirmwareWithoutTheRecordIsUnsupported) {
  const BoardSetupReading reading = ReadBoardSetup(usb_, "device");
  EXPECT_EQ(reading.source, BoardSetupSource::kUnsupported);
  EXPECT_EQ(reading.setup, BoardSetup{});

  EXPECT_EQ(WriteBoardSetup(usb_, "device", BenchBoard()),
            BoardSetupWriteResult::kUnsupported);
}

TEST_F(BoardSetupDeviceTest, ADeviceNothingWasDeclaredOnIsBlank) {
  usb_.SetBoardSetupSupported(true);
  const BoardSetupReading reading = ReadBoardSetup(usb_, "device");
  EXPECT_EQ(reading.source, BoardSetupSource::kBlank);
  EXPECT_EQ(reading.setup, BoardSetup{});
}

TEST_F(BoardSetupDeviceTest, ADeclarationIsReadOffTheDevice) {
  usb_.SetBoardSetupSupported(true);
  usb_.SetBoardSetupPage(kBenchBoardRecord);
  const BoardSetupReading reading = ReadBoardSetup(usb_, "device");
  EXPECT_EQ(reading.source, BoardSetupSource::kDeclared);
  EXPECT_EQ(reading.setup, BenchBoard());
}

TEST_F(BoardSetupDeviceTest, ADamagedRecordIsReportedAsSuch) {
  usb_.SetBoardSetupSupported(true);
  BoardSetupRecord page = kBenchBoardRecord;
  page[20] ^= 0x01;
  usb_.SetBoardSetupPage(page);
  EXPECT_EQ(ReadBoardSetup(usb_, "device").source, BoardSetupSource::kDamaged);
}

TEST_F(BoardSetupDeviceTest, AWrittenDeclarationIsOnTheDeviceAndReadsBack) {
  usb_.SetBoardSetupSupported(true);
  EXPECT_EQ(WriteBoardSetup(usb_, "device", BenchBoard()),
            BoardSetupWriteResult::kWritten);
  EXPECT_EQ(usb_.board_setup_page(), kBenchBoardRecord);
  EXPECT_EQ(ReadBoardSetup(usb_, "device").setup, BenchBoard());
}

// The firmware acknowledges a data stage it has already taken whether or not
// the EEPROM kept it, so the readback is the only confirmation there is.
TEST_F(BoardSetupDeviceTest, AWriteTheMediumDidNotKeepIsNotConfirmed) {
  usb_.SetBoardSetupSupported(true);
  usb_.SetBoardSetupDropsWrites(true);
  EXPECT_EQ(WriteBoardSetup(usb_, "device", BenchBoard()),
            BoardSetupWriteResult::kNotConfirmed);
  EXPECT_EQ(usb_.board_setup_write_count(), 1U);
}

}  // namespace
}  // namespace ddd::capture
