# Comparing requantisation settings

Which [requantisation](capture-control.md#requantisation) settings are right for a player and
a disc is a question for experiment: the margin, the bands, the shaping, its depth and its
order all trade file size against what is left of the noise floor, and the only judge that
matters is what ld-decode makes of the result. Trying each setting during a capture of its
own is slow and never quite fair, because no two plays of a disc are the same.

`ddd-requantize` takes one capture, written with requantisation **off**, and requantises it
afterwards, as many times and as many ways as are worth trying. Every version is made from
exactly the same samples, so the only difference between them is the setting.

## Identical to a capture

`ddd-requantize` is not a re-implementation. It drives the requantiser the capture application
drives, given its settings by the same code, fed the same segments in the same order from the
start of the file, and it stamps the same tags. A capture requantised here holds exactly the
samples the capture application would have written with those settings, byte for byte. The
unit tests check this by putting the same samples through both and comparing the files.

So a setting found here can be taken back to the capture panel, or to the
[command line](command-line.md), and a capture made with it will be what was tried.

## Where it is

| | |
| --- | --- |
| Linux (Nix) | `ddd-requantize`, beside `ddd-gui` |
| Linux (Flatpak) | `flatpak run --command=ddd-requantize io.github.simoninns.DddGui` |
| Windows | `C:\Program Files\Domesday Duplicator\ddd-requantize.exe` |
| macOS | Not in the DMG yet: build it from source with the capture application |

## Usage

```
ddd-requantize [options] <input> [<output>]
```

The input is a capture, `.flac` or `.s16`, or `-` for signed 16-bit little-endian samples on
standard input. The output is `.flac`, `.s16`, or `-` for signed 16-bit samples on standard
output. Without an output nothing is written, and the report says what a capture would have
done, as the capture panel's preview does.

| Option | |
| --- | --- |
| `--rate <MHz>` | The samples' rate. A FLAC capture says what it is; an uncompressed one has to be told: `30` for 60 MHz halved |
| `--margin <0-4\|off>` | How far the noise floor may rise in any 1 MHz slice of the protected bands: `0` aggressive (1 dB), `1` (0.5 dB), `2` safe (0.2 dB, the default), `3` (0.1 dB), `4` (0.05 dB). `off` writes the samples as they are: the reference the others are compared with |
| `--band <bands>` | The protected bands, in MHz: `0-12`, `2-14`, or several, `0-1.9,2.1-13.5`. Default: DC to 14 MHz, or 1.5 MHz short of the Nyquist limit |
| `--shaping <shaping>` | `fixed` (the default) or `adaptive` |
| `--depth <dB>[,<dB>...]` | How far above the bands the shaped noise may go, 0 to 40. One depth for everywhere outside the bands, or one for each stretch between them from DC up: with bands `2-14`, `10,40` spares the EFM below 2 MHz and puts the rest above 14 MHz. Fewer depths than stretches repeat the last; more is an error. Default 10 fixed, 20 adaptive |
| `--order <2-64>` | The shaping filter's order. Default 16 fixed, 32 adaptive |
| `--input-bits <5-16>` | The bits the input carries. A FLAC capture says, through its bit shift; otherwise they are found from the samples |
| `--compression <0-8>` | The FLAC level for a `.flac` output, as flac's `-0` to `-8`. Default 8, as a capture's |
| `--log <file.csv>` | One line per segment: what was decided, the noise floor it was decided against, and the slice of the bands that held it back |

The options are the same settings as the capture panel's, under shorter names. The command
line of `ddd-gui` spells them `--requantize`, `--requantize-band`, `--requantize-shaping`,
`--requantize-shaping-depth` and `--requantize-shaping-order`.

| Exit code | |
| --- | --- |
| 0 | Done |
| 2 | The command line was wrong, including settings that cannot be used together |
| 3 | The input could not be read |
| 4 | The output could not be written |

## A comparison, step by step

1. **Capture once with requantisation off**, as FLAC, at the rate you mean to use. A capture
   that was requantised when it was taken can be requantised again, and the tool says so, but
   the noise it added is already in it: start from one that was not.

2. **Write the reference** through the same encoder, with nothing dropped. Comparing against
   the original file would also measure the difference between two encodes.

   ```bash
   ddd-requantize --margin off disc.flac reference.flac
   ```

3. **Write each setting worth trying.**

   ```bash
   ddd-requantize --margin 0 --shaping adaptive --band 2-14 --depth 40 disc.flac depth-40.flac
   ```

   ```bash
   ddd-requantize --margin 0 --shaping adaptive --band 2-14 --depth 10,40 disc.flac efm-10.flac
   ```

4. **Compare.** Each run ends with the bits dropped and for how much of the capture, how much
   of it was shaped, the worst rise in the protected bands and the file's size in bits a
   sample. Then decode each file with ld-decode and compare the picture, the VITS figures
   ld-analyse gives for a disc that has them, and the EFM error counts, against the
   reference.

The setting worth keeping is the smallest file whose picture and EFM cannot be told from the
reference's.

A capture is decided segment by segment from its start, so a short extract of a long capture
is not decided exactly as that stretch was within the whole: the floor the first segments
are measured against is the extract's own. Compare whole captures, or extracts against
extracts.

## What it writes

A FLAC output keeps the input's tags, with `ENCODER` set to `ddd-requantize` and the four
`DDD_REQUANTIZATION` tags replaced by what was done, exactly as a capture records them (see
[What the file says about itself](capture-files.md#what-the-file-says-about-itself)). A
`.s16` output is the samples alone. Nothing else is written: no metadata sidecar, which is
the capture application's, and the CSV log only when it is asked for.

The report goes to standard output, or to standard error when standard output is carrying the
samples, so that nothing but samples ever reaches a pipe.
