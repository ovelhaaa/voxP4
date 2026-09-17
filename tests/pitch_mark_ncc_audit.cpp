#include "pitch_mark_ncc.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kRate = 48000.0f;
constexpr size_t kRing = 16384;

struct Fixture { std::string name; std::vector<float> samples; int period; };
struct Summary {
  uint64_t searches = 0, score_bit_mismatches = 0, best_mismatches = 0;
  uint64_t accept_mismatches = 0, mark_mismatches = 0;
  uint64_t confidence_bit_mismatches = 0;
  uint64_t coherent_mismatches = 0, state_mismatches = 0, nonfinite = 0;
  double max_score_abs = 0.0, max_score_rel = 0.0;
  double max_confidence_abs = 0.0, max_confidence_rel = 0.0;
  double max_bb_abs = 0.0, max_bb_rel = 0.0;
  uint8_t oracle_coherent = 1, candidate_coherent = 1;
  uint8_t oracle_failures = 0, candidate_failures = 0;
  uint8_t oracle_state = 1, candidate_state = 1; // acquiring
};

uint32_t bits(float x) { uint32_t b; std::memcpy(&b, &x, sizeof(b)); return b; }

std::vector<float> tone(float hz, bool harmonic, float noise) {
  std::vector<float> x(kRing);
  std::mt19937 rng(0xB4B4FU + static_cast<uint32_t>(hz));
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  for (size_t i = 0; i < x.size(); ++i) {
    const float p = 2.0f * kPi * hz * static_cast<float>(i) / kRate;
    x[i] = 0.6f * std::sin(p);
    if (harmonic)
      x[i] += 0.23f * std::sin(2.0f * p + .2f) +
              0.11f * std::sin(3.0f * p - .4f);
    x[i] += noise * dist(rng);
  }
  return x;
}

std::vector<float> read_wav(const fs::path &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
  if (bytes.size() < 44) return {};
  auto u16 = [&](size_t p) { return static_cast<uint16_t>(bytes[p] | bytes[p+1] << 8); };
  auto u32 = [&](size_t p) { return static_cast<uint32_t>(bytes[p] | bytes[p+1] << 8 |
      bytes[p+2] << 16 | bytes[p+3] << 24); };
  size_t p = 12, data = 0, data_size = 0; uint16_t format = 1, channels = 1, depth = 16;
  while (p + 8 <= bytes.size()) {
    const uint32_t size = u32(p + 4);
    if (p + 8 + size > bytes.size()) break;
    const std::string id(reinterpret_cast<const char *>(&bytes[p]), 4);
    if (id == "fmt " && size >= 16) { format = u16(p+8); channels = u16(p+10); depth = u16(p+22); }
    if (id == "data") { data = p + 8; data_size = size; break; }
    p += 8 + size + (size & 1U);
  }
  std::vector<float> out;
  const size_t width = depth / 8;
  if (!data || !width || !channels) return out;
  const size_t frames = std::min(kRing, data_size / (width * channels));
  out.resize(kRing);
  for (size_t i = 0; i < frames; ++i) {
    const size_t q = data + i * width * channels;
    if (format == 3 && depth == 32) std::memcpy(&out[i], &bytes[q], 4);
    else if (depth == 16) out[i] = static_cast<int16_t>(u16(q)) / 32768.0f;
    else if (depth == 24) {
      int32_t v = bytes[q] | bytes[q+1] << 8 | bytes[q+2] << 16;
      if (v & 0x800000) v |= ~0xffffff;
      out[i] = v / 8388608.0f;
    }
  }
  if (frames < kRing) out.resize(frames);
  return out;
}

std::array<PitchMarkNccVariant, 8> variants() {
  return {PitchMarkNccVariant::Reference, PitchMarkNccVariant::ReuseAa,
          PitchMarkNccVariant::Fma4AccDot, PitchMarkNccVariant::Fma8AccDot,
          PitchMarkNccVariant::Fma8SlidingBb,
          PitchMarkNccVariant::LinearScratch,
          PitchMarkNccVariant::ContiguousMulti4,
          PitchMarkNccVariant::ContiguousMulti8};
}

void compare_search(const Fixture &fixture, PitchMarkNccVariant variant,
                    uint64_t previous, Summary &s) {
  const int radius = std::max(2, fixture.period / 5);
  const int window = std::max(4, fixture.period / 2);
  const uint64_t predicted = previous + fixture.period;
  std::array<float, 2048> oracle_scores{}, candidate_scores{};
  std::array<float, kPitchMarkNccScratchCapacity> scratch{};
  const auto oracle = pitch_mark_ncc_search(
      PitchMarkNccVariant::Reference, fixture.samples.data(), fixture.samples.size(),
      previous, predicted, radius, window, nullptr, 0, oracle_scores.data(),
      oracle_scores.size());
  const auto candidate = pitch_mark_ncc_search(
      variant, fixture.samples.data(), fixture.samples.size(), previous, predicted,
      radius, window, scratch.data(), scratch.size(), candidate_scores.data(),
      candidate_scores.size());
  ++s.searches;
  for (int i = 0; i < 2 * radius + 1; ++i) {
    const float a = oracle_scores[i], b = candidate_scores[i];
    if (!std::isfinite(b)) ++s.nonfinite;
    if (bits(a) != bits(b)) ++s.score_bit_mismatches;
    const double ae = std::fabs(static_cast<double>(a) - b);
    s.max_score_abs = std::max(s.max_score_abs, ae);
    s.max_score_rel = std::max(s.max_score_rel, ae / std::max(1e-30, std::fabs(static_cast<double>(a))));
  }
  if (oracle.best_offset != candidate.best_offset) ++s.best_mismatches;
  const bool oa = oracle.best_score > .35f, ca = candidate.best_score > .35f;
  if (oa != ca) ++s.accept_mismatches;
  if (oa && ca && predicted + oracle.best_offset != predicted + candidate.best_offset)
    ++s.mark_mismatches;
  if (oa && ca) {
    const auto confidence = [radius](const PitchMarkNccResult &result) {
      const float distance = 1.0f -
          std::fabs(static_cast<float>(result.best_offset)) / (radius + 1);
      return std::clamp(.5f * .9f + .35f * std::max(result.best_score, 0.0f) +
                            .15f * distance,
                        0.0f, 1.0f);
    };
    const float oracle_confidence = confidence(oracle);
    const float candidate_confidence = confidence(candidate);
    if (bits(oracle_confidence) != bits(candidate_confidence))
      ++s.confidence_bit_mismatches;
    const double ae = std::fabs(static_cast<double>(oracle_confidence) -
                                candidate_confidence);
    s.max_confidence_abs = std::max(s.max_confidence_abs, ae);
    s.max_confidence_rel = std::max(
        s.max_confidence_rel,
        ae / std::max(1e-30, std::fabs(static_cast<double>(oracle_confidence))));
  }

  const auto advance_state = [](bool accepted, uint8_t &coherent,
                                uint8_t &failures, uint8_t &state) {
    if (accepted) {
      coherent = static_cast<uint8_t>(std::min<int>(255, coherent + 1));
      failures = 0;
      if (coherent >= 3) state = 2; // locked
    } else if (++failures >= 3) {
      coherent = 0;
      failures = 0;
      state = 0; // unlocked
    }
  };
  advance_state(oa, s.oracle_coherent, s.oracle_failures, s.oracle_state);
  advance_state(ca, s.candidate_coherent, s.candidate_failures,
                s.candidate_state);
  if (s.oracle_coherent != s.candidate_coherent) ++s.coherent_mismatches;
  if (s.oracle_state != s.candidate_state) ++s.state_mismatches;

  // Audit the recursive energy independently of the NCC score.  Reinitialize
  // at the first candidate exactly as the production kernel does, then compare
  // every recurrence result with a freshly accumulated scalar BB oracle.
  if (variant == PitchMarkNccVariant::Fma8SlidingBb) {
    const size_t mask = fixture.samples.size() - 1;
    float sliding_bb = 0.0f;
    for (int offset = -radius; offset <= radius; ++offset) {
      const uint64_t b_start = predicted + offset - window;
      float oracle_bb = 0.0f;
      for (int i = 0; i < window; ++i) {
        const float b = fixture.samples[(b_start + i) & mask];
        oracle_bb += b * b;
      }
      if (offset == -radius) {
        sliding_bb = oracle_bb;
      } else {
        const float leaving = fixture.samples[(b_start - 1) & mask];
        const float entering = fixture.samples[(b_start + window - 1) & mask];
        sliding_bb = sliding_bb - leaving * leaving + entering * entering;
      }
      const double ae = std::fabs(static_cast<double>(oracle_bb) - sliding_bb);
      s.max_bb_abs = std::max(s.max_bb_abs, ae);
      s.max_bb_rel = std::max(
          s.max_bb_rel,
          ae / std::max(1e-30, std::fabs(static_cast<double>(oracle_bb))));
    }
  }
}

int main(int argc, char **argv) {
  fs::path root = VOXP4_SOURCE_DIR;
  const bool verify_only = argc > 1 && std::string(argv[1]) == "--verify";
  const bool equivalence_only =
      argc > 1 && std::string(argv[1]) == "--equivalence-only";
  fs::path out = verify_only
                     ? fs::temp_directory_path() / "voxp4_b4b4f_verify"
                     : (argc > (equivalence_only ? 2 : 1)
                            ? fs::path(argv[equivalence_only ? 2 : 1])
                                 : root / "artifacts/alpha01b");
  fs::create_directories(out);
  std::vector<Fixture> fixtures;
  for (float hz : {80,110,147,220,330,440,700,900})
    fixtures.push_back({"tone_" + std::to_string(static_cast<int>(hz)), tone(hz, false, 0),
                        static_cast<int>(std::lround(kRate / hz))});
  fixtures.push_back({"harmonic_synthetic", tone(147, true, 0), 327});
  fixtures.push_back({"deterministic_noise", tone(220, false, 1.0f), 218});
  fixtures.push_back({"tone_plus_noise", tone(220, true, .08f), 218});
  auto vocal = read_wav(root / "samples/dry-acapella-leave-this-place_95bpm.wav");
  if (!vocal.empty()) fixtures.push_back({"repository_vocal_wav", std::move(vocal), 218});

  std::ofstream eq(out / "b4b4f_pitch_mark_equivalence.csv");
  eq << "scope,fixture,variant,searches,score_bit_mismatches,max_score_abs_error,max_score_relative_error,max_bb_abs_error,max_bb_relative_error,best_offset_mismatches,accept_reject_mismatches,mark_position_mismatches,mark_confidence_bit_mismatches,max_mark_confidence_abs_error,max_mark_confidence_relative_error,coherent_mark_mismatches,track_state_mismatches,nan_inf_regressions,eligible\n";
  bool guardrails = true;
  for (const auto &fixture : fixtures) {
    for (auto variant : variants()) {
      Summary s;
      for (uint64_t shift = 0; shift < 64; ++shift)
        compare_search(fixture, variant, 4096 + shift * 7, s);
      const bool eligible = s.best_mismatches == 0 && s.accept_mismatches == 0 &&
          s.mark_mismatches == 0 && s.coherent_mismatches == 0 &&
          s.state_mismatches == 0 && s.nonfinite == 0;
      if (variant == PitchMarkNccVariant::ReuseAa ||
          variant == PitchMarkNccVariant::LinearScratch ||
          variant == PitchMarkNccVariant::ContiguousMulti4 ||
          variant == PitchMarkNccVariant::ContiguousMulti8)
        guardrails &= eligible && s.score_bit_mismatches == 0;
      eq << "fixture," << fixture.name << ',' << pitch_mark_ncc_variant_name(variant)
         << ',' << s.searches << ',' << s.score_bit_mismatches << ','
         << s.max_score_abs << ',' << s.max_score_rel << ',' << s.max_bb_abs
         << ',' << s.max_bb_rel << ',' << s.best_mismatches
         << ',' << s.accept_mismatches << ',' << s.mark_mismatches << ','
         << s.confidence_bit_mismatches << ',' << s.max_confidence_abs << ','
         << s.max_confidence_rel << ','
         << s.coherent_mismatches << ',' << s.state_mismatches << ',' << s.nonfinite
         << ',' << (eligible ? "yes" : "no") << '\n';
    }
  }

  // Ten minutes at the required 200-hop/s cadence. Each search starts from
  // ring data, so this specifically checks for absence of recursive drift.
  Fixture long_fixture{"deterministic_10min", tone(220, true, .03f), 218};
  for (auto variant : {PitchMarkNccVariant::ReuseAa,
                       PitchMarkNccVariant::Fma4AccDot,
                       PitchMarkNccVariant::Fma8AccDot,
                       PitchMarkNccVariant::Fma8SlidingBb,
                       PitchMarkNccVariant::LinearScratch,
                       PitchMarkNccVariant::ContiguousMulti4,
                       PitchMarkNccVariant::ContiguousMulti8}) {
    Summary s;
    for (uint64_t hop = 0; hop < 120000; ++hop)
      compare_search(long_fixture, variant, 4096 + (hop % 2048), s);
    const bool eligible = !s.best_mismatches && !s.accept_mismatches &&
        !s.mark_mismatches && !s.coherent_mismatches && !s.state_mismatches && !s.nonfinite;
    guardrails &= (variant != PitchMarkNccVariant::ReuseAa &&
                   variant != PitchMarkNccVariant::ContiguousMulti4 &&
                   variant != PitchMarkNccVariant::ContiguousMulti8) ||
                  (eligible && s.score_bit_mismatches == 0);
    eq << "long_duration," << long_fixture.name << ','
       << pitch_mark_ncc_variant_name(variant) << ',' << s.searches << ','
       << s.score_bit_mismatches << ',' << s.max_score_abs << ',' << s.max_score_rel
       << ',' << s.max_bb_abs << ',' << s.max_bb_rel << ',' << s.best_mismatches
       << ',' << s.accept_mismatches << ','
       << s.mark_mismatches << ',' << s.confidence_bit_mismatches << ','
       << s.max_confidence_abs << ',' << s.max_confidence_rel << ','
       << s.coherent_mismatches << ','
       << s.state_mismatches << ',' << s.nonfinite << ','
       << (eligible ? "yes" : "no") << '\n';
  }

  if (equivalence_only) {
    std::cout << "B4B.4F host NCC audit: " << (guardrails ? "PASS" : "FAIL") << '\n';
    return guardrails ? 0 : 1;
  }

  std::ofstream bench(out / "b4b4f_pitch_mark_benchmark.csv");
  bench << "platform,variant,searches,window,offsets,sample_pairs,ns_per_search,ns_per_offset,ns_per_pair,checksum\n";
  const Fixture &b = fixtures[3];
  volatile float checksum = 0.0f;
  for (auto variant : variants()) {
    std::array<float, kPitchMarkNccScratchCapacity> scratch{};
    constexpr int repetitions = 2000;
    const auto start = std::chrono::steady_clock::now();
    PitchMarkNccResult last{};
    for (int i = 0; i < repetitions; ++i) {
      last = pitch_mark_ncc_search(variant, b.samples.data(), b.samples.size(),
          4096 + (i & 1023), 4096 + (i & 1023) + b.period,
          std::max(2, b.period / 5), std::max(4, b.period / 2), scratch.data(),
          scratch.size());
      checksum = checksum + last.best_score;
    }
    const double ns = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - start).count() / repetitions;
    bench << "host," << pitch_mark_ncc_variant_name(variant) << ',' << repetitions
          << ',' << b.period / 2 << ',' << last.offsets << ',' << last.sample_pairs
          << ',' << ns << ',' << ns / last.offsets << ',' << ns / last.sample_pairs
          << ',' << checksum << '\n';
  }
  std::cout << "B4B.4F host NCC audit: " << (guardrails ? "PASS" : "FAIL") << '\n';
  return guardrails ? 0 : 1;
}
