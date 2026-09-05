#include "fdn_reverb.h"
#include <cstdint>
#include <cstdio>

static bool write_bytes(FILE *file, const void *data, size_t size) {
  return fwrite(data, 1, size, file) == size;
}
static bool u32(FILE *file, uint32_t value) {
  return write_bytes(file, &value, sizeof(value));
}
static bool u16(FILE *file, uint16_t value) {
  return write_bytes(file, &value, sizeof(value));
}

int main() {
  constexpr uint32_t sample_rate = 48000;
  constexpr uint32_t frames = sample_rate * 5;
  constexpr uint32_t data_bytes = frames * 2 * sizeof(float);
  FdnReverb reverb;
  if (!reverb.init(sample_rate))
    return 1;
  reverb.set_wet(1);
  FILE *file = fopen("fdn_impulse.wav", "wb");
  if (!file)
    return 2;

  bool ok = write_bytes(file, "RIFF", 4) && u32(file, 48 + data_bytes) &&
            write_bytes(file, "WAVEfmt ", 8) && u32(file, 16) && u16(file, 3) &&
            u16(file, 2) && u32(file, sample_rate) &&
            u32(file, sample_rate * 2 * sizeof(float)) &&
            u16(file, 2 * sizeof(float)) && u16(file, 32) &&
            write_bytes(file, "fact", 4) && u32(file, 4) && u32(file, frames) &&
            write_bytes(file, "data", 4) && u32(file, data_bytes);
  for (uint32_t i = 0; ok && i < frames; ++i) {
    float left, right;
    reverb.process(i == 0 ? 1.0f : 0.0f, left, right);
    ok = write_bytes(file, &left, sizeof(left)) &&
         write_bytes(file, &right, sizeof(right));
  }
  if (fclose(file) != 0)
    ok = false;
  return ok ? 0 : 3;
}
