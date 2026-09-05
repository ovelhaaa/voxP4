#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
enum class ProfileSection : uint8_t {
  Input,
  Compressor,
  Delay,
  Reverb,
  Pipeline,
  Count
};
struct ProfileStats {
  uint64_t calls = 0, total_us = 0, max_us = 0, deadline_misses = 0;
};
class Profiler {
public:
  static uint64_t now_us();
  void begin(ProfileSection s);
  void end(ProfileSection s, uint64_t deadline_us = 0);
  ProfileStats stats(ProfileSection s) const;
  void reset();

private:
  std::array<uint64_t, (size_t)ProfileSection::Count> start_{};
  std::array<ProfileStats, (size_t)ProfileSection::Count> stats_{};
};
#if VOCAL_FX_ENABLE_PROFILING
#define VF_PROFILE_BEGIN(p, s) (p).begin(s)
#define VF_PROFILE_END(p, s, d) (p).end(s, d)
#else
#define VF_PROFILE_BEGIN(p, s) ((void)0)
#define VF_PROFILE_END(p, s, d) ((void)0)
#endif
