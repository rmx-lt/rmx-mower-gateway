/* CRC-16/CCITT-FALSE (wiki Mower-Protocol §14.1): polynomial 0x1021, initial
 * value 0xFFFF, no reflection, no final XOR. Check value of "123456789": 0x29B1. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define CRC16_INIT 0xFFFF

/* Continues crc over len bytes; start with CRC16_INIT. */
uint16_t crc16_update(uint16_t crc, const uint8_t *data, size_t len);
uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);
