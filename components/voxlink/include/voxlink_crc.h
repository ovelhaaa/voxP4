#pragma once
// CRC-16/CCITT-FALSE as locked by VoxLink v1:
//   polynomial = 0x1021 (normal, MSB-first)
//   init       = 0xFFFF
//   refin      = false
//   refout     = false
//   xorout     = 0x0000
// Check value for ASCII "123456789" is 0x29B1.
#include <cstddef>
#include <cstdint>

namespace voxlink {

uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);
uint16_t crc16_ccitt_false_update(uint16_t crc, const uint8_t *data, size_t len);

} // namespace voxlink
