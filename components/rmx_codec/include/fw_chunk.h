/* Firmware chunk header of wiki Mower-Protocol §12.2: a 64-byte little-endian
 * header, then the chunk's bytes. Pure C (no ESP-IDF), host-tested in test/. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FW_CHUNK_HDR_LEN  64
#define FW_CHUNK_MAX      4096   /* only the last chunk of a file is shorter */
#define FW_CHUNK_ID_HASH  8      /* first bytes of SHA-256 over the request_id */

typedef struct {
    uint32_t chunk_index;
    uint32_t chunk_count;
    uint32_t image_size;
    uint16_t chunk_len;
    uint8_t request_id_hash[FW_CHUNK_ID_HASH];
    uint8_t sha256[32];          /* of the whole file */
} fw_chunk_hdr_t;

/* True when data is one chunk for the open request: magic "RMFW", header
 * version 1, flags 1, header length 64, reserved 0, chunk_len <= 4096 and
 * exactly the bytes after the header, request-id hash equal to expect_hash.
 * Then hdr is filled and *payload points into data. False drops the frame
 * (§12.2: wrong fixed fields, bad length or another request). */
bool fw_chunk_parse(const uint8_t *data, size_t len, const uint8_t expect_hash[FW_CHUNK_ID_HASH],
                    fw_chunk_hdr_t *hdr, const uint8_t **payload);
