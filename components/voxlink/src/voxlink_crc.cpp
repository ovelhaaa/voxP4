#include "voxlink_crc.h"

namespace voxlink {

uint16_t crc16_ccitt_false_update(uint16_t crc, const uint8_t *data, size_t len) {
  if (data == nullptr)
    return crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      if (crc & 0x8000u)
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021u);
      else
        crc = static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

uint16_t crc16_ccitt_false(const uint8_t *data, size_t len) {
  return crc16_ccitt_false_update(0xFFFFu, data, len);
}

} // namespace voxlink
