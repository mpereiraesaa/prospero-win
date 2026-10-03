/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef LAPY_ELEVATION_PROTOCOL_H
#define LAPY_ELEVATION_PROTOCOL_H

#include <stdint.h>

#define LAPY_ELEVATION_MAGIC UINT32_C(0x31564c45) /* ELV1 */
#define LAPY_ELEVATION_VERSION UINT16_C(1)

enum lapy_elevation_kind {
    LAPY_ELEVATION_REQUEST = 1,
    LAPY_ELEVATION_PREPARE = 2,
    LAPY_ELEVATION_PREPARED = 3,
    LAPY_ELEVATION_RESPONSE = 4
};

enum lapy_elevation_capability {
    LAPY_ELEVATION_FILESYSTEM = 1
};

enum lapy_elevation_status {
    LAPY_ELEVATION_OK = 0,
    LAPY_ELEVATION_INVALID_REQUEST = 1,
    LAPY_ELEVATION_UNSUPPORTED_VERSION = 2,
    LAPY_ELEVATION_UNSUPPORTED_CAPABILITY = 3,
    LAPY_ELEVATION_TARGET_MISMATCH = 4,
    LAPY_ELEVATION_UNAVAILABLE = 5,
    LAPY_ELEVATION_PREPARE_FAILED = 6,
    LAPY_ELEVATION_APPLY_FAILED = 7,
    LAPY_ELEVATION_ROLLBACK_FAILED = 8,
    LAPY_ELEVATION_TRANSPORT_ERROR = 9,
    LAPY_ELEVATION_PROTOCOL_ERROR = 10
};

/* Fixed-width little-endian wire format shared with the Prospero Win client. */
struct lapy_elevation_message {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t kind;
    uint32_t capability;
    uint32_t pid;
    uint32_t status;
};

_Static_assert(sizeof(struct lapy_elevation_message) == 24,
               "Lapy elevation wire ABI mismatch");

#endif
