#include "audio_i2s.h"
#include "biquad.h"
#include "compressor.h"
#include "delay.h"
#include "fdn_reverb.h"
#include "gate.h"
#include "parameter_queue.h"
#include "profiling.h"
#include "smoothing.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
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
  ParameterQueue<4> queue;
  CHECK(queue.push({VocalFxParameter::DelayWet, .1f}));
  CHECK(queue.push({VocalFxParameter::DelayWet, .2f}));
  CHECK(queue.push({VocalFxParameter::DelayWet, .3f}));
  CHECK(!queue.push({VocalFxParameter::DelayWet, .4f}));
  ParameterChange change;
  CHECK(queue.pop(change) && change.value == .1f);
  CHECK(queue.pop(change) && change.value == .2f);
  CHECK(queue.pop(change) && change.value == .3f);
  CHECK(!queue.pop(change));
  ParameterQueue<64> concurrent_queue;
  constexpr uint32_t update_count = 100000;
  std::thread producer([&] {
    for (uint32_t i = 0; i < update_count; ++i)
      while (
          !concurrent_queue.push({VocalFxParameter::CompressorRatio, (float)i}))
        std::this_thread::yield();
  });
  for (uint32_t expected = 0; expected < update_count; ++expected) {
    while (!concurrent_queue.pop(change))
      std::this_thread::yield();
    CHECK(change.value == (float)expected);
  }
  producer.join();
  using vocal_fx_platform::AudioI2s;
  CHECK(AudioI2s::float_to_pcm16(1.0f) == 32767);
  CHECK(AudioI2s::float_to_pcm16(-1.0f) == -32768);
  CHECK(std::fabs(AudioI2s::pcm16_to_float(16384) - .5f) < 1e-6f);
  CHECK((AudioI2s::float_to_pcm24(.5f) & 0xff) == 0);
  CHECK(std::fabs(AudioI2s::pcm24_to_float(AudioI2s::float_to_pcm24(.5f)) -
                  .5f) < 1e-6f);
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
  Profiler profiler;
  std::atomic<bool> writer_done{false};
  std::thread writer([&] {
    for (int i = 0; i < 10000; ++i) {
      profiler.begin(ProfileSection::Pipeline);
      profiler.end(ProfileSection::Pipeline, 1000000);
    }
    writer_done.store(true, std::memory_order_release);
  });
  uint64_t observed_calls = 0;
  while (!writer_done.load(std::memory_order_acquire)) {
    const auto snapshot = profiler.stats(ProfileSection::Pipeline);
    CHECK(snapshot.calls >= observed_calls);
    CHECK(snapshot.max_us <= snapshot.total_us);
    observed_calls = snapshot.calls;
  }
  writer.join();
  CHECK(profiler.stats(ProfileSection::Pipeline).calls == 10000);
  std::puts("all DSP tests passed");
  return 0;
}
