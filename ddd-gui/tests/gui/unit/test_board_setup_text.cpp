/************************************************************************

    test_board_setup_text.cpp

    T1 tests for what the Board setup tab and the capture panel say about
    the board setup
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <gtest/gtest.h>

#include <QString>

#include "board_setup.h"
#include "board_setup_page.h"

namespace ddd::gui {
namespace {

capture::BoardSetupReading Reading(capture::BoardSetupSource source,
                                   const capture::BoardSetup& setup = {}) {
  capture::BoardSetupReading reading;
  reading.source = source;
  reading.setup = setup;
  return reading;
}

// Every source has something to say, and the one that means "nothing can be
// stored" says what to do about it.
TEST(BoardSetupTextTest, EverySourceIsDescribed) {
  for (const capture::BoardSetupSource source :
       {capture::BoardSetupSource::kDeclared, capture::BoardSetupSource::kBlank,
        capture::BoardSetupSource::kDamaged,
        capture::BoardSetupSource::kNewerLayout,
        capture::BoardSetupSource::kUnsupported,
        capture::BoardSetupSource::kUnavailable}) {
    EXPECT_FALSE(DescribeBoardSetupSource(source).isEmpty());
  }
  EXPECT_TRUE(DescribeBoardSetupSource(capture::BoardSetupSource::kUnsupported)
                  .contains(QStringLiteral("firmware")));
}

TEST(BoardSetupTextTest, AnOffsetTypedByHandSaysSo) {
  EXPECT_EQ(DescribeMeasuredAt(0), QStringLiteral("not measured"));
  EXPECT_TRUE(
      DescribeMeasuredAt(1790000000U).startsWith(QStringLiteral("measured ")));
}

TEST(BoardSetupTextTest, NoDeviceNoSummary) {
  EXPECT_TRUE(DescribeBoardSummary(
                  Reading(capture::BoardSetupSource::kUnavailable), 40, true)
                  .isEmpty());
}

capture::BoardSetup BenchBoard() {
  capture::BoardSetup setup;
  setup.name = "Bench #2";
  setup.adc = capture::AdcPart::kAds828;
  setup.rsel_wiring = capture::RselWiring::kAuto;
  setup.dc_offset_1vpp = {40, 52, 63, 71, 80, 92, 101, 111};
  setup.dc_offset_2vpp = {-6, 4, 9, 14, 19, 24, 29, 34};
  return setup;
}

// The summary names the converter and the rate it allows, because that is
// what the rate list beside it stops at.
TEST(BoardSetupTextTest, TheSummaryNamesTheConverterAndItsRate) {
  const QString summary = DescribeBoardSummary(
      Reading(capture::BoardSetupSource::kDeclared, BenchBoard()), 75, true);
  EXPECT_TRUE(summary.startsWith(QStringLiteral("Bench #2")));
  EXPECT_TRUE(summary.contains(QStringLiteral("ADS828")));
  EXPECT_TRUE(summary.contains(QStringLiteral("75 MHz max")));
  EXPECT_FALSE(summary.contains(QStringLiteral("defaults")));
}

// Of the sixteen offsets, the summary shows the one in force: the capture's
// rate and the range it runs at.
TEST(BoardSetupTextTest, TheSummaryShowsTheOffsetInForce) {
  const capture::BoardSetupReading reading =
      Reading(capture::BoardSetupSource::kDeclared, BenchBoard());

  const QString fast = DescribeBoardSummary(reading, 75, true);
  EXPECT_TRUE(fast.contains(QStringLiteral("+34 at 75 MSPS, 2Vpp")));

  const QString slow = DescribeBoardSummary(reading, 40, true);
  EXPECT_TRUE(slow.contains(QStringLiteral("-6 at 40 MSPS, 2Vpp")));

  const QString narrow = DescribeBoardSummary(reading, 75, false);
  EXPECT_TRUE(narrow.contains(QStringLiteral("+111 at 75 MSPS, 1Vpp")));
}

TEST(BoardSetupTextTest, TheDefaultsAreSaidToBeTheDefaults) {
  const QString summary = DescribeBoardSummary(
      Reading(capture::BoardSetupSource::kBlank), 40, true);
  EXPECT_TRUE(summary.contains(QStringLiteral("ADS825")));
  EXPECT_TRUE(summary.contains(QStringLiteral("40 MHz max")));
  EXPECT_TRUE(summary.contains(QStringLiteral("defaults")));
}

TEST(BoardSetupTextTest, ASessionOnlyDeclarationIsSaidToBeOne) {
  capture::BoardSetup setup;
  setup.adc = capture::AdcPart::kAds828;
  const QString summary = DescribeBoardSummary(
      Reading(capture::BoardSetupSource::kUnsupported, setup), 40, true);
  EXPECT_TRUE(summary.contains(QStringLiteral("this session only")));
}

}  // namespace
}  // namespace ddd::gui
