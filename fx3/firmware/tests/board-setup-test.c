/************************************************************************

    board-setup-test.c

    T1 unit test for the board setup record's framing
    Domesday Duplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

    Compiled and run on the build host, not the FX3, which is possible at
    all only because board-setup.c and update-protocol.c deliberately do
    not include the Cypress SDK.

    What is covered here is the one decision the firmware makes about the
    record - is this a record, or is it something that must not be
    written - and where it lives. Whether the EEPROM takes the page is the
    I2C transport's business, and has no coverage off a board.

************************************************************************/

#include <stdio.h>
#include <string.h>

#include "board-setup.h"

static int failures = 0;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void checkNumber(unsigned long got, unsigned long want, const char *what)
{
    if (got != want) {
        printf("FAIL: %s: got %lu, expected %lu\n", what, got, want);
        failures++;
    }
}

static void sealRecord(uint8_t *record)
{
    const uint32_t crc = updateCrc32Final(
        updateCrc32Update(UPDATE_CRC32_INITIAL, record, BOARD_SETUP_CRC_OFFSET));

    record[BOARD_SETUP_CRC_OFFSET] = (uint8_t)(crc & 0xFFu);
    record[BOARD_SETUP_CRC_OFFSET + 1u] = (uint8_t)((crc >> 8) & 0xFFu);
    record[BOARD_SETUP_CRC_OFFSET + 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    record[BOARD_SETUP_CRC_OFFSET + 3u] = (uint8_t)((crc >> 24) & 0xFFu);
}

// A layout 1 record with an arbitrary payload, sealed.
static void makeRecord(uint8_t *record)
{
    uint32_t index;

    memset(record, 0, BOARD_SETUP_LENGTH);
    record[0] = 'D';
    record[1] = 'D';
    record[2] = 'B';
    record[3] = 'S';
    record[BOARD_SETUP_VERSION_OFFSET] = 1u;

    for (index = 6u; index < BOARD_SETUP_CRC_OFFSET; index++) {
        record[index] = (uint8_t)(index * 7u);
    }

    sealRecord(record);
}

static void testPlacement(void)
{
    // The record is the EEPROM's last page, page-aligned, so that it is
    // always one page write and never shares a page with an image.
    checkNumber(BOARD_SETUP_LENGTH, UPDATE_EEPROM_PAGE_SIZE, "the record is one page");
    checkNumber(BOARD_SETUP_EEPROM_ADDRESS + BOARD_SETUP_LENGTH, UPDATE_EEPROM_SIZE,
                "the record ends where the EEPROM does");
    checkNumber(BOARD_SETUP_EEPROM_ADDRESS % UPDATE_EEPROM_PAGE_SIZE, 0u,
                "the record starts on a page boundary");
    checkNumber(updateEepromWriteSpan(BOARD_SETUP_EEPROM_ADDRESS, BOARD_SETUP_LENGTH),
                BOARD_SETUP_LENGTH, "the whole record goes in one page write");

    // An image padded to a whole page stops short of the record however
    // long it is allowed to be.
    check(updateEepromPadToPage(UPDATE_EEPROM_IMAGE_CAPACITY) <= BOARD_SETUP_EEPROM_ADDRESS,
          "the largest admissible image, padded, stops short of the record");
    check(updateEepromPadToPage(UPDATE_EEPROM_IMAGE_CAPACITY - 1u) <= BOARD_SETUP_EEPROM_ADDRESS,
          "an image a byte short of capacity, padded, stops short of the record");
}

static void testValidRecord(void)
{
    uint8_t record[BOARD_SETUP_LENGTH];

    makeRecord(record);
    check(boardSetupRecordIsValid(record, BOARD_SETUP_LENGTH),
          "a sealed record is valid");

    // The firmware does not interpret the payload, so a later layout is as
    // good as the first one.
    record[BOARD_SETUP_VERSION_OFFSET] = 7u;
    sealRecord(record);
    check(boardSetupRecordIsValid(record, BOARD_SETUP_LENGTH),
          "a record of a later layout version is valid");
}

// A record exactly as the capture application encodes it, copied from
// ddd-gui/tests/unit/test_board_setup.cpp, where the same bytes are pinned
// against the encoder. Two programs on two processors have to agree about
// these 64 bytes, and this is where that agreement is checked from the
// firmware's side.
static void testApplicationRecord(void)
{
    static const uint8_t record[BOARD_SETUP_LENGTH] = {
        0x44u, 0x44u, 0x42u, 0x53u, 0x01u, 0x00u, 0x01u, 0x00u,
        0x28u, 0x00u, 0x34u, 0x00u, 0x3Fu, 0x00u, 0x47u, 0x00u,
        0x50u, 0x00u, 0x5Cu, 0x00u, 0x65u, 0x00u, 0x6Fu, 0x00u,
        0xFEu, 0xFFu, 0x04u, 0x00u, 0x09u, 0x00u, 0x0Eu, 0x00u,
        0x13u, 0x00u, 0x18u, 0x00u, 0x1Du, 0x00u, 0x22u, 0x00u,
        0x80u, 0x3Bu, 0xB1u, 0x6Au, 0x42u, 0x65u, 0x6Eu, 0x63u,
        0x68u, 0x20u, 0x23u, 0x32u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0xBCu, 0x92u, 0x87u, 0x67u,
    };

    check(boardSetupRecordIsValid(record, BOARD_SETUP_LENGTH),
          "a record the capture application encodes is accepted");
}

static void testRefusals(void)
{
    uint8_t record[BOARD_SETUP_LENGTH];
    uint32_t index;

    check(!boardSetupRecordIsValid(NULL, BOARD_SETUP_LENGTH), "no buffer is not a record");

    makeRecord(record);
    check(!boardSetupRecordIsValid(record, BOARD_SETUP_LENGTH - 1u),
          "a short data stage is not a record");
    check(!boardSetupRecordIsValid(record, BOARD_SETUP_LENGTH + 1u),
          "a long data stage is not a record");

    // What an EEPROM that has never held a record reads as.
    memset(record, 0xFF, sizeof(record));
    check(!boardSetupRecordIsValid(record, BOARD_SETUP_LENGTH), "an erased page is not a record");

    memset(record, 0x00, sizeof(record));
    check(!boardSetupRecordIsValid(record, BOARD_SETUP_LENGTH), "a zeroed page is not a record");

    makeRecord(record);
    record[3] = 'B';
    sealRecord(record);
    check(!boardSetupRecordIsValid(record, BOARD_SETUP_LENGTH),
          "a page with the wrong magic is not a record, even sealed");

    makeRecord(record);
    record[BOARD_SETUP_VERSION_OFFSET] = 0u;
    sealRecord(record);
    check(!boardSetupRecordIsValid(record, BOARD_SETUP_LENGTH),
          "layout version zero is not a record, even sealed");

    // Every byte the CRC covers is covered: a single flipped bit anywhere
    // before it is caught.
    for (index = 0u; index < BOARD_SETUP_CRC_OFFSET; index++) {
        makeRecord(record);
        record[index] ^= 0x10u;
        if (boardSetupRecordIsValid(record, BOARD_SETUP_LENGTH)) {
            printf("FAIL: a flipped bit at byte %lu was not caught\n", (unsigned long)index);
            failures++;
        }
    }

    makeRecord(record);
    record[BOARD_SETUP_CRC_OFFSET + 3u] ^= 0x01u;
    check(!boardSetupRecordIsValid(record, BOARD_SETUP_LENGTH),
          "a damaged checksum is not a record");
}

int main(void)
{
    testPlacement();
    testValidRecord();
    testApplicationRecord();
    testRefusals();

    if (failures != 0) {
        printf("board-setup-test: FAIL (%d failures)\n", failures);
        return 1;
    }

    printf("board-setup-test: PASS\n");
    return 0;
}
