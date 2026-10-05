# Statistics

Everything the run is doing, refreshed twenty times a second. The figures come from a
wait-free tap on the capture pipeline, so reading them — however often — cannot slow the
capture down.

Every row is blank, shown as `—`, until there is something to report. A dash means *not
measured yet*, never *zero*.

## Throughput

```
76.3 MB/s  (40.00 Msps)
```

The rate over the **last second**, not the average since the run began. That distinction is
why the figure reads the device's true 40 Msps throughout instead of creeping up towards it
over the first minute.

Both units, because both are useful: MB/s against the drive and the USB link, Msps against
the converter. A working device delivers the [ADC rate](capture-control.md#adc-rate) divided
by the [decimation](capture-control.md#decimation) — 40.00 Msps from a 40 MHz board
undecimated, 75.00 from a 75 MHz one — and physically cannot deliver more. **A figure noticeably above
that means the samples are not coming from the ADC at all**, which is an unprogrammed or
wrongly programmed FPGA rather than a fast one.

## Sample rate

```
75.00 Msps received  (expected 75.00 Msps)
```

The rate the samples are actually arriving at, worked out from the throughput above, beside
the rate the capture settings say they should: the ADC rate divided by the decimation.

The two should agree to within a fraction of a percent. **When they do not, believe the
received one.** Everything worked out from the rate — the FLAC label and the
`DDD_SAMPLE_RATE_HZ` tag, the scope and spectrum axes, the duration limit, the metadata file
— uses the expected rate, so a board that did not apply an ADC rate change goes on streaming
at its old rate while every label says the new one. This line is where that shows.

## Integrity

Each 16-bit word off the device carries its sample together with a sequence marker, and the
application checks every one.

| What it says | What it means |
| --- | --- |
| **Synchronising** | Still finding the sequence. Normal for the first moments of a run |
| **Verified — no samples lost** | What you want. Nothing has been dropped anywhere between the FPGA and this application |
| **Broken — samples have been lost** | The sequence jumped. The capture is not bit-perfect |
| **Not available — this gateware does not send sequence markers** | Older gateware. The capture is fine; this particular proof is not available |

A broken sequence is almost always this machine failing to keep up, not the device
misbehaving. See [If a capture fails](if-a-capture-fails.md).

## Back pressure

How full the **FPGA's** own buffer got — the device end of the chain, before the samples
reach USB at all.

Read it with the Buffer queue below it. Together they say which end is struggling: the
device's buffer fills when the host cannot take packets fast enough, and the host's queue
fills when the disk or the encoder cannot keep up.

The caption is in words rather than percentages, and that is the point of it. A capture that
is working fills the buffer to the packet threshold on every single packet — about half of
it — and a bar reporting that as "0 %" would be true, useless, and indistinguishable from an
instrument reading nothing at all.

| Caption | What it means |
| --- | --- |
| **now *n*, peak *n* of *n*** | The ordinary case. The peak is dull on purpose: a working capture fills to the packet threshold and is drained from there, so it is that same figure on every reading, forever. It becomes news when it stops being constant |
| **Idle** | The device is attached and answering, and its buffer is neither filling nor draining |
| **… into the reserve** | Above the packet threshold. The host was late, and the buffer is into the room a stall is paid out of. This is the reading worth noticing |
| ***n* overflows, *n* samples lost** | Samples were lost at the device. Past this point the percentages have stopped being the interesting numbers |
| **Not reported by this gateware** | Gateware older than the instrument. Perfectly good at capturing; it just cannot report this |

The tooltip carries the figures behind the current reading — the peak since the device was
opened, the packet threshold, how many packets were taken since the last reading — and
changes with it.

The bar holds its position briefly rather than dropping instantly, because a reading covers a
fraction of a second and reports the worst moment in it. Without the hold, a single bad
interval would be a flash too brief to see.

## Buffer queue

How much of **this machine's** buffer queue is waiting to be processed.

A healthy capture sits near zero. A figure that climbs and stays up means the machine is not
keeping up. The caption carries the peak alongside the live value, because the peak is the
figure that matters afterwards: a capture that was fine except for one stall thirty minutes
in reads as perfect from the live value alone.

The queue size is set in [Settings](settings.md#buffer-queue).

## Signal level

```
128 to 896 of 1023  (75.1% of range, 449 mV p-p)
```

The extremes of the most recent buffer, as converter codes, with the proportion of the
converter's range they span. The millivolt figure appears only when a
[front-end gain](settings.md#front-end-gain) has been declared — the example above is at
SW401 `1010`, ×3.34.

## Extremes

The same thing across the whole run so far. The running total records the worst moment since
the run began and will not come back down, which is exactly why it is separate from the
figure above: on its own it cannot show that an adjustment has helped.

## Clipping

```
0 low, 0 high  (0 and 0 in the last buffer)
```

Samples that reached code 0 or code 1023 — the converter's ends, where the waveform stops
being a recording of anything. Whole-run totals first, the most recent buffer in brackets.

The bracketed figures are what make this usable while turning a gain control: the totals
will not fall, so only the recent count can show that a change worked.

Clipping detection never depends on the declared gain. A clipped sample is a property of the
converter, so these counts stay correct whether the gain declaration is absent, right or
wrong.

## Transfers

Completed USB transfers and buffers processed. Diagnostic — worth quoting in a bug report,
not worth watching.

## Samples, Elapsed, Written

Samples processed, how long the run has been going, and how many megabytes have reached the
disk. **Written** is not derivable from the sample count once a compressor is in the path,
which is why it is measured rather than calculated.

**Samples** and **Transfers** are scaled to three significant figures and a unit — `40.0 M`,
`90.1 G` — because a side of a disc reaches ninety thousand million samples, and nobody reads
`90,113,472,000`: they count the digit groups, get it wrong, and look away. Counts below a
thousand are given exactly, where every digit is still information. The units are powers of a
thousand, so a sample count lines up with a device specified at 40 million a second.

## Requantisation

```
Preview: 2 dropped (8 bit range), +0.12 dB in band, noise floor 1.80 LSB
```

What the [requantiser](capture-control.md#requantisation) is doing, updated with the other
figures: **Off** when it is off, in test mode, or with no stream. While monitoring it is a
preview, `Preview: …`, and changes nothing; while capturing it is what is being done to the
file, `Requantising: …`. The line gives the bits the latest decision drops and the range the
converter's codes are left spanning, `noise shaped` when the added noise is pushed out of the
band, how far that raises the noise floor of the worst 1 MHz of the protected band, and the
noise floor the decision was made against, in converter steps. The same line is under the
setting in the capture panel.

## Requantised so far

```
1.88 dropped on average (0: <1%, 1: 12%, 2: 88%), shaped 4%, worst +0.98 dB
```

The requantiser's totals since the preview or the capture began — a capture starts its own
count, and so does a preview rebuilt by a change of setting. The average number of bits
dropped is very nearly the bits a sample the FLAC file saves. Then the share of samples each
number of bits was dropped from, the share whose noise was shaped, and the most the noise floor
of any 1 MHz of the protected band rose. A dash until the first decision.

## Encoder backlog

```
12.4 ms  (496,000 samples)
```

Samples the FLAC encoder has taken but not yet written out. Shown only while a file is
attached — during monitoring there is no encoder, and a healthy-looking zero would be a
meaningless measurement rather than a good one.

A steady figure is the encoder keeping pace. One that climbs is the encoder falling behind,
and the answer to that is a **lower compression level**, not a faster drive.

The time is measured at the rate the stream is running, so it stays comparable with the
buffer queue's own depth at any
[decimation](capture-control.md#decimation): a decimated stream puts fewer samples
into a second of signal, and the same count of them is a proportionally larger backlog.

Read together with Buffer queue, the two localise a machine that is struggling:

| Buffer queue | Encoder backlog | The bottleneck is |
| --- | --- | --- |
| Near zero | Near zero | Nothing. This is a healthy capture |
| Climbing | Climbing | The FLAC encoder — lower the compression level |
| Climbing | Near zero | The disk — capture to a faster drive |

## Space left

How much longer the destination volume will hold a capture, and the bytes free. The estimate
is 40 MB/s, what a FLAC capture writes on average.

## Link speed

What the USB link actually enumerated at. Anything below SuperSpeed cannot carry a capture,
and the application refuses such a device with a specific reason rather than opening it and
failing later.

## Front-end gain

The SW401 position you declared, with the gain and full-scale input it implies — or *Not
declared — levels shown in converter codes*.

Said as a sentence rather than shown as a dash, because a dash reads as "nothing measured
yet" and this is "nobody has told me". See [Settings](settings.md#front-end-gain).
