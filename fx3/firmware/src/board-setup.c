/************************************************************************

    board-setup.c

    The board setup record, and the decisions about it that need no
    hardware
    DomesdayDuplicator - LaserDisc RF sampler
    SPDX-FileCopyrightText: 2026 Simon Inns
    SPDX-License-Identifier: GPL-3.0-or-later

************************************************************************/

#include <stddef.h>

#include "board-setup.h"

int boardSetupRecordIsValid(const uint8_t *record, uint32_t length)
{
    uint32_t stored;
    uint32_t computed;
    uint16_t version;

    if (record == NULL) return 0;
    if (length != BOARD_SETUP_LENGTH) return 0;

    if (record[0] != BOARD_SETUP_MAGIC_0 || record[1] != BOARD_SETUP_MAGIC_1 ||
        record[2] != BOARD_SETUP_MAGIC_2 || record[3] != BOARD_SETUP_MAGIC_3) {
        return 0;
    }

    version = (uint16_t)((uint16_t)record[BOARD_SETUP_VERSION_OFFSET] |
                         ((uint16_t)record[BOARD_SETUP_VERSION_OFFSET + 1u] << 8));
    if (version == 0u) return 0;

    stored = (uint32_t)record[BOARD_SETUP_CRC_OFFSET] |
             ((uint32_t)record[BOARD_SETUP_CRC_OFFSET + 1u] << 8) |
             ((uint32_t)record[BOARD_SETUP_CRC_OFFSET + 2u] << 16) |
             ((uint32_t)record[BOARD_SETUP_CRC_OFFSET + 3u] << 24);

    computed = updateCrc32Final(
        updateCrc32Update(UPDATE_CRC32_INITIAL, record, BOARD_SETUP_CRC_OFFSET));

    return (stored == computed) ? 1 : 0;
}
