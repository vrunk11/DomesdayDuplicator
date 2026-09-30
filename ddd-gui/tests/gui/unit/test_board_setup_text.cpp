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
  EXPECT_TRUE(
      DescribeBoardSummary(Reading(capture::BoardSetupSource::kUnavailable))
          .isEmpty());
}

// The summary names the converter and the rate it allows, because that is
// what the rate list beside it stops at.
TEST(BoardSetupTextTest, TheSummaryNamesTheConverterAndItsRate) {
  capture::BoardSetup setup;
  setup.name = "Bench #2";
  setup.adc = capture::AdcPart::kAds828;
  setup.rsel_wiring = capture::RselWiring::kAuto;
  setup.dc_offset_1vpp = -6;
  setup.dc_offset_2vpp = 3;

  const QString summary = DescribeBoardSummary(
      Reading(capture::BoardSetupSource::kDeclared, setup));
  EXPECT_TRUE(summary.startsWith(QStringLiteral("Bench #2")));
  EXPECT_TRUE(summary.contains(QStringLiteral("ADS828")));
  EXPECT_TRUE(summary.contains(QStringLiteral("75 MHz max")));
  EXPECT_TRUE(summary.contains(QStringLiteral("-6")));
  EXPECT_TRUE(summary.contains(QStringLiteral("+3")));
  EXPECT_FALSE(summary.contains(QStringLiteral("defaults")));
}

// A wired range has one offset in force, and only that one is shown.
TEST(BoardSetupTextTest, AWiredRangeShowsOnlyItsOwnOffset) {
  capture::BoardSetup setup;
  setup.rsel_wiring = capture::RselWiring::kHigh;
  setup.dc_offset_1vpp = -6;
  setup.dc_offset_2vpp = 3;

  const QString summary = DescribeBoardSummary(
      Reading(capture::BoardSetupSource::kDeclared, setup));
  EXPECT_TRUE(summary.contains(QStringLiteral("+3")));
  EXPECT_FALSE(summary.contains(QStringLiteral("-6")));
}

TEST(BoardSetupTextTest, TheDefaultsAreSaidToBeTheDefaults) {
  const QString summary =
      DescribeBoardSummary(Reading(capture::BoardSetupSource::kBlank));
  EXPECT_TRUE(summary.contains(QStringLiteral("ADS825")));
  EXPECT_TRUE(summary.contains(QStringLiteral("40 MHz max")));
  EXPECT_TRUE(summary.contains(QStringLiteral("defaults")));
}

TEST(BoardSetupTextTest, ASessionOnlyDeclarationIsSaidToBeOne) {
  capture::BoardSetup setup;
  setup.adc = capture::AdcPart::kAds828;
  const QString summary = DescribeBoardSummary(
      Reading(capture::BoardSetupSource::kUnsupported, setup));
  EXPECT_TRUE(summary.contains(QStringLiteral("this session only")));
}

}  // namespace
}  // namespace ddd::gui
