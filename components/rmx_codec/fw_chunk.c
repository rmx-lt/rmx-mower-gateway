#include "fw_chunk.h"

#include <string.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool fw_chunk_parse(const uint8_t *data, size_t len, const uint8_t expect_hash[FW_CHUNK_ID_HASH],
                    fw_chunk_hdr_t *hdr, const uint8_t **payload)
{
    if (!data || len < FW_CHUNK_HDR_LEN) {
        return false;
    }
    if (memcmp(data, "RMFW", 4) != 0 || data[4] != 1 || data[5] != 0x01 ||
            le16(data + 6) != FW_CHUNK_HDR_LEN || le16(data + 22) != 0) {
        return false;
    }
    uint16_t chunk_len = le16(data + 20);
    if (chunk_len > FW_CHUNK_MAX || len != FW_CHUNK_HDR_LEN + (size_t)chunk_len) {
        return false;
    }
    if (memcmp(data + 24, expect_hash, FW_CHUNK_ID_HASH) != 0) {
        return false;
    }
    hdr->chunk_index = le32(data + 8);
    hdr->chunk_count = le32(data + 12);
    hdr->image_size = le32(data + 16);
    hdr->chunk_len = chunk_len;
    memcpy(hdr->request_id_hash, data + 24, FW_CHUNK_ID_HASH);
    memcpy(hdr->sha256, data + 32, sizeof(hdr->sha256));
    *payload = data + FW_CHUNK_HDR_LEN;
    return true;
}
