#include "crc16.h"

/* shortcut: bitwise, ~8 shifts per byte; a 256-entry table if the 921600-baud link ever shows it in a profile. */
uint16_t crc16_update(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
{
    return crc16_update(CRC16_INIT, data, len);
}
