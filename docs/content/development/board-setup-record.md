# Board setup record

What the user has declared about the capture board — the converter fitted, how its RSEL pin
is wired, the DC offset to correct and a name — is kept by the FX3 firmware in the last page
of its boot EEPROM. This page is the specification of that record and of the two requests that
reach it. The user's side of it is the [Board setup](../capture-gui/board-setup.md) page.

!!! note "Normative"

    The firmware (`fx3/firmware/src/board-setup.h`) and the capture application
    (`ddd-gui/src/capture/board_setup.h`, `wire_protocol.h`) each carry a copy of what is
    below, as AGENTS.md §2 requires of a wire protocol. A change here is a change to both.

## Why on the device

A declaration kept by the application would follow the computer. Kept on the FX3 kit it follows
the kit — to another computer, or beside another Duplicator on the same one. The kit is itself a
separate board that plugs into the capture board, so the record describes whichever capture
board the kit was on when it was declared; the name is what makes a kit moved to another board
noticeable.

The firmware has no USB serial number, so the application has nothing else by which to tell
two boards apart. The record is the only per-board state there is.

## Where

The EEPROM's last page: byte address `0x3FFC0` on the 2 Mbit M24M02, slave `0xA6`, offset
`0xFFC0`. One page, so that a write is a single page write and the record is never left half
old and half new.

No firmware image reaches it. The update agent refuses a target 0 payload longer than
`UPDATE_EEPROM_IMAGE_CAPACITY` — the EEPROM less this page — and an image padded to a whole page
at that length stops exactly at `0x3FFC0`. No image comes within a hundred kilobytes of it.
`tools/blank-board.sh` erases the whole EEPROM, record included, which is right for a board
being taken back to nothing.

## The requests

| Request | `bmRequestType` | Direction | Data stage | Purpose |
| --- | --- | --- | --- | --- |
| `0xD6` `BOARD_SETUP_READ` | `0xC0` | IN | 64 bytes | The page, exactly as it is on the medium |
| `0xD7` `BOARD_SETUP_WRITE` | `0x40` | OUT | 64 bytes | A whole record, written and read back by the firmware |

Both are answered at any link speed and whether or not the capture path is up, like the update
requests. Both stall while an update is in progress, since the verification may be holding the
I2C bus, and when the I2C block never came up. Firmware older than the record stalls both, which
is how the application recognises it.

The read returns the page whatever it holds: an EEPROM nothing has declared on reads as all
ones, and telling that from a record is the host's job.

The write is refused by stalling where it can still be — a data stage of the wrong length, or
an update in progress. Once the data stage has been read the transfer can only be acknowledged,
so a record whose framing is wrong is simply not written, and **the host confirms every write
by reading the page back**. That readback is also what catches a write the EEPROM did not keep.

The firmware checks the framing and nothing else. It stores the record rather than
interpreting it, so a field added in a later layout needs no firmware change.

## Layout

64 bytes. Multi-byte fields are little-endian except the magic. The DC offset is held per ADC
rate as well as per input range, because on real boards the offset moves with the clock.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Magic `DDBS` (`0x44 0x44 0x42 0x53`), in that byte order so the page reads as itself in a dump |
| 4 | 2 | Layout version, `1`. Zero is never valid, so a page of zeros is not a record |
| 6 | 1 | Converter: `0` ADS825, `1` ADS828 |
| 7 | 1 | RSEL wiring: `0` auto (routed to the FPGA), `1` tied low (1Vpp), `2` tied high (2Vpp) |
| 8 | 16 | DC offsets at 1Vpp: eight signed 16-bit values, in converter codes, −512 to +511, for 40, 45, 50, 55, 60, 65, 70 and 75 MSPS in that order — every `PLL_PRESET` the gateware implements |
| 24 | 16 | DC offsets at 2Vpp, the same |
| 40 | 4 | When the offsets were last measured, seconds since the Unix epoch; `0` when any was entered by hand |
| 44 | 16 | Board name, UTF-8, padded with zeros; never cut inside a character |
| 60 | 4 | CRC-32 of bytes 0–59 |

The CRC-32 is the reflected one — polynomial `0xEDB88320`, initial value all ones, final
complement — which is the one the [FPGA boot block](epcs-layout-and-boot-flow.md) carries, and
its check value over `123456789` is `0xCBF43926`.

The firmware accepts a page when the length is 64, the magic matches, the layout version is not
zero and the CRC matches. The application additionally refuses, as damaged, a record whose
converter or wiring is not one of the values above or whose offsets are out of range, and treats
a layout later than 1 as written by a newer application — recognised, and not read.

### When nothing has been declared

A page without the magic is not a record: the device has never been declared on, and the
application applies the defaults — ADS825, RSEL tied high, no offset. The same defaults apply to
a damaged record, a newer layout, and a device whose firmware cannot store one.

## What the application does with it

- **ADC rate.** The rates offered stop at the lower of the gateware's `MAX_ADC_RATE_MHZ` and
  the declared converter's rating: 40 for the ADS825, 75 for the ADS828.
- **Input range.** With RSEL auto the capture chooses; tied low or high, the capture runs at the
  wired range, and that is what is written to the `RANGE_SELECT` register and recorded.
- **DC offset.** The declared offset for the rate and range in use — a gateware with no rate
  presets counting as 40 MSPS — is taken out of every sample written:
  `(code − 512 − offset) × 64`, saturated to the 16-bit range rather than wrapped. The offset is
  in whole codes so the six low bits stay zero. It is recorded as `DDD_DC_OFFSET` and as
  `dc_offset` in the metadata. Test mode is never corrected.
- **Samples the correction pushed out of range** that the converter had not clipped are counted,
  and any at all raise a warning: with an offset measured on the board, an AC-coupled signal
  cannot reach the far end of the corrected range without the converter clipping first.

## Where the code is

| File | Holds |
| --- | --- |
| `fx3/firmware/src/board-setup.h` | Request numbers, placement and framing check; host-tested by `fx3/firmware/tests/board-setup-test.c` |
| `fx3/firmware/src/update-agent.c` | The page read and write over the EEPROM's I2C transport |
| `fx3/firmware/src/domesday-duplicator.c` | The two requests' dispatch |
| `ddd-gui/src/capture/board_setup.h` | The declaration, the record's encoder and decoder, and reading and writing it on a device |
| `ddd-gui/src/capture/dc_offset_measurement.h` | The measurement arithmetic and the quiet-input check |
| `ddd-gui/src/capture/sample_format.h` | `ToCorrectedSigned16Bit` and `DcOffsetSaturates` |
| `ddd-gui/src/gui/board_setup_page.h` | The Board setup tab |

The same 64-byte record is pinned in both test suites — encoded by the application in
`test_board_setup.cpp` and accepted by the firmware in `board-setup-test.c` — so the two copies
of the layout cannot drift apart unnoticed.
