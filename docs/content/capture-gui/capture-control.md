# Capture control

The **Capture** panel is the one that starts things: what the capture is called, what it is
written as, and the buttons. Which device and which folder are set once in
[Settings](settings.md) — see [below](#where-the-device-and-the-folder-went).

## Monitoring and capturing

Two buttons, and the difference between them is the whole model.

**Start monitoring** opens the device and runs the stream with no file on the end of it.
The samples are validated, measured and drawn, and nothing is written anywhere. Use it to
set a player's RF output, to check a cable, or to decide whether a disc is worth capturing.
It costs nothing to leave running.

**Start capture** attaches a file to that stream. From idle it starts the stream as well, so
the common case is one press rather than two. From an existing monitoring session the file
is attached at the next buffer boundary and the stream is not interrupted — the device never
knows a capture began.

**Stop capture** detaches the file and finalises it, and leaves the stream running. That is
what makes both sides of a disc possible without reopening the device: stop, turn the disc
over, start again.

**Stop monitoring** ends the stream. It is disabled while a capture is running, because a
capture *is* this stream with a file on the end of it — stopping the stream would end the
recording too, which made this a second, unlabelled stop button sitting directly above the
real one. Stop the capture first; that leaves the stream running, so nothing is lost by
taking the shortcut away.

Both buttons take on a colour while they are doing something, so the state reads from across
a bench rather than only by reading the label: green while monitoring, red while capturing.
The red says *recording* — it is a normal state, not a fault.

**Automatic capture…** is the third button, and it is greyed out until
[player control](player-control.md) is on and a player is connected — present rather than
absent, so that a panel with no player attached still says the path exists. It opens the
[four-page workflow](player-control.md#capturing-a-side-by-itself) that examines the disc,
names the capture from what it found, takes the side, and reports what was written. It is a
way into that window rather than a mode the panel enters: the format and sample rate below
are the same settings the workflow's own second page shows, and the two cannot disagree.

Both buttons have a command-line equivalent, for when something other than a person has to
press them — an audio recording that must start at the same moment, or a capture nobody is
going to be in the room for. A capture started from a script can be stopped from this panel
and one started here can be stopped from a script; they are the same capture either way. See
[Scripting captures](scripting.md).

## Where the device and the folder went

Both are on the **Capture** tab of **File ▸ Settings…** rather than on this panel. Neither
changes once it is set — a Duplicator does not move between USB ports and captures do not
move between drives — and a control that is set once does not earn a row on the panel you
work from.

**Which device.** *Preferred device* in Settings, which offers *Whichever is attached* by
default and is what almost everybody leaves it at. The list names what is wrong with a device
rather than hiding it:

| Entry | What it means |
| --- | --- |
| A bare path | A capture device, ready |
| *— recovery mode, no firmware installed* | The device's USB chip has no firmware it will run. It cannot capture. [Tools ▸ Firmware ▸ Bring up a new or legacy board…](bringing-up-a-board.md) programs it whatever state it is in; [Update firmware…](if-an-update-fails.md) repairs it if it was working before |
| *— original firmware, too old for this application* | The device is running the firmware it had before this application existed. It works, but nothing here can talk to it — see [Bringing up a new or legacy board](bringing-up-a-board.md) |
| *— connected at insufficient speed* | It enumerated below SuperSpeed. It is on a USB 2 port or through a hub that is, and cannot carry 80 MB/s |

A device with no firmware is listed rather than hidden. Reporting "no device attached" to
somebody looking straight at one is exactly the moment they most need to be told what to do
next — which is why the **Status** line at the bottom of this panel says the same thing
without your having to open Settings at all.

**Where the file goes.** *Folder* in Settings, with a `Browse…` beside it. It defaults to the
platform's Movies or Videos folder — a capture is tens of gigabytes of media, and that is the
location a machine's backup rules and disk-space expectations are already built around.

The **Free space** row below stays on this panel, because it is live and it is about the
capture you are about to take. Hover it and it names the volume it is talking about.

Test mode lives on the **Tools ▸ Test data** submenu, for the same kind of reason: it is a
diagnostic rather than part of setting up a capture. The full procedure, and how to check a
test capture afterwards, is on [Test mode and integrity checking](test-mode.md).

## What gets written

### Name

Leave it empty and each capture is named after the time it was taken —
`RF-Sample_2026-08-16_14-30-00` — in local time, which is what keeps a folder of captures in
order and matches what the person who took them remembers.

Type a name and it is used instead. Characters that would not survive a copy to another
machine are removed: Windows rejects `<>:"/\|?*`, treats a trailing dot or space as
invisible, and reserves a list of device names. A name that works where it was typed and
fails on a colleague's machine is prevented here rather than discovered later.

In test mode the field is disabled, not merely ignored. A field that accepted text and then
did not use it would be a lie about what the application was going to do.

**Naming…** beside the field opens the other way of naming a capture: say what the disc is —
title, type, standard, side, notes, mint marks — and let a name be built from it. All of it
is recorded in the metadata file written beside every capture, whether or not any of it
reaches the file name. See [Naming and metadata](capture-naming.md).

A name typed here wins outright over anything set there, and the dialog says so rather than
leaving it to be discovered.

**The Naming… button colours itself** when nothing at all has been said about the disc — no
name typed, no field ticked — so that a capture is not filed under a timestamp by accident.
It is a nudge and not a warning: an unnamed capture is a legitimate way to work, the colour
is the theme's accent rather than an error red, and **Start capture** is never held up by it.
The colour clears on the first keystroke in the Name field rather than when the field loses
focus, so it is not arguing with you while you type. Test mode suppresses it, because there
the name is forced to `TestData_` and there is nothing to offer.

### Format

| Choice | What you get |
| --- | --- |
| **FLAC — `.ddd.flac`** | Mono 16-bit native FLAC, roughly half the size, carrying the capture's provenance in its tags. The default |
| **Uncompressed — `.ddd.s16`** | The same samples with nothing wrapped round them: signed 16-bit little-endian, no header |

Uncompressed is twice the disk for none of the encoder, which is the trade worth having on a
machine that cannot sustain the encode or when the output is going straight into another
tool. Nothing in the file says what it is, what rate it was written at or which build
produced it — that is the format's nature, and the reason FLAC stays the default.

An uncompressed capture can be encoded to FLAC afterwards, which is the usual thing to do
when the format was chosen because the machine could not sustain the encoder live:

```bash
flac -8 --force-raw-format --endian=little --sign=signed \
     --channels=1 --bps=16 --sample-rate=40000 \
     capture.ddd.s16 -o capture.ddd.flac
```

The raw file says nothing about itself, so every one of those has to be supplied — including
`--sample-rate=20000` in place of `40000` for a capture taken at 20 Msps. The full
walkthrough, and what to do about the tags the conversion cannot recover, is on
[Capture files](capture-files.md#compressing-a-raw-capture-afterwards).

Both are read back by **Tools → Test data → Analyse test data…** and by `--analyse-test-data`.

### Decimation

| Choice | What you get |
| --- | --- |
| **Every sample (LaserDisc)** | The converter's own rate, undivided. The default |
| **Half rate (VHS and other tape)** | Half the rate, half the file |
| **Quarter rate** | A quarter of the rate, a quarter of the file |

The choices are named by what they are for and by how much they divide, rather than by an
absolute number: this setting divides the [ADC rate](#adc-rate) below, and that rate is
itself a separate, independently selectable setting — a build with a reconfigurable PLL is
not always running at 40 Msps, so a fixed number here would only be true for the default rate.
"Half" always means half of whatever the ADC rate is, however that was chosen.

**VHS names the common case for half rate rather than the only one** — Betamax, Video8 and
any other tape format are the same choice, because what they share is a bandwidth that is a
fraction of a LaserDisc's. Quarter rate is for sources narrower still, or for when the disk
budget does not allow half.

**Decimation happens in the FPGA, not on this machine**, and that is what makes it worth
having: dividing the rate correctly means low-passing the signal first, at half of whatever
rate feeds each stage, and the gateware does that with a 63-tap half-band filter costing 13%
of the FPGA's logic and no CPU at all — cascaded a second time for quarter rate. The
application asks for the division over the register link and receives a stream that is
already divided — so the USB link carries proportionally less data too.

Without the filter, decimation would fold everything above the new Nyquist down on top of the
signal: at half rate, a component just above the ADC rate's quarter would reappear just below
it, directly on top of a tape's luma FM carrier, and nothing downstream could tell the alias
from the signal. The filter is flat to within 0.0015 dB up to 80% of its passband and 85 dB
down beyond it, so that fold lands well below the converter's own noise floor. It also delays
every frequency by the same amount, so an FM carrier and its sidebands arrive together rather
than being smeared apart.

What no filter can do is protect the band edge itself. The response passes −6 dB at exactly
the new Nyquist and is symmetric about it, so energy just above it still folds down to just
below it. **A signal with real content near the new Nyquist should be captured at a lower
decimation instead** — that is a property of halving a sampling rate, not a shortcoming of
this implementation.

Tape RF has a fraction of a LaserDisc's bandwidth, which is what makes half rate enough for
it.

The design, the coefficients and the measured response and phase are on
[The decimation filter](../development/fpga-decimation-filter.md).

The setting is written to the device before the stream is opened, so it is **fixed from the
moment monitoring starts** rather than only for the duration of a capture — there is no way
to change it under a running stream, and a control that stayed live would appear to work and
do nothing. Stop monitoring to change it.

The [signal analysis](signal-analysis.md#at-20-msps) panels follow it, scaled to whatever the
resulting rate actually is: the spectrum's axis tops out at the new Nyquist, the scope's spans
cover proportionally more time, and the bins are proportionally narrower. A display that kept
the converter's undivided rate would put a tape's 5 MHz carrier at the wrong place on the
axis.

A decimated FLAC capture carries the rate label for the resulting rate and a `DDD_DECIMATION`
tag. A decimated `.ddd.s16` carries nothing at all, because there is nowhere to put it: write
the rate down.

It works in test mode too. The gateware generates its test pattern downstream of the
decimator, so a decimated test capture is an unbroken ramp at the decimated rate and
**Tools → Test data → Analyse test data…** checks the decimated path exactly as it checks the
full-rate one.

### Input range

| Choice | What you get |
| --- | --- |
| **2Vpp (default)** | The wider range. The safe default until the source's own level is known |
| **1Vpp** | The narrower range, for a source that never approaches 2Vpp |

The ADC's input range, sent to the device as RSEL. Clipping the input loses signal
irrecoverably, where a range wider than the source's own output level only costs resolution —
so 2Vpp is the default, and 1Vpp is a choice to make deliberately once the source is known to
need it, not a default to guess at.

Only takes effect on gateware built for a board with the RSEL-capable ADC (ADS828 and later);
gateware built for the earlier ADS825 board stores whatever is written here but has nothing
wired to read it back from, so the setting is harmless to leave alone on either board.

Written to the device before the stream is opened, on the same terms as decimation above:
fixed from the moment monitoring starts, and not changeable under a running stream.

### ADC rate

The converter's own rate, before decimation above divides it further — sent to the device as
PLL_PRESET. **Board default** is whatever rate this build's gateware was compiled for, and is
the only choice offered by a board or gateware build that cannot report which other rates it
supports; a board built with a reconfigurable PLL offers every rate up to its own
`MAX_ADC_RATE_MHZ` capability instead.

This is a property of the converter, not of the capture: decimation above always divides
whatever this is set to, and nothing in the application assumes it is any particular number.
A capture's real rate — after both settings are applied — is what is written into the FLAC
label and the `DDD_SAMPLE_RATE_HZ` tag, not a number baked into either control.

Refused by the device if it is above what the connected board can do, on the same terms as
every other register write here: fixed from the moment monitoring starts, and not changeable
under a running stream.

### Compression

FLAC compression, 0 to 8. The default of 8 gives the smallest file and is what a
multithreaded libFLAC sustains at the device's full rate. It is disabled for the
uncompressed format, which has no encoder to ask.

Lowering it is the first remedy for a machine that cannot keep up. The **Encoder backlog**
and **Buffer queue** figures in [Statistics](statistics.md) are what say whether that is the
problem: a backlog that climbs is the encoder, a queue that climbs with no backlog is the
disk.

### Duration limit

Stop automatically after this many minutes, or **No limit**, which is the default. The stop
lands on a buffer boundary, so nothing is half-written. **Reset** puts it straight back to
No limit — a limit tends to be set for one capture, and holding the down arrow back from
forty minutes is forty presses.

The limit is read on every statistics tick rather than latched when the capture starts, so
both it and **Reset** stay live during a capture: deciding halfway through a side that the
limit should go is a reasonable thing to want. It is a length of *time*, so a decimated
capture still runs for as long as it says.

The default is no limit deliberately: a limit that fired in the middle of a side would be
worse than no limit at all.

### Warn below

Warn when the destination volume has less than this much *capture time* left. Ten minutes by
default; **Never** turns it off.

It is a warning and not a refusal, and it is raised once per capture rather than repeatedly.
The estimate is an estimate, and an application that declined to start because it predicted
a shortfall would sometimes be wrong in the direction that costs a session.

### Free space

How much longer this volume will hold a capture, with the byte figure after it. Hover it and
it names the folder it is talking about — that folder is in [Settings](settings.md) now, so
the row says which volume it means rather than leaving you to remember.

The time comes first because it is the question people actually have. "412 GB free" does not
tell you whether this will last the side you are about to play; "2:51:40 of capture" does.

The estimate follows the **Format** and **Sample rate** you have chosen: 40 MB/s for FLAC,
which is what one writes on average, 80 MB/s uncompressed, and half of either when
decimating. The same volume therefore holds four times as much 20 Msps FLAC as 40 Msps
uncompressed, and the readout says so.

An unknown figure means the folder does not exist yet, which is an ordinary thing to have on
the way to creating it — it is not reported as a full disk. It names the folder when it says
so, because you cannot read it off this panel.

## Status

The line at the bottom of the panel: *No capture device attached*, *Ready*, *Monitoring*,
or the full path of the file being written. The whole path, not just the name — somebody who
has just started a forty-minute capture should be able to see that it is going to the drive
they meant without leaving the application.
