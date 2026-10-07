/************************************************************************

    test_capture_metadata.cpp

    T1 tests for the YAML sidecar written beside every capture
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "capture_metadata.h"
#include "utc_time_zone.h"

namespace ddd::capture {
namespace {

// 2026-08-13 12:34:56 UTC, the same fixed point the naming tests use.
constexpr std::time_t kFixedTime = 1'786'624'496;

class CaptureMetadataTest : public ::testing::Test {
 protected:
  void SetUp() override { ddd::capture::test::UseUtc(); }

  // A capture that ran and finished, with nothing said about the disc and no
  // player attached — which is what almost every capture is.
  static CaptureMetadata Ordinary() {
    CaptureMetadata metadata;
    metadata.capture_file_name = "RF-Sample_2026-08-13_12-34-56.ddd.flac";
    metadata.application_version = "1.2.3";
    metadata.format = "FLAC";
    metadata.sample_rate_hz = 40'000'000;
    metadata.decimation_factor = 1;
    metadata.started = kFixedTime;
    metadata.finished = kFixedTime + 60;
    metadata.outcome.duration_seconds = 60.0;
    metadata.outcome.samples = 2'400'000'000;
    metadata.outcome.bytes = 1'234'567'890;
    metadata.outcome.sequence_check = "running";
    return metadata;
  }

  static bool Contains(const std::string& document, const std::string& line) {
    return document.find(line) != std::string::npos;
  }
};

TEST_F(CaptureMetadataTest, TheSidecarSitsBesideTheCaptureItDescribes) {
  EXPECT_EQ(CaptureMetadataPath("/captures/Casper_side1.ddd.flac"),
            std::filesystem::path("/captures/Casper_side1.ddd.yaml"));

  // The uncompressed format has no tags of its own, so this file is the whole
  // of its provenance — and it is named on the same rule.
  EXPECT_EQ(CaptureMetadataPath("/captures/Casper_side1.ddd.s16"),
            std::filesystem::path("/captures/Casper_side1.ddd.yaml"));
}

TEST_F(CaptureMetadataTest, APathWithNoCaptureSuffixGainsRatherThanLosesOne) {
  // Nothing in this application produces such a path, but appending cannot
  // destroy the association between the two files where replacing an unknown
  // extension could.
  EXPECT_EQ(CaptureMetadataPath("/captures/oddity.bin"),
            std::filesystem::path("/captures/oddity.bin.ddd.yaml"));
}

TEST_F(CaptureMetadataTest, AnOrdinaryCaptureRecordsWhatItWas) {
  const std::string document = BuildCaptureMetadataYaml(Ordinary());

  EXPECT_TRUE(Contains(document, "\"schema_version\": 1"));
  EXPECT_TRUE(Contains(document, "\"application_version\": \"1.2.3\""));
  EXPECT_TRUE(Contains(document,
                       "\"file\": \"RF-Sample_2026-08-13_12-34-56.ddd.flac\""));
  EXPECT_TRUE(Contains(document, "\"format\": \"FLAC\""));
  EXPECT_TRUE(Contains(document, "\"sample_rate_hz\": 40000000"));
  EXPECT_TRUE(Contains(document, "\"duration_seconds\": 60.000"));
  EXPECT_TRUE(Contains(document, "\"samples\": 2400000000"));
  EXPECT_TRUE(Contains(document, "\"completed\": true"));
  EXPECT_TRUE(Contains(document, "\"sequence_check\": \"running\""));

  // Local time with the offset on the end, so the timestamp is a moment rather
  // than a reading on a clock. The tests run in UTC.
  //
  // Windows is the exception, and by decision rather than by accident: its
  // struct tm carries no offset, and the writer declines to reconstruct one
  // from the timezone globals, so the timestamp there is the local time alone.
  // Asserting the offset unconditionally is what failed every MSI build.
#ifdef _WIN32
  EXPECT_TRUE(Contains(document, "\"started\": \"2026-08-13T12:34:56\""))
      << document;
#else
  EXPECT_TRUE(Contains(document, "\"started\": \"2026-08-13T12:34:56+00:00\""))
      << document;
#endif
}

TEST_F(CaptureMetadataTest, TheTestModeFlagIsAlwaysWritten) {
  // Never left out, whichever way round it is. A capture of ramps and a capture
  // of signal are indistinguishable by inspection until somebody decodes one,
  // so this is the one boolean that is worth stating even when it is false.
  EXPECT_TRUE(
      Contains(BuildCaptureMetadataYaml(Ordinary()), "\"test_mode\": false"));

  CaptureMetadata test_capture = Ordinary();
  test_capture.test_mode = true;
  EXPECT_TRUE(
      Contains(BuildCaptureMetadataYaml(test_capture), "\"test_mode\": true"));
}

TEST_F(CaptureMetadataTest, NothingEstablishedIsNothingWritten) {
  const std::string document = BuildCaptureMetadataYaml(Ordinary());

  // The gain in particular: a capture carrying a default figure nobody had
  // checked would read as calibration data, which is worse than saying nothing.
  EXPECT_FALSE(Contains(document, "front_end_gain"));
  EXPECT_FALSE(Contains(document, "\"title\""));
  EXPECT_FALSE(Contains(document, "model_name"));

  // But the sections themselves are there and empty, so a reader can tell this
  // document from one written before those sections existed.
  EXPECT_TRUE(Contains(document, "\"naming\": {}"));
  EXPECT_TRUE(Contains(document, "\"device\": {}"));
  EXPECT_TRUE(Contains(document, "\"player\": {}"));
  EXPECT_TRUE(Contains(document, "\"disc\": {}"));
}

TEST_F(CaptureMetadataTest, WhatTheUserSaidTheDiscWasIsRecorded) {
  CaptureMetadata metadata = Ordinary();
  metadata.naming.title_used = true;
  metadata.naming.title = "Casper";
  metadata.naming.disc_type_used = true;
  metadata.naming.disc_type = DiscTypeChoice::kClv;
  metadata.naming.video_standard_used = true;
  metadata.naming.video_standard = VideoStandardChoice::kPal;
  metadata.naming.audio_used = true;
  metadata.naming.audio = AudioTypeChoice::kAnalogue;
  metadata.naming.side_used = true;
  metadata.naming.side = 2;
  metadata.naming.notes_used = true;
  metadata.naming.notes = "second pressing";
  metadata.naming.mint_marks_used = true;
  metadata.naming.mint_marks = "NM";
  metadata.naming.metadata_notes = "Rot on the outer edge.";

  const std::string document = BuildCaptureMetadataYaml(metadata);

  EXPECT_TRUE(Contains(document, "\"title\": \"Casper\""));
  EXPECT_TRUE(Contains(document, "\"disc_type\": \"CLV\""));
  EXPECT_TRUE(Contains(document, "\"video_standard\": \"PAL\""));

  // Spelled out in the metadata where the file name gets "ANA". A file name is
  // short because it is a file name; a metadata field is read by somebody who
  // was not there.
  EXPECT_TRUE(Contains(document, "\"audio\": \"Analogue\""));
  EXPECT_TRUE(Contains(document, "\"side\": 2"));
  EXPECT_TRUE(Contains(document, "\"notes\": \"second pressing\""));
  EXPECT_TRUE(Contains(document, "\"mint_marks\": \"NM\""));
  EXPECT_TRUE(
      Contains(document, "\"metadata_notes\": \"Rot on the outer edge.\""));
}

TEST_F(CaptureMetadataTest, AFieldNobodyWasAskedAboutStaysOut) {
  // The flag decides, not the value. A title typed and then unticked is a title
  // nobody is claiming, and the sidecar must not claim it either.
  CaptureMetadata metadata = Ordinary();
  metadata.naming.title = "Casper";
  metadata.naming.side = 2;

  const std::string document = BuildCaptureMetadataYaml(metadata);
  EXPECT_FALSE(Contains(document, "Casper"));
  EXPECT_FALSE(Contains(document, "\"side\""));
}

TEST_F(CaptureMetadataTest, ThePlayerIsRecordedWhereThereWasOne) {
  CaptureMetadata metadata = Ordinary();
  metadata.player.model_name = "Pioneer LD-V4300D";
  metadata.player.model_id_code = "P15";
  metadata.player.model_code = "P1512";
  metadata.player.firmware_version = "12";
  metadata.player.port = "/dev/ttyUSB0";
  metadata.player.baud_rate = 9600;
  metadata.player.recognised_model = true;

  const std::string document = BuildCaptureMetadataYaml(metadata);

  EXPECT_TRUE(Contains(document, "\"model_name\": \"Pioneer LD-V4300D\""));
  EXPECT_TRUE(Contains(document, "\"model_id_code\": \"P15\""));
  EXPECT_TRUE(Contains(document, "\"model_code\": \"P1512\""));
  EXPECT_TRUE(Contains(document, "\"firmware_version\": \"12\""));
  EXPECT_TRUE(Contains(document, "\"port\": \"/dev/ttyUSB0\""));
  EXPECT_TRUE(Contains(document, "\"baud_rate\": 9600"));
  EXPECT_TRUE(Contains(document, "\"recognised_model\": true"));
}

TEST_F(CaptureMetadataTest, TheDevicesOwnBuildIsRecorded) {
  CaptureMetadata metadata = Ordinary();
  metadata.device.firmware_version = "a1b2c3d4";
  metadata.device.gateware_version = "a1b2c3d4";
  metadata.device.gateware_register_map = 2;

  const std::string document = BuildCaptureMetadataYaml(metadata);

  EXPECT_TRUE(Contains(document, "\"device\":"));
  EXPECT_TRUE(Contains(document, "\"firmware_version\": \"a1b2c3d4\""));
  EXPECT_TRUE(Contains(document, "\"gateware_version\": \"a1b2c3d4\""));
  EXPECT_TRUE(Contains(document, "\"gateware_register_map\": 2"));
}

// The three versions in a release are one commit, so what this section is for
// is the case where they are not: a device that was never updated, or a
// gateware built by hand on the bench.
TEST_F(CaptureMetadataTest, ThreeVersionsThatDisagreeAreAllRecorded) {
  CaptureMetadata metadata = Ordinary();
  metadata.application_version = "1.2.3-aaaaaaa";
  metadata.device.firmware_version = "bbbbbbbb";
  metadata.device.gateware_version = "cccccccc";

  const std::string document = BuildCaptureMetadataYaml(metadata);

  EXPECT_TRUE(Contains(document, "\"application_version\": \"1.2.3-aaaaaaa\""));
  EXPECT_TRUE(Contains(document, "\"firmware_version\": \"bbbbbbbb\""));
  EXPECT_TRUE(Contains(document, "\"gateware_version\": \"cccccccc\""));
}

// A commit hash on its own asserts that a published build produced this file.
// For a tree with uncommitted changes that is not true, and the same "-dirty"
// convention the application's own stamp uses is what says so.
TEST_F(CaptureMetadataTest, GatewareBuiltFromAModifiedTreeSaysSo) {
  CaptureMetadata metadata = Ordinary();
  metadata.device.gateware_version = "a1b2c3d4-dirty";

  EXPECT_TRUE(Contains(BuildCaptureMetadataYaml(metadata),
                       "\"gateware_version\": \"a1b2c3d4-dirty\""));
}

// Firmware too old to stamp its commit, gateware that had not finished
// configuring, a device nothing asked — all of them land here, and none of
// them is an error worth writing a field about.
TEST_F(CaptureMetadataTest, ADeviceThatSaidNothingWritesNothing) {
  const std::string document = BuildCaptureMetadataYaml(Ordinary());

  EXPECT_FALSE(Contains(document, "gateware_version"));
  EXPECT_FALSE(Contains(document, "gateware_register_map"));

  // Zero is not a register map, and writing it would be a reading rather than
  // the absence of one.
  CaptureMetadata metadata = Ordinary();
  metadata.device.firmware_version = "a1b2c3d4";
  metadata.device.gateware_register_map = 0;
  EXPECT_FALSE(
      Contains(BuildCaptureMetadataYaml(metadata), "gateware_register_map"));
}

// The board is a declaration, so it has a block of its own rather than keys
// among the capture's settings — and the offset that was taken out of the
// samples is in it, so they can always be put back.
TEST_F(CaptureMetadataTest, TheBoardDeclarationIsABlockOfItsOwn) {
  CaptureMetadata metadata = Ordinary();
  metadata.board.known = true;
  metadata.board.declared = true;
  metadata.board.name = "Bench #2";
  metadata.board.adc = "ADS828";
  metadata.board.rsel_wiring = "auto";
  metadata.board.dc_offset = -12;
  metadata.board.offset_saturated_samples = 0;

  const std::string document = BuildCaptureMetadataYaml(metadata);

  EXPECT_TRUE(Contains(document, "\"board\":"));
  EXPECT_TRUE(Contains(document, "\"setup\": \"declared\""));
  EXPECT_TRUE(Contains(document, "\"name\": \"Bench #2\""));
  EXPECT_TRUE(Contains(document, "\"adc\": \"ADS828\""));
  EXPECT_TRUE(Contains(document, "\"rsel_wiring\": \"auto\""));
  EXPECT_TRUE(Contains(document, "\"dc_offset\": -12"));
  EXPECT_TRUE(Contains(document, "\"offset_saturated_samples\": 0"));
}

// A board nothing was declared on is described as running on the defaults, not
// as a board somebody said was an ADS825.
TEST_F(CaptureMetadataTest, AnUndeclaredBoardSaysItRanOnTheDefaults) {
  CaptureMetadata metadata = Ordinary();
  metadata.board.known = true;
  metadata.board.declared = false;
  metadata.board.adc = "ADS825";
  metadata.board.rsel_wiring = "high";

  const std::string document = BuildCaptureMetadataYaml(metadata);

  EXPECT_TRUE(Contains(document, "\"setup\": \"default\""));
  EXPECT_FALSE(Contains(document, "\"name\":"));
}

// The count that says a declaration belongs to another board is written as it
// was measured over this file.
TEST_F(CaptureMetadataTest, SamplesTheCorrectionPushedOutOfRangeAreRecorded) {
  CaptureMetadata metadata = Ordinary();
  metadata.board.known = true;
  metadata.board.dc_offset = 40;
  metadata.board.offset_saturated_samples = 1234;

  EXPECT_TRUE(Contains(BuildCaptureMetadataYaml(metadata),
                       "\"offset_saturated_samples\": 1234"));
}

// Every change made to the signal is in the sidecar beside the DC offset: the
// bit shift as the action taken and written at zero too, the requantisation
// written as off when there was none, and the samples the shift clipped
// counted with the rest of the signal's figures.
TEST_F(CaptureMetadataTest, TheBitShiftAndTheRequantisationAreRecorded) {
  CaptureMetadata metadata = Ordinary();
  const std::string untouched = BuildCaptureMetadataYaml(metadata);
  EXPECT_TRUE(Contains(untouched, "\"bit_shift\": 0"));
  EXPECT_TRUE(Contains(untouched, "\"requantization\":\n  \"mode\": \"off\""))
      << untouched;

  metadata.bit_shift = 1;
  metadata.signal.known = true;
  metadata.signal.shift_clipped_samples = 77;
  const std::string shifted = BuildCaptureMetadataYaml(metadata);
  EXPECT_TRUE(Contains(shifted, "\"bit_shift\": 1"));
  EXPECT_TRUE(Contains(shifted, "\"shift_clipped_samples\": 77"));
}

// A requantised capture says what it was asked for and what was done to which
// samples: the drop each run of samples got, keyed by its first sample, and
// how many samples got each.
TEST_F(CaptureMetadataTest, ARequantisedCaptureRecordsEveryChange) {
  CaptureMetadata metadata = Ordinary();
  RequantizationRecord& record = metadata.requantization;
  record.enabled = true;
  record.margin_level = 2;
  record.protected_bands = {{0.0, 13.5}};
  record.input_bits = 10;
  record.shaping_order = 16;
  record.shaping_depth_db = 10.0;
  record.samples_by_drop = {0, 2'097'152, 0, 1'048'576};
  record.shaped_samples = 1'048'576;
  record.worst_degradation_db = 0.1875;
  record.clipped_samples = 3;
  record.changes = {{0, 1, false}, {2'097'152, 3, true}};

  const std::string document = BuildCaptureMetadataYaml(metadata);
  EXPECT_TRUE(Contains(document, "\"mode\": \"dynamic\"")) << document;
  EXPECT_TRUE(Contains(document, "\"margin_level\": 2"));
  EXPECT_TRUE(Contains(document, "\"margin\": \"safe\""));
  EXPECT_TRUE(Contains(document, "\"limit_db\": 0.20"));
  EXPECT_TRUE(Contains(document, "\"protected_bands\": \"0-13.5 MHz\""));
  EXPECT_TRUE(Contains(document, "\"input_bits\": 10"));
  EXPECT_TRUE(Contains(document, "\"shaping\": \"fixed\""));
  EXPECT_TRUE(Contains(document, "\"shaped_samples\": 1048576"));
  EXPECT_TRUE(Contains(document, "\"worst_degradation_db\": 0.188"));
  EXPECT_TRUE(Contains(document, "\"clipped_samples\": 3"));
  EXPECT_TRUE(Contains(document,
                       "\"samples_by_bits_dropped\":\n"
                       "    \"1\": 2097152\n"
                       "    \"3\": 1048576\n"));
  EXPECT_TRUE(Contains(document,
                       "\"changes\":\n"
                       "    \"0\": \"1\"\n"
                       "    \"2097152\": \"3 shaped\"\n"));

  // Zones only when there are some.
  EXPECT_FALSE(Contains(document, "shaping_zones"));

  record.adaptive_shaping = true;
  EXPECT_TRUE(Contains(BuildCaptureMetadataYaml(metadata),
                       "\"shaping\": \"adaptive\""));

  record.shaping_zones = {{0.0, 2.0, 10.0}, {14.0, 17.5, 40.0}};
  EXPECT_TRUE(
      Contains(BuildCaptureMetadataYaml(metadata),
               "\"shaping_zones\": \"0-2 MHz @ 10 dB, 14-17.5 MHz @ 40 dB\""));
}

TEST_F(CaptureMetadataTest, NoBoardSetupWritesNoBoardBlock) {
  EXPECT_FALSE(Contains(BuildCaptureMetadataYaml(Ordinary()), "\"board\":"));
}

TEST_F(CaptureMetadataTest, TheScanRecordsEveryFactWithHowItWasEstablished) {
  CaptureMetadata metadata = Ordinary();
  metadata.disc.examined = true;
  metadata.disc.disc_type = ScannedFact{"CLV", "reported"};
  metadata.disc.disc_side = ScannedFact{"2", "reported"};
  metadata.disc.programme_end = ScannedFact{"1:02:03", "measured"};
  metadata.disc.video_standard = ScannedFact{"PAL", "declared"};
  metadata.disc.disc_status_reply = "11011";

  const std::string document = BuildCaptureMetadataYaml(metadata);

  // The provenance travels with the value rather than being implied by the
  // section. A length that came from seeking past the end of the side and one a
  // disc merely claims are both numbers, and a file showing them alike would
  // have to be believed rather than read.
  EXPECT_TRUE(Contains(document,
                       "    \"value\": \"1:02:03\"\n"
                       "    \"source\": \"measured\""));
  EXPECT_TRUE(Contains(document,
                       "    \"value\": \"PAL\"\n"
                       "    \"source\": \"declared\""));
  EXPECT_TRUE(Contains(document, "\"examined\": true"));

  // The working, not the answer: a sidecar that says "side 2" and shows the
  // characters it read that from is one somebody can check.
  EXPECT_TRUE(Contains(document, "\"disc_status_reply\": \"11011\""));
}

TEST_F(CaptureMetadataTest, AnExaminationThatFoundNothingIsNotNoExamination) {
  // The distinction the `examined` flag exists for: a player that refused every
  // query still produced a finding, and it is not the same finding as a capture
  // taken with no examination at all.
  CaptureMetadata metadata = Ordinary();
  metadata.disc.examined = true;

  const std::string document = BuildCaptureMetadataYaml(metadata);
  EXPECT_TRUE(Contains(document, "\"examined\": true"));
  EXPECT_FALSE(Contains(document, "\"disc\": {}"));
}

TEST_F(CaptureMetadataTest, AUserCodeKeepsItsOutcomeAndItsCharacters) {
  CaptureMetadata metadata = Ordinary();
  metadata.disc.examined = true;
  metadata.disc.standard_user_code_outcome = "not encoded on the disc";
  metadata.disc.pioneer_user_code_outcome = "read";

  // Sixty characters the player could not read, and then one that was never
  // encoded. Recording those alike would record the absence of evidence as
  // evidence of absence.
  metadata.disc.pioneer_user_code = std::string("``` ") + '\0';

  const std::string document = BuildCaptureMetadataYaml(metadata);

  EXPECT_TRUE(Contains(document, "\"outcome\": \"not encoded on the disc\""));
  EXPECT_TRUE(Contains(document, "\"text\": \"``` \\x00\""));
}

TEST_F(CaptureMetadataTest, AFailedCaptureSaysSoAndSaysWhy) {
  CaptureMetadata metadata = Ordinary();
  metadata.outcome.completed = false;
  metadata.outcome.detail = "The device was disconnected";

  const std::string document = BuildCaptureMetadataYaml(metadata);
  EXPECT_TRUE(Contains(document, "\"completed\": false"));
  EXPECT_TRUE(
      Contains(document, "\"detail\": \"The device was disconnected\""));
}

TEST_F(CaptureMetadataTest, TheSignalFiguresAreAboutThisFile) {
  CaptureMetadata metadata = Ordinary();
  metadata.signal.known = true;
  metadata.signal.minimum_value = 12;
  metadata.signal.maximum_value = 1008;
  metadata.signal.rms = 123.456;
  metadata.signal.clipped_low_samples = 4;

  const std::string document = BuildCaptureMetadataYaml(metadata);

  EXPECT_TRUE(Contains(document, "\"minimum_value\": 12"));
  EXPECT_TRUE(Contains(document, "\"rms\": 123.46"));
  EXPECT_TRUE(Contains(document, "\"clipped_low_samples\": 4"));

  // Said in the file rather than only in the source, because a number in a
  // file's metadata will be read as a number about that file whatever the
  // source says.
  EXPECT_TRUE(Contains(document, "# Measured over this file's own samples"));
}

// Metadata is data about the data. Anything measured over the monitoring
// session either side of the file describes something that was never recorded,
// so it is not in the document at all — not written with a caveat, absent.
//
// Ring depth and the device's back-pressure peak are the examples: they say how
// hard this machine was working, which is worth watching live and is not a
// property of a recording that outlives the session by years.
TEST_F(CaptureMetadataTest, NothingAboutTheSessionRatherThanTheRecording) {
  const std::string document = BuildCaptureMetadataYaml(Ordinary());

  EXPECT_FALSE(Contains(document, "peak_buffers_in_use"));
  EXPECT_FALSE(Contains(document, "buffer_count"));
  EXPECT_FALSE(Contains(document, "back_pressure"));
  EXPECT_FALSE(Contains(document, "monitoring run"));
}

// The two that stay, and why they are different from the ones above: a dropped
// word is a sample that existed on the disc and is not in this file.
TEST_F(CaptureMetadataTest, WhatTheDeviceLostWhileWritingThisFileIsRecorded) {
  CaptureMetadata metadata = Ordinary();
  metadata.outcome.device_overflow_events = 2;
  metadata.outcome.device_dropped_words = 1024;

  const std::string document = BuildCaptureMetadataYaml(metadata);
  EXPECT_TRUE(Contains(document, "\"device_overflow_events\": 2"));
  EXPECT_TRUE(Contains(document, "\"device_dropped_words\": 1024"));
}

TEST_F(CaptureMetadataTest, TheDocumentIsWrittenToDiskAsItWasBuilt) {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "ddd_metadata_test";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);

  const std::filesystem::path path = directory / "capture.ddd.yaml";
  const CaptureMetadata metadata = Ordinary();

  std::string error;
  ASSERT_TRUE(WriteCaptureMetadataFile(path, metadata, error)) << error;
  ASSERT_TRUE(std::filesystem::exists(path));

  // Read in a scope of its own, so the handle is closed before the directory
  // goes. Windows refuses to delete a file something still has open, and this
  // is what threw "the process cannot access the file" out of the tidy-up.
  std::string written;
  {
    std::ifstream file(path, std::ios::binary);
    written.assign((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  }
  EXPECT_EQ(written, BuildCaptureMetadataYaml(metadata));

  std::filesystem::remove_all(directory);
}

TEST_F(CaptureMetadataTest, AFailureToWriteIsReportedRatherThanThrown) {
  // Never treated as a capture failure by the caller — the recording is on disk
  // and complete — so this has to come back as a value rather than as an
  // exception somebody has to remember to catch.
  std::string error;
  EXPECT_FALSE(WriteCaptureMetadataFile(
      std::filesystem::path("/this/directory/does/not/exist/x.ddd.yaml"),
      Ordinary(), error));
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace ddd::capture
