#include "biquad.h"
#include "compressor.h"
#include "delay.h"
#include "fdn_reverb.h"
#include "gate.h"
#include "smoothing.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);        \
      return 1;                                                                \
    }                                                                          \
  } while (0)
int main() {
  Biquad b;
  b.configure(BiquadType::HighPass, 48000, 80);
  for (int i = 0; i < 200000; i++)
    CHECK(std::isfinite(b.process(i == 0 ? 1.0f : 0.0f)));
  b.reset();
  float dc = 0;
  for (int i = 0; i < 48000; i++)
    dc = b.process(1);
  CHECK(std::fabs(dc) < 1e-3f);
  b.set_bypass(true);
  CHECK(b.process(.123f) == .123f);
  for (auto t : {BiquadType::LowPass, BiquadType::Peaking, BiquadType::LowShelf,
                 BiquadType::HighShelf}) {
    b.configure(t, 48000, 1000, .707f, 6);
    b.reset();
    for (int i = 0; i < 10000; i++)
      CHECK(std::isfinite(b.process(i ? 0 : 1)));
  }
  SmoothedValue s;
  s.init(0, 48000, 10);
  s.set_target(1);
  float prev = 0;
  for (int i = 0; i < 48000; i++) {
    float v = s.next();
    CHECK(v >= prev && v <= 1);
    prev = v;
  }
  CHECK(prev > .999f);
  Gate g;
  g.init(48000);
  for (int i = 0; i < 10000; i++)
    CHECK(g.process(0) == 0);
  Compressor c;
  c.init(48000);
  CHECK(c.gain_db_for(-40) == 0);
  CHECK(c.gain_db_for(0) < 0);
  for (int i = 0; i < 10000; i++)
    CHECK(std::isfinite(c.process(i & 1 ? .8f : -.8f)));
  StereoDelay d;
  CHECK(d.init(48000, .01f));
  d.set_times(1, 1);
  d.set_mix(0, 1);
  d.set_feedback(0);
  float l, r;
  for (int i = 0; i < 2000; i++) {
    d.process(i == 0 ? 1 : 0, l, r);
    CHECK(std::isfinite(l) && std::isfinite(r));
  }
  d.reset();
  for (int i = 0; i < 5000; i++)
    d.process(i % 97 == 0 ? .1f : 0, l, r);
  float h[8] = {1, 0, 0, 0, 0, 0, 0, 0};
  FdnReverb::hadamard8(h);
  float energy = 0;
  for (float v : h)
    energy += v * v;
  CHECK(std::fabs(energy - 1) < 1e-5f);
  FdnReverb fdn;
  CHECK(fdn.init(48000));
  float peak = 0;
  for (int i = 0; i < 480000; i++) {
    fdn.process(i == 0 ? 1 : 0, l, r);
    CHECK(std::isfinite(l) && std::isfinite(r));
    peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
  }
  CHECK(peak < 2);
  fdn.reset();
  for (int i = 0; i < 10000; i++) {
    fdn.process(0, l, r);
    CHECK(l == 0 && r == 0);
  }
  std::puts("all DSP tests passed");
  return 0;
}
