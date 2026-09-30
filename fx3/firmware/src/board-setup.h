/************************************************************************

    board-setup.h

    The board setup record: what the user has declared about the capture
    board this FX3 is plugged into, kept in the boot EEPROM's last page
    DomesdayDuplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

    Deliberately free of the FX3 SDK, for the same reason update-protocol.h
    is: deciding whether 64 bytes are a record worth writing is arithmetic,
    so it compiles and runs on a build machine and is tested there. The
    I2C transport is update-agent.c's, because the EEPROM is one medium
    with one driver.

    The firmware is a store for this record and not an interpreter of it.
    It checks the framing - magic, layout version, CRC-32 - and nothing
    between them: which converter is fitted, how RSEL is wired and what DC
    offset to correct are the capture application's to decide and to
    explain. A field added to the payload therefore needs no firmware
    update, only a new layout version the application understands.

    The record describes the capture board, not the FX3. The kit plugs
    into the capture board and can be moved to another one, so the record
    is a declaration the user can rewrite at any time and never something
    the firmware derives or defaults.

    The record's layout is specified on the "Board setup record" page of
    the documentation site. This header is the firmware's copy of it.

************************************************************************/

#ifndef _BOARD_SETUP_H_
#define _BOARD_SETUP_H_

#include <stdint.h>

#include "update-protocol.h"

// Vendor requests, continuing the update agent's block. Both are answered
// on a device at any link speed and whether or not a capture path is up,
// for the same reason the update requests are.
//
// BOARD_SETUP_READ is device-to-host and returns the record's page exactly
// as it is on the medium, valid or not: a blank EEPROM reads as all ones,
// and telling that apart from a record is the host's job, done with the
// same check as boardSetupRecordIsValid().
//
// BOARD_SETUP_WRITE is host-to-device with a BOARD_SETUP_LENGTH data stage.
// A record whose framing is wrong is not written. That cannot be reported
// by a stall - the data stage has been acknowledged by the time it has been
// read - so the host reads the record back afterwards and compares, which
// is also what catches a write the EEPROM did not take.
#define BOARD_SETUP_REQUEST_READ        (0xD6u)
#define BOARD_SETUP_REQUEST_WRITE       (0xD7u)

// One EEPROM page, so that a write is one page write and the record is
// never left half old and half new by an interrupted one.
#define BOARD_SETUP_LENGTH              (UPDATE_EEPROM_PAGE_SIZE)

// The last page of the EEPROM, which no firmware image may reach: the
// update agent refuses an image longer than UPDATE_EEPROM_IMAGE_CAPACITY,
// and that capacity ends exactly here.
#define BOARD_SETUP_EEPROM_ADDRESS      (UPDATE_EEPROM_IMAGE_CAPACITY)

// "DDBS", in the order the bytes appear on the medium, so the page reads as
// itself in a dump.
#define BOARD_SETUP_MAGIC_0             (0x44u)
#define BOARD_SETUP_MAGIC_1             (0x44u)
#define BOARD_SETUP_MAGIC_2             (0x42u)
#define BOARD_SETUP_MAGIC_3             (0x53u)

// Bytes 4-5 carry the layout version, little-endian. Zero is never a valid
// layout, so a page of zeros is not mistaken for a record.
#define BOARD_SETUP_VERSION_OFFSET      (4u)

// The last four bytes carry the CRC-32 of everything before them,
// little-endian - the same CRC-32 the FPGA boot block carries.
#define BOARD_SETUP_CRC_OFFSET          (BOARD_SETUP_LENGTH - 4u)

// Is this a record: the right length, the magic, a non-zero layout version
// and a CRC-32 that matches? Returns non-zero if so.
int boardSetupRecordIsValid(const uint8_t *record, uint32_t length);

#endif // _BOARD_SETUP_H_
