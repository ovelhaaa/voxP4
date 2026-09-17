#include "pitch_analysis.h"
#include "yin_detector.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>
#include <vector>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr size_t kWindow = 512, kHop = 60;
constexpr float kMinInputDb = -55.0f;

const std::array<YinEnergyVariant, 3> kVariants{{
    YinEnergyVariant::ReferenceDouble, YinEnergyVariant::F32,
    YinEnergyVariant::F32Compensated,
}};

struct Fixture {
  std::string name;
  std::vector<float> input_48k;
  bool threshold = false;
};

uint16_t u16(std::istream &in) {
  unsigned char b[2]{};
  in.read(reinterpret_cast<char *>(b), 2);
  return static_cast<uint16_t>(b[0] | (b[1] << 8));
}
uint32_t u32(std::istream &in) {
  unsigned char b[4]{};
  in.read(reinterpret_cast<char *>(b), 4);
  return static_cast<uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16) |
                               (b[3] << 24));
}

std::vector<float> load_wav(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  char id[4]{};
  in.read(id, 4);
  (void)u32(in);
  char wave[4]{};
  in.read(wave, 4);
  if (!in || std::memcmp(id, "RIFF", 4) || std::memcmp(wave, "WAVE", 4))
    return {};
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  std::vector<unsigned char> data;
  while (in.read(id, 4)) {
    const uint32_t size = u32(in);
    if (!std::memcmp(id, "fmt ", 4)) {
      format = u16(in);
      channels = u16(in);
      rate = u32(in);
      (void)u32(in);
      (void)u16(in);
      bits = u16(in);
      if (size > 16)
        in.seekg(size - 16, std::ios::cur);
    } else if (!std::memcmp(id, "data", 4)) {
      data.resize(size);
      in.read(reinterpret_cast<char *>(data.data()), size);
    } else {
      in.seekg(size, std::ios::cur);
    }
    if (size & 1)
      in.seekg(1, std::ios::cur);
  }
  if (rate != 48000 || channels == 0 || data.empty())
    return {};
  const size_t bytes = bits / 8, frame_bytes = bytes * channels;
  if (!bytes || data.size() < frame_bytes)
    return {};
  std::vector<float> out(data.size() / frame_bytes);
  for (size_t i = 0; i < out.size(); ++i) {
    double sum = 0.0;
    for (size_t c = 0; c < channels; ++c) {
      const unsigned char *p = data.data() + i * frame_bytes + c * bytes;
      if (format == 1 && bits == 16) {
        int16_t sample = 0;
        std::memcpy(&sample, p, sizeof(sample));
        sum += sample / 32768.0;
      } else if (format == 3 && bits == 32) {
        float sample = 0.0f;
        std::memcpy(&sample, p, sizeof(sample));
        sum += sample;
      } else {
        return {};
      }
    }
    out[i] = static_cast<float>(sum / channels);
  }
  return out;
}

float sine_amplitude_for_rms_db(float db) {
  return std::sqrt(2.0f) * std::pow(10.0f, db / 20.0f);
}

Fixture sine_fixture(std::string name, float hz, float db = -18.0f,
                     float seconds = 1.2f, bool threshold = false) {
  Fixture f{std::move(name),
            std::vector<float>(static_cast<size_t>(48000 * seconds)),
            threshold};
  const float amplitude = sine_amplitude_for_rms_db(db);
  for (size_t i = 0; i < f.input_48k.size(); ++i)
    f.input_48k[i] = amplitude *
                     std::sin(2.0f * kPi * hz * i / 48000.0f);
  return f;
}

std::vector<Fixture> fixtures(const std::filesystem::path &root) {
  std::vector<Fixture> out;
  out.push_back({"silence", std::vector<float>(48000), false});
  Fixture low{"very_low_noise", std::vector<float>(48000), true};
  uint32_t state = 0x4234424bU;
  for (float &sample : low.input_48k) {
    state = state * 1664525U + 1013904223U;
    sample = 1.0e-7f * (static_cast<int32_t>(state) / 2147483648.0f);
  }
  out.push_back(std::move(low));
  out.push_back(sine_fixture("level_gate_below", 220.0f, -55.01f, 1.2f,
                             true));
  out.push_back(sine_fixture("level_gate_above", 220.0f, -54.99f, 1.2f,
                             true));
  out.push_back(sine_fixture("derived_floor_below", 220.0f, -120.01f, 1.2f,
                             true));
  out.push_back(sine_fixture("derived_floor_above", 220.0f, -119.99f, 1.2f,
                             true));
  for (float hz : {80.0f, 110.0f, 147.0f, 220.0f, 330.0f, 440.0f,
                   700.0f, 900.0f})
    out.push_back(sine_fixture("sine_" + std::to_string(static_cast<int>(hz)),
                               hz));
  Fixture harmonic = sine_fixture("harmonic_220", 220.0f);
  for (size_t i = 0; i < harmonic.input_48k.size(); ++i)
    harmonic.input_48k[i] += 0.04f *
        std::sin(4.0f * kPi * 220.0f * i / 48000.0f);
  out.push_back(std::move(harmonic));
  Fixture noise{"deterministic_noise", std::vector<float>(57600), false};
  state = 0x4b454e45U;
  for (float &sample : noise.input_48k) {
    state = state * 1664525U + 1013904223U;
    sample = 0.08f * (static_cast<int32_t>(state) / 2147483648.0f);
  }
  out.push_back(std::move(noise));
  Fixture mixed = sine_fixture("tone_noise", 220.0f);
  state = 0x544f4e45U;
  for (float &sample : mixed.input_48k) {
    state = state * 1664525U + 1013904223U;
    sample += 0.025f * (static_cast<int32_t>(state) / 2147483648.0f);
  }
  out.push_back(std::move(mixed));
  Fixture onset{"onset_ratio_border", std::vector<float>(96000), true};
  const float base = sine_amplitude_for_rms_db(-40.0f);
  const float ratio_below = std::sqrt(2.5f) * (1.0f - 2.0e-4f);
  const float ratio_above = std::sqrt(2.5f) * (1.0f + 2.0e-4f);
  for (size_t i = 0; i < onset.input_48k.size(); ++i) {
    const float gain = i < 24000 ? 1.0f : i < 48000 ? ratio_below
                                      : i < 72000 ? 1.0f : ratio_above;
    onset.input_48k[i] = base * gain *
        std::sin(2.0f * kPi * 220.0f * i / 48000.0f);
  }
  out.push_back(std::move(onset));
  auto vocal = load_wav(root /
      "artifacts/formant_preservation/renders/full/00_lead_dry.wav");
  if (!vocal.empty()) {
    const size_t limit = std::min<size_t>(vocal.size(), 96000);
    vocal.resize(limit);
    out.push_back({"repository_vocal_wav", std::move(vocal), false});
  }
  return out;
}

std::vector<float> analysis_samples(const Fixture &fixture) {
  std::vector<float> out;
  out.reserve(fixture.input_48k.size() / 4);
  for (size_t i = 0; i < fixture.input_48k.size(); i += 4)
    out.push_back(fixture.input_48k[i]);
  return out;
}

struct EnergyStats {
  uint64_t hops = 0, energy_bit_mismatch = 0, rms_bit_mismatch = 0;
  uint64_t nonfinite = 0;
  double max_energy_abs = 0.0, max_energy_rel = 0.0;
  long double energy_squared = 0.0, rms_squared = 0.0;
  float max_rms_abs = 0.0f;
};

float rms_db(double energy) {
  return 10.0f * std::log10(static_cast<float>(energy / kWindow) + 1e-20f);
}

EnergyStats direct_energy(const Fixture &fixture, YinEnergyVariant variant) {
  EnergyStats stats{};
  const auto samples = analysis_samples(fixture);
  if (samples.size() < kWindow)
    return stats;
  for (size_t offset = 0; offset + kWindow <= samples.size(); offset += kHop) {
    const float *frame = samples.data() + offset;
    const double reference = YIN_ENERGY_REFERENCE_DOUBLE(frame, kWindow);
    const double candidate = yin_energy_compute(variant, frame, kWindow);
    const float reference_db = rms_db(reference);
    const float candidate_db = rms_db(candidate);
    uint64_t reference_bits = 0, candidate_bits = 0;
    std::memcpy(&reference_bits, &reference, sizeof(reference));
    std::memcpy(&candidate_bits, &candidate, sizeof(candidate));
    stats.energy_bit_mismatch += reference_bits != candidate_bits;
    uint32_t reference_db_bits = 0, candidate_db_bits = 0;
    std::memcpy(&reference_db_bits, &reference_db, sizeof(reference_db));
    std::memcpy(&candidate_db_bits, &candidate_db, sizeof(candidate_db));
    stats.rms_bit_mismatch += reference_db_bits != candidate_db_bits;
    const double error = candidate - reference;
    const double absolute = std::fabs(error);
    stats.max_energy_abs = std::max(stats.max_energy_abs, absolute);
    stats.max_energy_rel = std::max(
        stats.max_energy_rel,
        reference == 0.0 ? (candidate == 0.0 ? 0.0 :
                            std::numeric_limits<double>::infinity())
                         : absolute / std::fabs(reference));
    stats.energy_squared += static_cast<long double>(error) * error;
    const float db_error = candidate_db - reference_db;
    stats.max_rms_abs = std::max(stats.max_rms_abs, std::fabs(db_error));
    stats.rms_squared += static_cast<long double>(db_error) * db_error;
    stats.nonfinite += (!std::isfinite(reference) || !std::isfinite(candidate) ||
                        !std::isfinite(reference_db) ||
                        !std::isfinite(candidate_db));
    ++stats.hops;
  }
  return stats;
}

struct TrackerRun {
  std::vector<PitchResult> results;
  std::vector<PitchMark> marks;
};

TrackerRun tracker_run(const Fixture &fixture, YinEnergyVariant variant) {
  PitchAnalysisConfig config{};
  config.yin_difference = YinDifferenceVariant::IncrementalF32;
  config.yin_incremental_rebase_hops = 64;
  config.yin_cmnd = YinCmndVariant::F32Compensated;
  config.yin_energy = variant;
  config.pitch_mark_ncc = PitchMarkNccVariant::ReuseAa;
  PitchAnalysis tracker;
  TrackerRun run;
  if (!tracker.init(config))
    return run;
  for (size_t offset = 0; offset < fixture.input_48k.size(); offset += 64) {
    const size_t count = std::min<size_t>(64, fixture.input_48k.size() - offset);
    tracker.tap(fixture.input_48k.data() + offset, count);
    while (tracker.run(1))
      run.results.push_back(tracker.latest());
  }
  std::array<PitchMark, 128> marks{};
  const size_t count = tracker.marks(0, std::numeric_limits<uint64_t>::max(),
                                     marks.data(), marks.size());
  run.marks.assign(marks.begin(), marks.begin() + count);
  return run;
}

struct PitchStats {
  uint64_t reference_hops = 0, candidate_hops = 0;
  uint64_t level_ok = 0, voiced_raw = 0, voiced_stateful = 0, voiced = 0;
  uint64_t onset = 0, pitch_changed = 0, selected_tau = 0, track_state = 0;
  uint64_t coherent_marks = 0, mark_count = 0, mark_position = 0;
  uint64_t nonfinite = 0;
  float period = 0.0f, f0 = 0.0f, confidence = 0.0f;
  float rms_db = 0.0f, linear_energy = 0.0f;
  float previous_before = 0.0f, previous_after = 0.0f;
};

PitchStats compare(const TrackerRun &a, const TrackerRun &b,
                   const std::string &fixture, YinEnergyVariant variant,
                   std::ofstream *threshold) {
  PitchStats s{};
  s.reference_hops = a.results.size();
  s.candidate_hops = b.results.size();
  const size_t count = std::min(a.results.size(), b.results.size());
  for (size_t i = 0; i < count; ++i) {
    const auto &x = a.results[i];
    const auto &y = b.results[i];
    s.level_ok += x.level_ok != y.level_ok;
    s.voiced_raw += x.voiced_raw != y.voiced_raw;
    s.voiced_stateful += x.voiced_stateful != y.voiced_stateful;
    s.voiced += x.voiced != y.voiced;
    s.onset += x.onset != y.onset;
    s.pitch_changed += x.pitch_changed != y.pitch_changed;
    s.selected_tau += x.yin_tau != y.yin_tau;
    s.track_state += x.pitch_track_state != y.pitch_track_state;
    s.coherent_marks += x.coherent_marks != y.coherent_marks;
    s.period = std::max(s.period, std::fabs(x.period_samples-y.period_samples));
    s.f0 = std::max(s.f0, std::fabs(x.frequency_hz-y.frequency_hz));
    s.confidence = std::max(s.confidence, std::fabs(x.confidence-y.confidence));
    s.rms_db = std::max(s.rms_db,
                        std::fabs(x.detector_rms_db-y.detector_rms_db));
    s.linear_energy = std::max(s.linear_energy,
        std::fabs(x.detector_linear_energy-y.detector_linear_energy));
    s.previous_before = std::max(s.previous_before,
        std::fabs(x.previous_energy_before-y.previous_energy_before));
    s.previous_after = std::max(s.previous_after,
        std::fabs(x.previous_energy_after-y.previous_energy_after));
    s.nonfinite += !std::isfinite(y.detector_rms_db) ||
                   !std::isfinite(y.detector_linear_energy) ||
                   !std::isfinite(y.previous_energy_before) ||
                   !std::isfinite(y.previous_energy_after);
    if (threshold)
      *threshold << fixture << ',' << yin_energy_variant_name(variant) << ','
                 << i << ',' << x.detector_rms_db << ',' << y.detector_rms_db
                 << ',' << x.level_ok << ',' << y.level_ok << ','
                 << x.detector_rms_db-kMinInputDb << ','
                 << y.detector_rms_db-kMinInputDb << ','
                 << x.detector_linear_energy << ','
                 << y.detector_linear_energy << ','
                 << x.previous_energy_before << ','
                 << y.previous_energy_before << ','
                 << x.previous_energy_after << ',' << y.previous_energy_after
                 << ',' << x.onset << ',' << y.onset << ','
                 << (x.previous_energy_before > 0.0f
                         ? x.detector_linear_energy /
                               (x.previous_energy_before * 2.5f) - 1.0f
                         : 0.0f)
                 << '\n';
  }
  s.mark_count = a.marks.size() != b.marks.size();
  const size_t marks = std::min(a.marks.size(), b.marks.size());
  for (size_t i = 0; i < marks; ++i)
    s.mark_position += a.marks[i].sample_position != b.marks[i].sample_position;
  return s;
}

bool eligible(const EnergyStats &e, const PitchStats &p) {
  return e.nonfinite == 0 && e.max_rms_abs <= 1e-5f && p.level_ok == 0 &&
         p.voiced_raw == 0 && p.voiced_stateful == 0 && p.voiced == 0 &&
         p.onset == 0 && p.pitch_changed == 0 && p.selected_tau == 0 &&
         p.track_state == 0 && p.coherent_marks == 0 && p.mark_count == 0 &&
         p.mark_position == 0 && p.nonfinite == 0;
}

void long_audit(std::ofstream &energy_csv, std::ofstream &pitch_csv,
                YinEnergyVariant variant, bool *pass) {
  constexpr size_t kHops = 120000;
  std::array<float, kWindow> frame{};
  EnergyStats e{};
  float previous_reference = 0.0f, previous_candidate = 0.0f;
  PitchStats p{};
  uint32_t state = 0x4b4c4f4eU;
  for (size_t hop = 0; hop < kHops; ++hop) {
    for (size_t i = 0; i < frame.size(); ++i) {
      state = state * 1664525U + 1013904223U;
      const float noise = static_cast<int32_t>(state) / 2147483648.0f;
      const float t = static_cast<float>(hop*kHop+i) / 12000.0f;
      const float hz = hop < kHops/2 ? 147.0f : 220.0f;
      frame[i] = 0.12f*std::sin(2*kPi*hz*t) + 0.02f*noise;
    }
    const double reference = YIN_ENERGY_REFERENCE_DOUBLE(frame.data(),kWindow);
    const double candidate = yin_energy_compute(variant,frame.data(),kWindow);
    const float rd = rms_db(reference), cd = rms_db(candidate);
    uint64_t reference_bits = 0, candidate_bits = 0;
    std::memcpy(&reference_bits, &reference, sizeof(reference));
    std::memcpy(&candidate_bits, &candidate, sizeof(candidate));
    e.energy_bit_mismatch += reference_bits != candidate_bits;
    uint32_t rd_bits = 0, cd_bits = 0;
    std::memcpy(&rd_bits, &rd, sizeof(rd));
    std::memcpy(&cd_bits, &cd, sizeof(cd));
    e.rms_bit_mismatch += rd_bits != cd_bits;
    const double error = candidate-reference;
    e.max_energy_abs=std::max(e.max_energy_abs,std::fabs(error));
    e.max_energy_rel=std::max(e.max_energy_rel,std::fabs(error)/reference);
    e.energy_squared += static_cast<long double>(error)*error;
    e.max_rms_abs=std::max(e.max_rms_abs,std::fabs(cd-rd));
    e.rms_squared += static_cast<long double>(cd-rd)*(cd-rd);
    e.nonfinite += (!std::isfinite(reference) || !std::isfinite(candidate) ||
                    !std::isfinite(rd) || !std::isfinite(cd));
    const float re=std::pow(10.0f,rd/10.0f), ce=std::pow(10.0f,cd/10.0f);
    const bool rl=rd>kMinInputDb, cl=cd>kMinInputDb;
    const bool ro=re>1e-12f&&previous_reference>1e-12f&&
                  re>previous_reference*2.5f;
    const bool co=ce>1e-12f&&previous_candidate>1e-12f&&
                  ce>previous_candidate*2.5f;
    p.level_ok += rl!=cl;
    p.onset += ro!=co;
    p.rms_db=std::max(p.rms_db,std::fabs(rd-cd));
    p.linear_energy=std::max(p.linear_energy,std::fabs(re-ce));
    p.previous_before=std::max(p.previous_before,
                               std::fabs(previous_reference-previous_candidate));
    previous_reference=.8f*previous_reference+.2f*re;
    previous_candidate=.8f*previous_candidate+.2f*ce;
    p.previous_after=std::max(p.previous_after,
                              std::fabs(previous_reference-previous_candidate));
    ++e.hops;
  }
  const double energy_rms=std::sqrt(static_cast<double>(e.energy_squared/e.hops));
  const double db_rms=std::sqrt(static_cast<double>(e.rms_squared/e.hops));
  const bool ok=eligible(e,p);
  *pass &= ok;
  energy_csv << "long_10min," << yin_energy_variant_name(variant) << ','
             << e.hops << ',' << e.max_energy_abs << ',' << e.max_energy_rel
             << ',' << energy_rms << ',' << e.max_rms_abs << ',' << db_rms
             << ',' << e.energy_bit_mismatch << ',' << e.rms_bit_mismatch
             << ',' << e.nonfinite << ',' << (ok?"yes":"no") << '\n';
  pitch_csv << "long_10min," << yin_energy_variant_name(variant) << ','
            << e.hops << ',' << e.hops << ',' << p.level_ok << ",0,0,0,"
            << p.onset << ",0,0,0,0,0,0,0,0,0," << p.rms_db << ','
            << p.linear_energy << ',' << p.previous_before << ','
            << p.previous_after << ',' << p.nonfinite << ','
            << (ok?"yes":"no") << '\n';
}
} // namespace

int main(int argc, char **argv) {
  const bool verify = argc > 1 && std::string(argv[1]) == "--verify";
  const bool long_stream = argc > 1 && std::string(argv[1]) == "--long";
  const std::filesystem::path root = VOXP4_SOURCE_DIR;
  const auto corpus = fixtures(root);
  if (corpus.size() < 18) {
    std::fprintf(stderr,"B4B.4K fixture corpus incomplete: %zu\n",corpus.size());
    return 1;
  }
  const auto out = root / "artifacts/alpha01b";
  std::filesystem::create_directories(out);
  std::ofstream energy_csv, threshold_csv, pitch_csv;
  if (!verify) {
    energy_csv.open(out / "b4b4k_yin_energy_equivalence.csv");
    threshold_csv.open(out / "b4b4k_threshold_guard.csv");
    pitch_csv.open(out / "b4b4k_pitch_equivalence.csv");
    energy_csv << "fixture,variant,hops,energy_max_abs_error,energy_max_relative_error,energy_rms_error,rms_db_max_abs_error,rms_db_rms_error,energy_bit_mismatches,rms_db_bit_mismatches,nonfinite_regressions,eligible\n";
    threshold_csv << "fixture,variant,hop,reference_rms_db,candidate_rms_db,reference_level_ok,candidate_level_ok,reference_min_input_margin_db,candidate_min_input_margin_db,reference_derived_energy,candidate_derived_energy,reference_previous_energy_before,candidate_previous_energy_before,reference_previous_energy_after,candidate_previous_energy_after,reference_onset,candidate_onset,reference_onset_ratio_margin\n";
    pitch_csv << "fixture,variant,reference_hops,candidate_hops,level_ok_mismatches,voiced_raw_mismatches,voiced_stateful_mismatches,final_voiced_mismatches,onset_mismatches,pitch_changed_mismatches,selected_tau_mismatches,track_state_mismatches,coherent_mark_mismatches,pitch_mark_count_mismatches,pitch_mark_position_mismatches,max_period_error_samples,max_f0_error_hz,max_confidence_error,max_rms_db_error,max_derived_energy_error,max_previous_energy_before_error,max_previous_energy_after_error,nonfinite_regressions,eligible\n";
    energy_csv << std::setprecision(17);
    threshold_csv << std::setprecision(12);
    pitch_csv << std::setprecision(12);
  }
  bool pass = true;
  for (const auto &fixture : corpus) {
    const TrackerRun reference =
        tracker_run(fixture,YinEnergyVariant::ReferenceDouble);
    for (const auto variant : kVariants) {
      const EnergyStats e=direct_energy(fixture,variant);
      const PitchStats p=compare(reference,tracker_run(fixture,variant),
                                 fixture.name,variant,
                                 !verify&&fixture.threshold?&threshold_csv:nullptr);
      const bool ok=eligible(e,p);
      if (variant==YinEnergyVariant::F32Compensated)
        pass &= ok;
      if (!verify) {
        const double energy_rms=e.hops?std::sqrt(static_cast<double>(
            e.energy_squared/e.hops)):0.0;
        const double db_rms=e.hops?std::sqrt(static_cast<double>(
            e.rms_squared/e.hops)):0.0;
        energy_csv << fixture.name << ',' << yin_energy_variant_name(variant)
                   << ',' << e.hops << ',' << e.max_energy_abs << ','
                   << e.max_energy_rel << ',' << energy_rms << ','
                   << e.max_rms_abs << ',' << db_rms << ','
                   << e.energy_bit_mismatch << ',' << e.rms_bit_mismatch << ','
                   << e.nonfinite << ',' << (ok?"yes":"no") << '\n';
        pitch_csv << fixture.name << ',' << yin_energy_variant_name(variant)
                  << ',' << p.reference_hops << ',' << p.candidate_hops << ','
                  << p.level_ok << ',' << p.voiced_raw << ','
                  << p.voiced_stateful << ',' << p.voiced << ',' << p.onset
                  << ',' << p.pitch_changed << ',' << p.selected_tau << ','
                  << p.track_state << ',' << p.coherent_marks << ','
                  << p.mark_count << ',' << p.mark_position << ',' << p.period
                  << ',' << p.f0 << ',' << p.confidence << ',' << p.rms_db
                  << ',' << p.linear_energy << ',' << p.previous_before << ','
                  << p.previous_after << ',' << p.nonfinite << ','
                  << (ok?"yes":"no") << '\n';
      }
    }
  }
  if (!verify && long_stream)
    for (const auto variant : kVariants)
      long_audit(energy_csv,pitch_csv,variant,&pass);
  std::printf("B4B.4K host energy audit: fixtures=%zu samples/hop=%zu long=%s compensated=%s\n",
              corpus.size(),kWindow,long_stream?"yes":"no",pass?"PASS":"FAIL");
  return pass?0:1;
}
