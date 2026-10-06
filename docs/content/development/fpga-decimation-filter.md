# The decimation filter

The gateware can halve its own sampling rate: from the converter's 40 Msps to 20 Msps for
capturing tape RF, where a LaserDisc's bandwidth is not needed and half the samples are half
the file, or from 60 Msps to 30 Msps for a LaserDisc captured at the lowest rate that still
holds its whole band. This page describes the filter that makes that safe to do. Its figures
are given at 40 MHz in; every one of them scales with the input rate, so at 60 MHz in each
frequency is half as large again.

It is enabled by writing 2 to the [`DECIMATION` register](fpga-register-interface.md), which
is what the capture application's [Decimation](../capture-gui/capture-control.md#decimation)
control does. The source is `fpga/application/halfBandDecimator.v`, and the coefficients come
from `fpga/make-halfband-coefficients.py`.

Quarter rate (writing 4) is not a second filter: it is two instances of the same stage in
series, the second one fed the first's output, each halving whatever rate reaches it. Every
figure below describes one stage, at whatever rate is feeding it — halving 40 Msps to 20 Msps
for the first stage of a quarter-rate capture, then 20 Msps to 10 Msps for the second.

## Why there is a filter at all

Dropping every second sample, on its own, does not halve the bandwidth — it folds it. Any
content above the new Nyquist of 10 MHz reappears below it, mirrored about 10 MHz, and once
it has landed there nothing downstream can separate it from the signal.

The damage is not theoretical for this application. A tape's luma FM carrier sits around
5 MHz, and 15 MHz folds to exactly 5 MHz. The board's own analogue anti-aliasing filter turns
over at 13.2 MHz, so there is real energy at 15 MHz to fold. Decimating without a digital
filter first would drop a copy of the 15 MHz noise floor straight on top of the carrier the
capture exists to record.

So the stream is low-passed at 10 MHz before alternate samples are dropped, and the two are
one operation rather than an option with a caveat.

## Why this filter

**An FIR, not an IIR.** An IIR of the same selectivity would be a fraction of the arithmetic,
and its phase response would not be linear. Group delay that varies with frequency disperses
an FM signal — the sidebands arrive at a different time from the carrier — and that is
distortion no later stage can undo. A symmetric FIR has exactly constant group delay by
construction, and that is worth far more here than the logic it costs.

**A half-band FIR.** The cutoff needed is exactly a quarter of the input rate, which is the
one case a half-band filter is built for. It has two properties that follow from its form
rather than from the design:

- Every second coefficient either side of the centre is **exactly zero**. Forty-six of the
  ninety-four non-centre taps cost nothing at all.
- The centre tap is **exactly one half** — a power of two, so it costs no multiplier whichever
  way it is spelled.

That leaves forty-eight taps at odd offsets. Symmetry pairs them off, and every linear-phase
FIR is symmetric, so ninety-five taps are computed with **twenty-four multiplies**. That is
what makes a filter this sharp affordable on a Cyclone IV.

**Ninety-five taps, Kaiser window, β = 7.** The length and the window were chosen by
measuring candidates rather than from a rule of thumb. A half-band is flat to some edge and
reaches its stopband at the half rate minus that edge, so everything that could fold into the
flat part is in the stopband: **the flat part is alias-free by construction**, and the length
sets how wide it is. Ninety-five taps put the edge at 0.225 of the input rate and the stopband
at 0.275 — at 60 MHz in, flat and alias-free to 13.5 MHz, a LaserDisc's whole band.

The earlier 63-tap filter ran its transition from 0.2 to 0.3 of the rate. That was fine for
tape at 40 MHz in, but at 60 MHz in it was flat only to 12 MHz and folded 16.5 MHz onto
13.5 MHz at −32 dB, inside the band. Longer than ninety-five buys next to nothing: with
coefficients on a 2<sup>15</sup> scale the stopband floors at −72 to −76 dB however long the
filter is. A 10-bit converter's quantisation floor is around −62 dB, and the stopband is
already 10 dB below that — spending more logic to push it lower would be improving a number
nothing downstream can measure.

## Coefficients

Twenty-four values, the odd-offset taps of one half, innermost first. The centre is 16384 and
every even offset is zero.

```
10415  -3431   2011  -1386   1028   -792    623   -496
  396   -316    252   -199    156   -120     92    -69
   50    -36     25    -17     10     -6      3     -1
```

Scaled by 2<sup>15</sup>, held as 16-bit signed. Three properties are exact rather than
approximate, and each one is checked by `fpga/tests/test_halfband_coefficients.py`:

| Property | Value | Why it has to be exact |
| --- | --- | --- |
| Sum of all taps | 32768 = 2<sup>15</sup> | DC gain is exactly unity after the shift. A filter that was 0.2% low would darken every capture by a fixed amount |
| Centre tap | 16384 = 2<sup>14</sup> | A power of two, so the centre costs no multiplier. The Verilog writes it as a multiply by the constant anyway, because that gives it the same width as every other product and so needs no sign extension spelled out |
| Even-offset taps | 0 | The half-band property. If rounding had left them at ±1 they would cost twenty-three more multiplies for nothing |

The generator forces the even taps to zero and puts the whole of the DC-gain correction on
the centre tap, which is the one place it can go without disturbing the symmetry or the
half-band structure.

## Magnitude response

Measured from the coefficients as committed in the Verilog, at 40 MHz in:

| Frequency | At 60 MHz in | Response | |
| --- | --- | --- | --- |
| 0 – 9 MHz | 0 – 13.5 MHz | **±0.0021 dB** | the passband, flat to well under the converter's own linearity |
| 9.09 MHz | 13.6 MHz | −0.01 dB | |
| 9.25 MHz | 13.9 MHz | −0.1 dB | |
| 9.56 MHz | 14.3 MHz | −1 dB | |
| 9.80 MHz | 14.7 MHz | −3 dB | |
| **10.00 MHz** | **15 MHz** | **−6.02 dB** | exactly half amplitude — fixed by the half-band form |
| 10.5 MHz | 15.75 MHz | −22.2 dB | |
| 10.96 MHz | 16.4 MHz | −72 dB | stopband reached |
| 11 – 20 MHz | 16.5 – 30 MHz | **≤ −72.4 dB** | worst case at 11.27 MHz |

The transition occupies 9.1 to 11 MHz. Below it the filter is transparent; above it,
nothing survives that the converter could have resolved in the first place.

### The band edge

The response passes through exactly −6 dB at 10 MHz and is **antisymmetric** about that
point. That is not a coincidence of the design, it is the defining property of a half-band
filter, and it holds exactly:

```
|H(10 - d)| + |H(10 + d)| = 1     for any offset d
```

| Offset *d* | \|H(10−*d*)\| | \|H(10+*d*)\| | Sum |
| --- | --- | --- | --- |
| 0.25 MHz | 0.75493 | 0.24507 | 1.000000 |
| 0.50 MHz | 0.92239 | 0.07761 | 1.000000 |
| 1.00 MHz | 1.00011 | 0.00011 | 1.000212 |

(The last row's 0.02% is the passband ripple at 9 MHz showing through, not a departure from
the form.)

**This is the one thing the filter cannot fix.** Energy just above 10 MHz is attenuated only
as much as its mirror image just below 10 MHz is kept, so it folds down at a comparable
level. At 10.5 MHz the alias lands on 9.5 MHz at −22.2 dB, which is not negligible.

That is a property of halving a sampling rate, not a shortcoming of this implementation: no
filter of any length can both pass 9.9 MHz and reject 10.1 MHz. **A signal with real content
near 10 MHz should be captured at the full rate** — or, at 60 MHz in, near 15 MHz. What a
longer filter does is narrow the region this applies to: everything below 9 MHz (13.5 at
60 MHz in) is alias-free. Tape RF has nothing up there, which is what makes 20 Msps the right
choice for it, and a LaserDisc's band ends at 13.5 MHz, which is what makes 30 Msps work.

### What folds where

For content that survives the filter, this is where it ends up:

| Input frequency | At 60 MHz in | Aliases to | Attenuation |
| --- | --- | --- | --- |
| 10.5 MHz | 15.75 MHz | 9.5 MHz | −22.2 dB |
| 11 MHz | 16.5 MHz | 9 MHz | −79.5 dB |
| 12 MHz | 18 MHz | 8 MHz | −79.3 dB |
| 13 MHz | 19.5 MHz | 7 MHz | −98.7 dB |
| 15 MHz | 22.5 MHz | 5 MHz | −77.1 dB |
| 18 MHz | 27 MHz | 2 MHz | −103.3 dB |
| 19.5 MHz | 29.25 MHz | 0.5 MHz | −83.0 dB |

The 15 MHz row is the case the filter exists for at 40 MHz in: the fold onto a tape's luma
carrier is 77 dB down, 15 dB below the converter's quantisation floor. (The 63-tap filter put
it at −85.5 dB; this one spends its length on a narrower transition rather than on the depth
of particular points, and every point in the stopband is still below the floor.) The 11 MHz
row is the one it exists for at 60 MHz in: 16.5 MHz, which folds onto the top of a
LaserDisc's band, is 79 dB down where it was 32.

## Phase and delay

The filter is **exactly linear phase**. The impulse response is symmetric about its centre
tap, which forces the phase to be a pure linear function of frequency:

```
phase(f) = -2 * pi * (f / 40 MHz) * 47 radians
         = -423 degrees per MHz
```

The consequence is constant **group delay**:

| | |
| --- | --- |
| Group delay | **47 input samples** — exactly (*N*−1)/2 for *N* = 95 |
| In time | **1,175 ns** at 40 MHz in, 783 ns at 60 |
| In output samples | 23.5 |

Every frequency in the passband is delayed by the same time, so the waveform arrives later
but otherwise unaltered — no dispersion, no ringing asymmetry, no differential delay between
an FM carrier and its sidebands. This is the property that made an FIR worth its logic, and
it holds at any length: a longer filter is a longer delay, not a different shape.

Two details worth knowing:

- The delay in *output* samples is **23.5** — a half sample. The filter's centre of symmetry
  falls between two output samples, because 47 is odd and only every second input sample is
  kept. It is a constant offset and nothing in the capture path depends on the alignment, but
  it means a decimated capture is not sample-aligned with an undecimated one of the same
  material.
- Group delay is a property of the filter, not of the implementation. The pipeline below adds
  its own latency, which is a separate figure.

## Implementation

Eight pipeline stages, so that no combinational path has to settle the whole 95-tap sum
inside one system clock period. There are four system clocks per output sample, so the
depth costs latency and nothing else.

| Stage | What happens |
| --- | --- |
| 1 | The twenty-four symmetric pre-adds — each pair of equal-coefficient taps summed to a 12-bit signed value — and the centre sample taken |
| 2 | Twenty-four products, 12-bit signed × 16-bit signed coefficient; the centre sample multiplied by 2<sup>14</sup> |
| 3 – 6 | Adder tree, 24 → 12 → 6 → 3 → 2, with the centre folded in at the first level and the odd third carried through the last |
| 7 | The final sum |
| 8 | Rounded and clipped to a 10-bit sample |

Adding each symmetric pair *before* multiplying is what halves the multiplier count: the two
taps share a coefficient, so `(a + b) × c` replaces `a × c + b × c`. All forty-nine taps
that matter are read from one consistent snapshot of the delay line, so the pre-adds cannot
straddle two different cycles' worth of history.

A `stage_valid` shift register carries validity alongside the data, so the output asserts
`output_enable` exactly when a filtered sample emerges — one for every two that go in.

Rounding is add-half-then-shift: +2<sup>14</sup> then an arithmetic shift right by 15, then
a clip to 0 – 1023. Truncation instead would bias every sample downwards by half a code, a DC
offset of −0.5 LSB across the whole capture.

The accumulator is 32 bits. The worst case is about 3.1 × 10<sup>7</sup>, under
2<sup>25</sup>, so that is seven bits of headroom over the real bound rather than a guess.

### Cost on the device

The 63-tap filter, measured on the whole application image:

| | |
| --- | --- |
| Logic elements | 2,999 of 22,320 (13%) |
| Embedded multipliers | **0 of 132** |
| Worst-case setup slack, 80 MHz, slow 85 °C | **+0.887 ns** |

The 95-tap filter has half as many multiplies again and an adder tree one level deeper, in
each of the two stages; its own figures are what the Quartus build reports, and are to be
recorded here once measured. Every coefficient is a compile-time constant, so Quartus can
build each multiply as a shift-and-add network in ordinary logic rather than take any of the
132 hard multipliers.

### Bypass

With `decimate` low the input reaches the output **unaltered on the cycle it arrived**. This
is a true bypass, not the filter configured to pass everything: the arithmetic runs on and its
result is ignored.

An undecimated capture is therefore bit-for-bit identical to what the gateware produced before
this module existed — no added delay, no rounding, nothing. That is the property that matters
for LaserDisc capture and for the test-pattern integrity check, and it is why the passthrough
path is checked separately in the testbench.

### Position in the chain

The decimator sits **in front of** the test-pattern generator and the sequence counter:

```
ADC ─► halfBandDecimator ─► dataGenerator ─► buffer ─► FX3
```

So the counter and the test ramp attach to the samples that survive, not to the ones that were
dropped. A decimated test capture is an unbroken ramp at 20 Msps, and the integrity oracle
checks the decimated path exactly as it checks the full-rate one. A host-side decimator could
never have had that property — it would have been discarding the very markers the check
depends on.

## How this is verified

| Check | What it covers |
| --- | --- |
| `fpga/tests/test_halfband_coefficients.py` | DC gain, centre tap, half-band zeros, symmetry, 16-bit fit, response at fixed frequencies, the whole passband to 9 MHz within 0.05 dB and the whole stopband from 11 MHz at −70 dB or below, and that the committed Verilog table matches the generator character for character |
| `fpga/tests/tb_halfBandDecimator.v` | Drives real sinusoids and measures amplitude: passthrough, output rate, DC handling, passband at 1/5/8/9 MHz, stopband at 11/13/15/18 MHz, both band edges, and clipping |
| `ddd-gui` hardware test | `TheDecimatedTestPatternArrivesIntactAtHalfTheRate` — streams from the device and checks the ramp survives at half the rate |

The band-edge test uses 9.5 and 10.5 MHz rather than 10 MHz itself. At exactly 10 MHz there
are four samples per cycle, so decimation lands on either the zero crossings or the peaks
depending on phase, and the measured amplitude says more about that phase than about the
filter.

!!! note "What the hardware tests cannot show"
    A ramp is not a spectrum — every sample of one sits in the passband — so the hardware test
    proves the decimation plumbing and proves nothing about the frequency response. The
    response is pinned by the simulation testbench and the coefficient arithmetic only.
    Measuring it on real hardware would need a signal generator and a spectrum analyser on the
    RF input.

## Regenerating the coefficients

```
python3 fpga/make-halfband-coefficients.py            # the Verilog table
python3 fpga/make-halfband-coefficients.py --response # the measured response
```

The script has no dependencies — it computes the sinc, the Kaiser window and the Bessel
function it needs itself — so it runs anywhere Python does, and the checks that compare its
output with the committed Verilog run in CI without a numeric stack.

Changing the length, the window or the scaling changes the table, and
`test_halfband_coefficients.py` will fail until the new table is pasted into
`halfBandDecimator.v`. That failure is the point: the coefficients in the gateware and the
script that documents them cannot drift apart silently.
