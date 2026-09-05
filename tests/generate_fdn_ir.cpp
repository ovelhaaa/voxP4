#include "fdn_reverb.h"
#include <cstdint>
#include <cstdio>
static void u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void u16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
int main() {
  constexpr uint32_t sr = 48000, n = sr * 5;
  FdnReverb r;
  if (!r.init(sr))
    return 1;
  r.set_wet(1);
  FILE *f = fopen("fdn_impulse.wav", "wb");
  if (!f)
    return 2;
  fwrite("RIFF", 1, 4, f);
  u32(f, 36 + n * 8);
  fwrite("WAVEfmt ", 1, 8, f);
  u32(f, 16);
  u16(f, 3);
  u16(f, 2);
  u32(f, sr);
  u32(f, sr * 8);
  u16(f, 8);
  u16(f, 32);
  fwrite("data", 1, 4, f);
  u32(f, n * 8);
  for (uint32_t i = 0; i < n; i++) {
    float l, rr;
    r.process(i ? 0 : 1, l, rr);
    fwrite(&l, 4, 1, f);
    fwrite(&rr, 4, 1, f);
  }
  fclose(f);
  return 0;
}
