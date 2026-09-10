#include "vocal_fx.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static uint32_t u32(const unsigned char *p) {
  return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
}
static uint16_t u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static void put16(std::ofstream &f, uint16_t v) {
  char b[2] = {(char)v, (char)(v >> 8)};
  f.write(b, 2);
}
static void put32(std::ofstream &f, uint32_t v) {
  char b[4] = {(char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24)};
  f.write(b, 4);
}
int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: pitch_shift input.wav output.wav semitones [--formants off|lpc] [--formant-amount 0..1] [--lpc-order 10|12|16|20] [--history-offset-ms 24|32|40|48|64] [--debug-csv file] [--continuity-policy baseline|onset|coasting|combined]\n");
    return 2;
  }
  std::ifstream f(argv[1], std::ios::binary);
  std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), {});
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) ||
      std::memcmp(b.data() + 8, "WAVE", 4)) {
    std::fprintf(stderr, "invalid WAV\n");
    return 2;
  }
  uint16_t fmt = 0, ch = 0, bits = 0;
  uint32_t rate = 0;
  const unsigned char *data = nullptr;
  size_t bytes = 0;
  for (size_t p = 12; p + 8 <= b.size();) {
    uint32_t n = u32(&b[p + 4]);
    if (p + 8 + n > b.size())
      break;
    if (!std::memcmp(&b[p], "fmt ", 4) && n >= 16) {
      fmt = u16(&b[p + 8]);
      ch = u16(&b[p + 10]);
      rate = u32(&b[p + 12]);
      bits = u16(&b[p + 22]);
    } else if (!std::memcmp(&b[p], "data", 4)) {
      data = &b[p + 8];
      bytes = n;
    }
    p += 8 + n + (n & 1);
  }
  if (!data || rate != 48000 || !ch ||
      !((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32))) {
    std::fprintf(stderr, "requires 48 kHz PCM16 or float32 WAV\n");
    return 2;
  }
  VocalFxConfig c{};
  c.enable_gate = c.enable_compressor = c.enable_delay = c.enable_reverb =
      false;
  c.pitch_shift.enabled = true;
  c.pitch_shift.semitones = std::stof(argv[3]);
  c.pitch_shift.wet = 1;
  c.isolate_pitch_shift_output = true;
  FormantMode formants=FormantMode::Off; float amount=1.0f;
  std::string debug_path;
  std::string sample_telemetry_path;
  for(int i=4;i<argc;++i){
    const std::string option=argv[i];
    if(option=="--formants" && i+1<argc){const std::string value=argv[++i];if(value=="lpc")formants=FormantMode::Lpc;else if(value!="off"){std::fprintf(stderr,"invalid formant mode\n");return 2;}}
    else if(option=="--formant-amount"&&i+1<argc)amount=std::stof(argv[++i]);
    else if(option=="--lpc-order"&&i+1<argc)c.lpc.order=static_cast<uint16_t>(std::stoi(argv[++i]));
    else if(option=="--history-offset-ms"&&i+1<argc){const float ms=std::stof(argv[++i]);c.pitch_shift.history_offset_samples=static_cast<uint32_t>(std::lround(ms*48.0f));}
    else if(option=="--debug-csv"&&i+1<argc)debug_path=argv[++i];
    else if(option=="--sample-telemetry"&&i+1<argc)sample_telemetry_path=argv[++i];
    else if(option=="--continuity-policy"&&i+1<argc){const std::string val=argv[++i];if(val=="onset")c.pitch_shift.continuity_policy=PsolaContinuityPolicy::OnsetContinuity;else if(val=="coasting")c.pitch_shift.continuity_policy=PsolaContinuityPolicy::Coasting;else if(val=="combined")c.pitch_shift.continuity_policy=PsolaContinuityPolicy::OnsetContinuityCoasting;else if(val!="baseline"){std::fprintf(stderr,"invalid continuity policy: %s\n",val.c_str());return 2;}}
    else if(option=="--coast-ms"&&i+1<argc){const float ms=std::stof(argv[++i]);c.pitch_shift.coast_ms=ms;c.psola_coast_ms=ms;}
    else if(option=="--fallback-policy"&&i+1<argc){const std::string val=argv[++i];if(val=="muted")c.pitch_shift.fallback_policy=HarmonyFallbackPolicy::Muted;else if(val=="dry")c.pitch_shift.fallback_policy=HarmonyFallbackPolicy::CurrentDry;else if(val=="unvoiced")c.pitch_shift.fallback_policy=HarmonyFallbackPolicy::UnvoicedOnly;else if(val=="highpass")c.pitch_shift.fallback_policy=HarmonyFallbackPolicy::HighpassUnvoiced;else{std::fprintf(stderr,"invalid fallback policy: %s\n",val.c_str());return 2;}}
    else if(option=="--stateful"&&i+1<argc)c.pitch_shift.stateful_voicing_enabled=(std::stoi(argv[++i])!=0);
    else if(option=="--enter-confidence"&&i+1<argc)c.pitch_shift.voiced_enter_confidence=std::stof(argv[++i]);
    else if(option=="--stay-confidence"&&i+1<argc)c.pitch_shift.voiced_stay_confidence=std::stof(argv[++i]);
    else if(option=="--exit-confidence"&&i+1<argc)c.pitch_shift.voiced_exit_confidence=std::stof(argv[++i]);
    else if(option=="--release-frames"&&i+1<argc)c.pitch_shift.voiced_release_frames=static_cast<uint8_t>(std::stoi(argv[++i]));
    else if(option=="--attack-frames"&&i+1<argc)c.pitch_shift.voiced_attack_frames=static_cast<uint8_t>(std::stoi(argv[++i]));
    else if(option=="--continuity-cents"&&i+1<argc)c.pitch_shift.f0_continuity_tolerance_cents=std::stof(argv[++i]);
    else if(option=="--align-dry"&&i+1<argc)c.align_dry_to_harmony=(std::stoi(argv[++i])!=0);
    else if(option=="--dry-delay-ms"&&i+1<argc)c.dry_alignment_ms=std::stof(argv[++i]);
    else if(option=="--harmony-attack-ms"&&i+1<argc)c.harmony_attack_ms=std::stof(argv[++i]);
    else if(option=="--harmony-release-ms"&&i+1<argc)c.harmony_release_ms=std::stof(argv[++i]);
    else if(option=="--harmony-limiter"&&i+1<argc)c.enable_harmony_limiter=(std::stoi(argv[++i])!=0);
    else if(option=="--limiter-thresh-db"&&i+1<argc)c.harmony_limiter_threshold_db=std::stof(argv[++i]);
    else if(option=="--render-mode"&&i+1<argc){
      const std::string m=argv[++i];
      if(m=="solo"){c.isolate_pitch_shift_output=true;c.apply_isolated_voice_envelope=true;}
      else if(m=="mix"){c.isolate_pitch_shift_output=false;}
      else if(m=="raw"){c.isolate_pitch_shift_output=true;c.apply_isolated_voice_envelope=false;}
      else{std::fprintf(stderr,"invalid render mode: %s\n",m.c_str());return 2;}
    }
    else {std::fprintf(stderr,"unknown option: %s\n",option.c_str());return 2;}
  }
  if (!vocal_fx_init(c)) {
    std::fprintf(stderr, "engine init failed\n");
    return 2;
  }
  vocal_fx_set_formant_mode(0,formants); vocal_fx_set_formant_amount(0,amount);
  std::ofstream debug;
  if(!debug_path.empty()){
    debug.open(debug_path);
    debug << "time,input_absolute_sample,pitch_timestamp,selected_source_mark,source_grain_timestamp,output_synthesis_timestamp,history_offset,analysis_age,requested_semitones,target_ratio,current_smoothed_ratio,source_f0,target_f0,actual_synthesis_period,voice_state,pitch_tracker_state,pitch_voiced,pitch_confidence,grains_total,usable,psola_gain\n";
  }
  std::ofstream telem_file;
  if (!sample_telemetry_path.empty()) {
    telem_file.open(sample_telemetry_path, std::ios::binary);
    if (telem_file.is_open()) {
      vocal_fx_set_sample_telemetry_callback([](const SampleTelemetryRecord *records, size_t count, void *user_data) {
        auto *out = static_cast<std::ofstream*>(user_data);
        if (out && out->is_open() && records && count > 0) {
          out->write(reinterpret_cast<const char*>(records), count * sizeof(SampleTelemetryRecord));
        }
      }, &telem_file);
    } else {
      std::fprintf(stderr, "warning: could not open sample telemetry file: %s\n", sample_telemetry_path.c_str());
    }
  }
  const size_t stride = ch * bits / 8, frames = bytes / stride;
  std::vector<float> mono(frames), out(frames), right(64);
  for (size_t i = 0; i < frames; ++i) {
    double sum = 0;
    for (unsigned k = 0; k < ch; ++k) {
      const unsigned char *p = data + i * stride + k * bits / 8;
      if (fmt == 1)
        sum += (int16_t)u16(p) / 32768.0;
      else {
        float v;
        std::memcpy(&v, p, 4);
        sum += v;
      }
    }
    mono[i] = sum / ch;
  }
  for (size_t i = 0; i < frames; i += 64) {
    size_t n = std::min<size_t>(64, frames - i);
    vocal_fx_process(mono.data() + i, out.data() + i, right.data(), n);
    while (vocal_fx_run_pitch_analysis(8)) {
    }
    if(debug){const auto d=vocal_fx_pitch_shift_debug();debug << static_cast<double>(i+n)/rate << ',' << d.input_absolute_sample << ',' << d.pitch_timestamp << ',' << d.selected_source_mark << ',' << d.source_grain_timestamp << ',' << d.output_synthesis_timestamp << ',' << d.history_offset << ',' << d.analysis_age << ',' << d.requested_semitones << ',' << d.target_ratio << ',' << d.current_smoothed_ratio << ',' << d.source_f0 << ',' << d.target_f0 << ',' << d.actual_synthesis_period << ',' << static_cast<unsigned>(d.voice_state) << ',' << static_cast<unsigned>(d.pitch_tracker_state) << ',' << (d.pitch_voiced ? 1 : 0) << ',' << d.pitch_confidence << ',' << d.grains_total << ',' << (d.usable ? 1 : 0) << ',' << d.psola_gain << '\n';}
  }
  if (telem_file.is_open()) {
    vocal_fx_set_sample_telemetry_callback(nullptr, nullptr);
    telem_file.close();
  }
  std::ofstream w(argv[2], std::ios::binary);
  if (!w.is_open()) {
    std::fprintf(stderr, "could not open output WAV: %s\n", argv[2]);
    return 3;
  }
  const uint32_t data_bytes = frames * 4;
  w.write("RIFF", 4);
  put32(w, 36 + data_bytes);
  w.write("WAVEfmt ", 8);
  put32(w, 16);
  put16(w, 3);
  put16(w, 1);
  put32(w, rate);
  put32(w, rate * 4);
  put16(w, 4);
  put16(w, 32);
  w.write("data", 4);
  put32(w, data_bytes);
  w.write(reinterpret_cast<const char *>(out.data()), data_bytes);
  const bool write_failed = !w;
  if (write_failed)
    std::fprintf(stderr, "failed while writing output WAV: %s\n", argv[2]);
  w.close();
  if (w.fail()) {
    if (!write_failed)
      std::fprintf(stderr, "failed while closing output WAV: %s\n", argv[2]);
    return 3;
  }
  const auto t = vocal_fx_pitch_shift_telemetry();
  std::fprintf(stderr,
               "latency=%u samples grains=%llu max/block=%u fallback=%llu "
               "mark_underflow=%llu history_underflow=%llu\n",
               vocal_fx_pitch_shift_latency_samples(),
               (unsigned long long)t.grains, t.max_grains_per_block,
               (unsigned long long)t.fallback_frames,
               (unsigned long long)t.pitch_mark_underflows,
               (unsigned long long)t.audio_history_underflows);
  const auto lt=vocal_fx_lpc_telemetry();
  std::fprintf(stderr,"lpc_frames=%llu invalid=%llu fallback=%llu max_error=%.6f\n",(unsigned long long)lt.lpc_frames,(unsigned long long)lt.lpc_invalid_frames,(unsigned long long)lt.lpc_fallback_frames,lt.max_prediction_error);
}
