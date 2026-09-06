/************************************************************************

    capture_format.h

    What the capture application writes, and what it can read back
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "sample_format.h"

namespace ddd::capture {

// This application writes native FLAC, and uncompressed signed 16-bit for
// anyone who would rather spend disk than CPU.
//
// The historical .ldf is FLAC inside an Ogg container, and the encapsulation
// was a workaround for FLAC limitations that were fixed years ago. Dropping it
// costs nothing and gains a format two toolchains accept rather than one:
// ld-decode routes .flac and .ldf through the same libavcodec loader
// (lddecode/utils.py), while general audio editors — Tenacity, Audacity —
// import native FLAC and cannot open Ogg FLAC at all.
//
// The packed 10-bit .lds and the Ogg .ldf are neither written nor read here.
// They were the output of the capture application this one replaced, which has
// been removed from the repository; carrying a decoder for them into a format
// nothing new is written in would be duplication for its own sake.

// What a capture is written as.
enum class CaptureOutputFormat {
  // Mono 16-bit native FLAC. The default, and what an archival capture wants:
  // the encoding cost is paid once and the storage cost is paid for years.
  kFlac,

  // The same samples with nothing wrapped round them — signed 16-bit
  // little-endian, no header, no metadata.
  //
  // Twice the file for none of the encoder, which is the trade worth having on
  // a machine that cannot sustain the encode, or on a run whose output is going
  // straight into another tool. Nothing about the file says what it is or where
  // it came from, so the provenance a FLAC capture carries in its tags is
  // simply lost — which is why this is an option and not the default.
  kSigned16Bit,
};

// The rate stamped into a FLAC header.
//
// Not a measurement. FLAC's sample-rate field tops out at 655,350 Hz, which
// no rate this device can run at fits, so the file carries a label instead —
// the real rate divided by kFlacLabelScale — and everything downstream
// treats it as one. At the device's default 40,000,000 Hz this produces
// 40,000, which is what lddecode/compress.py's SAMPLE_RATE was matched to; a
// different scale here would produce a file ld-decode reads at the wrong
// speed. The real rate a file was written at, unscaled, is what
// BuildProvenanceTags writes into DDD_SAMPLE_RATE_HZ — this label is a
// container-format constraint, not the fact of record.
inline constexpr uint32_t kFlacLabelScale = 1'000;
inline constexpr uint32_t kFlacSampleRateLabel =
    kSampleRateHz / kFlacLabelScale;

// The decimation factors a capture may be written at.
//
// One is every sample, which is what a LaserDisc capture needs. Two and four
// divide both the rate and the file by that much — enough for tape RF, whose
// bandwidth is a fraction of a LaserDisc's, and the reason this exists at
// all; four is the choice when the source is narrow enough, or the disk
// budget tight enough, that halving once is not enough. Decimation happens
// in the FPGA, before the samples reach the USB link, and is independent of
// which rate the converter itself is running at (see PLL_PRESET in the
// register interface doc) — a factor here always divides whatever that rate
// is, not some fixed number. That independence is deliberate: this factor
// says how much further to divide the converter's own rate, and the
// converter's own rate is a separate choice (see
// CaptureSettings::BaseSampleRateHz()) — nothing in this file or its callers
// may assume what that rate is.
//
// Not plain selection: the gateware low-passes first with a 63-tap
// half-band filter at a quarter of its input rate, cascaded a second time for
// four, because dropping samples without that folds everything above the new
// Nyquist down on top of the signal. See fpga/application/halfBandDecimator.v.
inline constexpr int kUndecimatedFactor = 1;
inline constexpr int kTapeDecimationFactor = 2;
inline constexpr int kQuarterDecimationFactor = 4;

// Whether a factor is one this application will write.
bool IsSupportedDecimationFactor(int factor);

// Whether an ADC rate preset is one the gateware's PLL_PRESET register
// implements — see kPllPreset40Mhz..kPllPreset75Mhz in wire_protocol.h,
// which this mirrors. 0 (PllPresetNone, "no override") is deliberately not
// among these: it means run at the board's own default rate, which is a
// choice this application can offer without knowing what that rate is,
// where every other value is a specific claim about the converter that has
// to be one the gateware actually has a scan sequence for.
bool IsSupportedPllPreset(uint8_t mhz);

// The rate label for a file written at a given decimation of base_rate_hz —
// the converter's own rate, defaulting to the device's default 40,000,000 Hz
// for a build that has not asked for a different one via PLL_PRESET. A 2:1
// capture at the default rate is a real 20 Msps stream and says so, on the
// same convention as the undecimated label above.
uint32_t FlacSampleRateLabelFor(int decimation_factor,
                                uint32_t base_rate_hz = kSampleRateHz);

// The rate the samples actually arrive at, in hertz.
//
// A real measurement, unlike the FLAC label above, and the figure every display
// that turns samples into time or frequency has to use: a decimated stream at
// the default rate is 20 Msps, so its Nyquist is 10 MHz and a sample is 50 ns.
// Anything that assumes the converter's own rate here draws a tape's 5 MHz
// carrier at 10 MHz and calls a 1 ms sweep 500 µs — and anything that assumes
// the *default* converter rate where a PLL_PRESET is in effect is wrong by
// whatever that preset changed the rate to, which is why base_rate_hz is a
// parameter here rather than the kSampleRateHz constant used directly.
uint32_t SampleRateHzFor(int decimation_factor,
                         uint32_t base_rate_hz = kSampleRateHz);

// Channels and bit depth in the written file. Mono is definitional: a stereo
// file is not a Domesday Duplicator capture, and the reader says so rather than
// silently reading one channel.
inline constexpr uint32_t kFlacChannels = 1;
inline constexpr uint32_t kFlacBitsPerSample = 16;

// The suffix a capture is given by default.
//
// Two extensions rather than one, and deliberately: ".flac" is what makes the
// file open in ld-decode and in an audio editor, and ".ddd" in front of it says
// where the samples came from. A user who finds one of these a year later can
// tell it apart from an ordinary audio file without opening it.
inline constexpr const char* kCaptureFileSuffix = ".ddd.flac";

// And the uncompressed one, on the same pattern. ".s16" rather than ".raw"
// because it names the layout: signed 16-bit, and the only thing that can be
// read out of a headerless file is what its name says is in it.
inline constexpr const char* kSigned16BitCaptureFileSuffix = ".ddd.s16";

// Extensions the reader recognises, without the leading dot.
inline constexpr const char* kFlacExtension = "flac";
inline constexpr const char* kSigned16BitExtension = "s16";

// The suffix a capture in this format is written with.
const char* CaptureFileSuffix(CaptureOutputFormat format);

// Append the capture suffix to a name, unless it is already there.
//
// Idempotent because the name reaches this from a text field a user can type
// into, and "disc1.ddd.flac.ddd.flac" is the obvious way to get that wrong.
std::filesystem::path AddCaptureFileSuffix(
    const std::filesystem::path& stem,
    CaptureOutputFormat format = CaptureOutputFormat::kFlac);

// The lower-cased extension of a path, without the dot. Separated out because
// both the reader's format detection and the GUI's file dialogs need it, and
// the two must agree.
std::string LowerCaseExtension(const std::filesystem::path& file_path);

// Whichever capture suffix a path ends with, or an empty string for one that
// ends with neither.
//
// The suffixes are compound, so `path.stem()` leaves ".ddd" behind and
// `path.extension()` yields only ".flac" — neither answers the question anyone
// actually has, which is "what is this file called without the bit that says
// what format it is". Three callers need that: making a taken name unique,
// showing a name in a field, and putting the sidecar beside the capture.
std::string MatchedCaptureFileSuffix(const std::string& file_path);

// The same path with that suffix taken off, or unchanged when it had neither.
std::string StripCaptureFileSuffix(const std::string& file_path);

}  // namespace ddd::capture
