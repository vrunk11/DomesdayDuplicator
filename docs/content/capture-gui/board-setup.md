# Board setup

**File → Settings… → Board setup** says what the capture board *is*: which converter is
soldered to it, how that converter's RSEL pin is wired, and the DC offset its analogue front
end puts on the signal. The panel on the [Capture control](capture-control.md) page shows it
read-only, beside the two controls it bounds, with a **Board setup…** link back here.

!!! note "A declaration, not a capture setting"

    None of this changes from one capture to the next, and none of it can be read off the
    hardware — the gateware drives RSEL whether or not anything is wired to it, and nothing
    on the board reports its converter. It is declared once per board, and the capture
    settings are bounded by it: the ADC rate list stops at the declared converter, and the
    input range is fixed where RSEL is wired to a level.

## Where it is kept

On the device, in the last page of the FX3 kit's boot EEPROM — not on this computer. The kit
brings its declaration to any machine it is plugged into, and several Duplicators on one
machine each keep their own.

The kit is a board of its own that plugs into the capture board, and it can be moved to
another one. The declaration then describes the board it came from, which is what the
**Board name** is for: seeing the old board's name here is the reminder to declare the new
one.

Nothing on this tab waits for the dialog's **OK**. **Write to board** writes there and then,
and **Cancel** does not take it back. Both **Measure** and **Write to board** need the stream
stopped: stop monitoring first.

A device whose firmware predates the board setup cannot store it. What is written then applies
until the application closes, and the tab says so; [update the firmware](updating-your-domesday-duplicator.md)
to keep it on the board.

## What is declared

| Field | Choices | When nothing has been declared |
| --- | --- | --- |
| Board name | Free text, up to 32 bytes | Empty |
| ADC fitted | **ADS825** — up to 40 MSPS · **ADS828** — up to 75 MSPS | ADS825 |
| RSEL wiring | **Auto** — routed to the FPGA · **Low** — tied low, always 1Vpp · **High** — tied high, always 2Vpp | High |
| DC offset at 1Vpp, at 2Vpp | −512 to +511 converter codes, with **Measure…** | 0 |

The defaults are the conservative board: every Duplicator ever built runs at 40 MSPS, and one
whose RSEL is not routed captures at 2Vpp.

### ADC fitted

Read the part number printed on the converter chip.

**Declaring a converter that is not fitted does not make the board faster.** Past its rated
speed an ADC keeps sending samples — wrong ones — and nothing reports it: the integrity check
proves the samples arrived, not that the converter got them right, and the received sample rate
is the rate the gateware clocks the converter at. Changing the declaration from ADS825 to
ADS828 asks you to confirm you have checked the chip. Going the other way is never asked about,
since the slower declaration can only cost speed.

### RSEL wiring

RSEL selects the input range on both converters. Whether the board routes it to the FPGA is a
separate question from which converter is fitted.

- **Auto** — the [Input range](capture-control.md#input-range) control chooses per capture,
  and there is an offset for each range.
- **Low** or **High** — the pin is soldered to one level. The Input range control is greyed
  out and shows the wired range, every capture runs at and records that range, and only its
  offset is used.

### DC offset

How far above code 512 the signal sits with nothing connected to the input, in whole converter
codes. On some boards the front end is not quite centred; the offset takes that out of every
sample written, so the signal is centred in the file.

One per range, because a DC error in the front end is a voltage, and 1Vpp spreads the same
voltage over twice as many codes.

Whole codes, deliberately. Samples are written as the converter code times 64, so their six low
bits are always zero and FLAC stores them for nothing; a fractional correction would fill them.
The offset applied is recorded in every capture — `DDD_DC_OFFSET` in the file and `dc_offset`
in the [metadata file](capture-naming.md#board) — so the converter's own codes can always be
recovered. A test-mode capture is never corrected: its samples are the gateware's counter,
not the converter's.

## Measuring the offset

1. Stop monitoring.
2. **Disconnect the BNC input.** The dialog that opens asks you to, and **Cancel** there
   changes nothing.
3. **Measure…** starts the stream at each range the RSEL wiring can select, lets it settle for a
   quarter of a second, averages one second of it, and stops. One second is tens of millions of
   samples, so the converter's noise averages away, and it is a whole number of mains cycles at
   50 Hz and at 60 Hz, so hum does too.
4. The results fill the fields, each with the time it was measured. They are not on the board
   yet: **Write to board** puts them there.

A measurement is refused when the input is not quiet — when the signal covered more than 64
codes while it was being averaged. That is what the BNC still connected looks like, and the
mean of a player's output would otherwise be declared as the board's own offset.

An offset typed by hand is marked *not measured*.

## When the offset is wrong

With the offset measured on this board, the correction cannot push a sample out of range on its
own. The signal is symmetric about its mean, and its mean is the offset, so it cannot reach the
far end of the corrected range without the converter clipping at the near end first.

So a sample the correction had to hold at the end of the range — one the converter itself had
not clipped — means the declared offset belongs to some other board. The capture panel then
shows a warning, until the next run starts:

> The DC offset correction is pushing samples out of range. The offset declared for this board
> does not match its signal — the board setup is wrong, not the input level.

The samples are held at the end of the range, never wrapped round to the other end, and each
capture's metadata records how many there were as `offset_saturated_samples`. Samples the
converter clipped are counted as clipping, as they always were, and never here.
