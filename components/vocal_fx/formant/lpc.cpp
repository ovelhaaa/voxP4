#include "lpc.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr float kPi = 3.14159265358979323846f;
ProfileSection section(LpcProfileSection s) {
  return static_cast<ProfileSection>(static_cast<size_t>(ProfileSection::LpcWindowing) +
                                     static_cast<size_t>(s));
}
}

bool SharedLpcAnalysis::init(float rate, const LpcConfig &c) {
  if (!std::isfinite(rate) || rate < 8000 || c.order < 1 ||
      c.order > VOCAL_FX_LPC_MAX_ORDER || c.window_size < c.order + 2 ||
      c.window_size > frame_.size() || c.hop_size == 0 ||
      c.hop_size > c.window_size || !std::isfinite(c.preemphasis) ||
      c.preemphasis < 0 || c.preemphasis >= 1)
    return false;
  sample_rate_ = rate; config_ = c; reset(); return true;
}
void SharedLpcAnalysis::reset() {
  fifo_.reset(); frame_.fill(0); fill_ = since_frame_ = 0; last_position_ = 0;
  telemetry_ = {}; profiler_.reset(); published_.store(0, std::memory_order_release);
  for (auto &m : models_) { m.sequence.store(0); m.valid.store(0); }
  publish_telemetry();
}
void SharedLpcAnalysis::tap(const float *x, size_t n) {
  for (size_t i=0;i<n;++i)
    (void)fifo_.push({std::isfinite(x[i]) ? x[i] : 0.0f,
                      static_cast<uint32_t>(last_position_++)});
}
bool SharedLpcAnalysis::solve(const float *x, size_t n, uint16_t order,
                              float pre, SharedLpcModel *out) {
  if (!x || !out || n > 1024 || n < static_cast<size_t>(order) + 2 ||
      order > VOCAL_FX_LPC_MAX_ORDER)
    return false;
  std::array<float, 1024> y{};
  double energy=0;
  for(size_t i=0;i<n;++i){
    const float v=x[i]-(i?pre*x[i-1]:0.0f);
    const float w=.5f-.5f*std::cos(2*kPi*i/(n-1)); y[i]=v*w; energy+=double(y[i])*y[i];
  }
  if (!(energy > 1e-8) || !std::isfinite(energy)) return false;
  std::array<double,VOCAL_FX_LPC_MAX_ORDER+1> r{},a{},next{};
  // O(N*order) hotspot: candidate for ESP32-P4 Xai/SIMD after target profiling.
  for(size_t k=0;k<=order;++k) for(size_t i=k;i<n;++i) r[k]+=double(y[i])*y[i-k];
  // Tiny diagonal loading prevents perfectly periodic synthetic frames from
  // producing a singular Toeplitz system without materially flattening speech.
  r[0] *= 1.000001;
  double error=r[0]; a[0]=1.0;
  for(size_t i=1;i<=order;++i){
    double sum=r[i]; for(size_t j=1;j<i;++j) sum+=a[j]*r[i-j];
    const double reflection=-sum/error;
    if(!std::isfinite(reflection)||std::fabs(reflection)>=.999999||error<=1e-12) return false;
    next=a; next[i]=reflection;
    for(size_t j=1;j<i;++j) next[j]=a[j]+reflection*a[i-j];
    a=next; error*=1.0-reflection*reflection;
    if(!std::isfinite(error)||error<=1e-12) return false;
  }
  out->coefficients.fill(0); for(size_t i=0;i<=order;++i) out->coefficients[i]=float(a[i]);
  out->order=order; out->prediction_error=float(error/r[0]);
  out->confidence=std::clamp(1.0f-out->prediction_error,0.0f,1.0f);
  out->valid=std::isfinite(out->prediction_error); return out->valid;
}

float SharedLpcAnalysis::lambda_from_semitones(float semitones) {
  if (std::fabs(semitones) < 1e-4f) return 0.0f;
  const float beta = std::exp2(std::clamp(semitones, -12.0f, 12.0f) / 12.0f);
  return (1.0f - beta) / (1.0f + beta);
}

bool SharedLpcAnalysis::warp_polynomial(const float *a_in, uint16_t order,
                                        float lambda, float gamma, float *a_out) {
  if (!a_in || !a_out || order == 0 || order > VOCAL_FX_LPC_MAX_ORDER)
    return false;
  if (std::fabs(lambda) < 1e-5f && (gamma >= 0.9999f || gamma <= 0.0f)) {
    for (size_t i = 0; i <= order; ++i) a_out[i] = a_in[i];
    return true;
  }
  if (std::fabs(lambda) < 1e-5f) {
    float g = 1.0f;
    for (size_t i = 0; i <= order; ++i) {
      a_out[i] = a_in[i] * g;
      g *= gamma;
    }
    return true;
  }

  lambda = std::clamp(lambda, -0.4f, 0.4f);

  std::array<std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1>, VOCAL_FX_LPC_MAX_ORDER + 1> N_pow{};
  std::array<std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1>, VOCAL_FX_LPC_MAX_ORDER + 1> D_pow{};

  N_pow[0][0] = 1.0f;
  D_pow[0][0] = 1.0f;

  for (size_t k = 1; k <= order; ++k) {
    N_pow[k][0] = -lambda * N_pow[k - 1][0];
    for (size_t j = 1; j <= k; ++j) {
      N_pow[k][j] = -lambda * N_pow[k - 1][j] + N_pow[k - 1][j - 1];
    }
    D_pow[k][0] = D_pow[k - 1][0];
    for (size_t j = 1; j <= k; ++j) {
      D_pow[k][j] = D_pow[k - 1][j] - lambda * D_pow[k - 1][j - 1];
    }
  }

  std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1> res{};
  float g = 1.0f;
  for (size_t k = 0; k <= order; ++k) {
    const float coeff = a_in[k] * g;
    if (gamma > 0.0f && gamma < 1.0f) g *= gamma;
    const size_t p_minus_k = order - k;
    for (size_t i = 0; i <= k; ++i) {
      const float ni = N_pow[k][i];
      for (size_t j = 0; j <= p_minus_k; ++j) {
        res[i + j] += coeff * ni * D_pow[p_minus_k][j];
      }
    }
  }

  if (std::fabs(res[0]) < 1e-8f || !std::isfinite(res[0])) {
    for (size_t i = 0; i <= order; ++i) a_out[i] = a_in[i];
    return false;
  }

  const float inv_r0 = 1.0f / res[0];
  for (size_t i = 0; i <= order; ++i) {
    a_out[i] = res[i] * inv_r0;
  }
  return true;
}

float SharedLpcAnalysis::compute_gain_normalization(const float *a_orig, const float *a_warped,
                                                    uint16_t order, FormantNormalizationStrategy strategy,
                                                    float sample_rate) {
  if (!a_orig || !a_warped || order == 0 || order > VOCAL_FX_LPC_MAX_ORDER)
    return 1.0f;

  switch (strategy) {
  case FormantNormalizationStrategy::StrategyA_DC: {
    float sum_orig = 0.0f, sum_warped = 0.0f;
    for (size_t i = 0; i <= order; ++i) {
      sum_orig += a_orig[i];
      sum_warped += a_warped[i];
    }
    const float abs_orig = std::fabs(sum_orig);
    const float abs_warped = std::fabs(sum_warped);
    if (abs_orig > 1e-4f && abs_warped > 1e-4f) {
      return std::clamp(abs_warped / abs_orig, 0.01f, 10.0f);
    }
    return 1.0f;
  }

  case FormantNormalizationStrategy::StrategyB_ReferenceFrequency: {
    // Reference frequency: 800 Hz (typical vocal vowel formant center)
    const float f_ref = 800.0f;
    const float w = 2.0f * kPi * (f_ref / sample_rate);
    float re_orig = 0.0f, im_orig = 0.0f;
    float re_warp = 0.0f, im_warp = 0.0f;
    for (size_t k = 0; k <= order; ++k) {
      const float phase = -static_cast<float>(k) * w;
      const float c = std::cos(phase);
      const float s = std::sin(phase);
      re_orig += a_orig[k] * c;
      im_orig += a_orig[k] * s;
      re_warp += a_warped[k] * c;
      im_warp += a_warped[k] * s;
    }
    const float mag_orig_sq = re_orig * re_orig + im_orig * im_orig;
    const float mag_warp_sq = re_warp * re_warp + im_warp * im_warp;
    if (mag_orig_sq > 1e-8f && mag_warp_sq > 1e-8f) {
      return std::clamp(std::sqrt(mag_warp_sq / mag_orig_sq), 0.01f, 10.0f);
    }
    return 1.0f;
  }

  case FormantNormalizationStrategy::StrategyC_IntegratedSpectral: {
    // 24 grid points log-spaced between 200 Hz and 4500 Hz
    constexpr size_t kPoints = 24;
    static constexpr float kFrequencies[kPoints] = {
      200.0f, 240.0f, 290.0f, 350.0f, 420.0f, 500.0f,
      600.0f, 720.0f, 860.0f, 1030.0f, 1230.0f, 1470.0f,
      1760.0f, 2100.0f, 2500.0f, 2900.0f, 3300.0f, 3700.0f,
      4000.0f, 4200.0f, 4400.0f, 4600.0f, 4800.0f, 5000.0f
    };
    double sum_mag_orig_sq = 0.0;
    double sum_mag_warp_sq = 0.0;

    for (size_t p = 0; p < kPoints; ++p) {
      const float w = 2.0f * kPi * (kFrequencies[p] / sample_rate);
      float re_o = 0.0f, im_o = 0.0f;
      float re_w = 0.0f, im_w = 0.0f;
      for (size_t k = 0; k <= order; ++k) {
        const float phase = -static_cast<float>(k) * w;
        const float c = std::cos(phase);
        const float s = std::sin(phase);
        re_o += a_orig[k] * c;
        im_o += a_orig[k] * s;
        re_w += a_warped[k] * c;
        im_w += a_warped[k] * s;
      }
      sum_mag_orig_sq += (re_o * re_o + im_o * im_o);
      sum_mag_warp_sq += (re_w * re_w + im_w * im_w);
    }
    if (sum_mag_orig_sq > 1e-12 && sum_mag_warp_sq > 1e-12) {
      return static_cast<float>(std::clamp(std::sqrt(sum_mag_warp_sq / sum_mag_orig_sq), 0.01, 10.0));
    }
    return 1.0f;
  }

  case FormantNormalizationStrategy::StrategyD_ResidualEnergy: {
    // Residual / Excitation energy: ratio of polynomial norms sum(a_k^2)
    float norm_orig = 0.0f, norm_warp = 0.0f;
    for (size_t i = 0; i <= order; ++i) {
      norm_orig += a_orig[i] * a_orig[i];
      norm_warp += a_warped[i] * a_warped[i];
    }
    if (norm_orig > 1e-6f && norm_warp > 1e-6f) {
      return std::clamp(std::sqrt(norm_warp / norm_orig), 0.01f, 10.0f);
    }
    return 1.0f;
  }

  default:
    return 1.0f;
  }
}

void SharedLpcAnalysis::publish(const SharedLpcModel &m) {
  const uint32_t serial=published_.load(std::memory_order_relaxed)+1;
  auto &p=models_[serial%kModelCount]; p.sequence.fetch_add(1,std::memory_order_acq_rel);
  for(size_t i=0;i<p.coefficients.size();++i)p.coefficients[i].store(m.coefficients[i],std::memory_order_relaxed);
  p.error.store(m.prediction_error); p.confidence.store(m.confidence);
  p.timestamp_low.store(uint32_t(m.timestamp)); p.timestamp_high.store(uint32_t(m.timestamp>>32));
  p.order.store(m.order); p.valid.store(m.valid?1:0); p.sequence.fetch_add(1,std::memory_order_release);
  published_.store(serial,std::memory_order_release);
}
size_t SharedLpcAnalysis::run(size_t maximum,const PitchResult &pitch) {
  size_t made=0; LpcSample s;
  while(made<maximum && fifo_.pop(s)){
    if(fill_<config_.window_size) frame_[fill_++]=s.value;
    else { std::move(frame_.begin()+1,frame_.begin()+config_.window_size,frame_.begin()); frame_[config_.window_size-1]=s.value; }
    if(fill_==config_.window_size && (++since_frame_>=config_.hop_size || telemetry_.lpc_frames==0)){
      since_frame_=0; SharedLpcModel m; m.timestamp=s.input_position-config_.window_size/2;
      const bool voiced=pitch.voiced && pitch.confidence>.25f;
      VF_PROFILE_BEGIN(profiler_, section(LpcProfileSection::Total));
      m.valid=config_.enabled && voiced && solve(frame_.data(),config_.window_size,config_.order,config_.preemphasis,&m);
      m.confidence*=std::clamp(pitch.confidence,0.0f,1.0f);
      if(!m.valid)++telemetry_.lpc_invalid_frames;
      telemetry_.max_prediction_error=std::max(telemetry_.max_prediction_error,m.prediction_error);
      publish(m); ++telemetry_.lpc_frames; ++made;
      publish_telemetry();
      VF_PROFILE_END(profiler_, section(LpcProfileSection::Total), 0);
    }
  }
  return made;
}
bool SharedLpcAnalysis::model_near(uint64_t ts,SharedLpcModel *out) const {
  if(!out) return false;
  const uint32_t newest=published_.load(std::memory_order_acquire);
  bool found=false; uint64_t best=UINT64_MAX;
  const uint32_t count=std::min<uint32_t>(newest,kModelCount);
  for(uint32_t k=0;k<count;++k){const auto &p=models_[(newest-k)%kModelCount]; const uint32_t before=p.sequence.load(std::memory_order_acquire); if(before&1)continue;
    SharedLpcModel m; for(size_t i=0;i<m.coefficients.size();++i)m.coefficients[i]=p.coefficients[i].load();
    m.prediction_error=p.error.load();m.confidence=p.confidence.load();m.timestamp=uint64_t(p.timestamp_low.load())|(uint64_t(p.timestamp_high.load())<<32);m.order=uint16_t(p.order.load());m.valid=p.valid.load()!=0;
    if(before!=p.sequence.load(std::memory_order_acquire)||!m.valid) continue;
    const uint64_t d=ts>m.timestamp?ts-m.timestamp:m.timestamp-ts;
    if(d<best){best=d;*out=m;found=true;}
  } return found && best<=uint64_t(config_.window_size+config_.hop_size);
}
ProfileStats SharedLpcAnalysis::profile(LpcProfileSection s) const{return profiler_.stats(section(s));}
void SharedLpcAnalysis::publish_telemetry() {
  auto store=[](std::atomic<uint32_t> &lo,std::atomic<uint32_t> &hi,uint64_t v){lo.store(uint32_t(v),std::memory_order_relaxed);hi.store(uint32_t(v>>32),std::memory_order_relaxed);};
  published_telemetry_.sequence.fetch_add(1,std::memory_order_acq_rel);
  store(published_telemetry_.frames_low,published_telemetry_.frames_high,telemetry_.lpc_frames);
  store(published_telemetry_.invalid_low,published_telemetry_.invalid_high,telemetry_.lpc_invalid_frames);
  store(published_telemetry_.fallback_low,published_telemetry_.fallback_high,telemetry_.lpc_fallback_frames);
  published_telemetry_.max_error.store(telemetry_.max_prediction_error,std::memory_order_relaxed);
  published_telemetry_.sequence.fetch_add(1,std::memory_order_release);
}
LpcTelemetry SharedLpcAnalysis::telemetry() const {
  LpcTelemetry result; uint32_t before,after;
  auto load=[](const std::atomic<uint32_t> &lo,const std::atomic<uint32_t> &hi){return uint64_t(lo.load(std::memory_order_relaxed))|(uint64_t(hi.load(std::memory_order_relaxed))<<32);};
  do { before=published_telemetry_.sequence.load(std::memory_order_acquire); if(before&1U){after=before;continue;}
    result.lpc_frames=load(published_telemetry_.frames_low,published_telemetry_.frames_high);
    result.lpc_invalid_frames=load(published_telemetry_.invalid_low,published_telemetry_.invalid_high);
    result.lpc_fallback_frames=load(published_telemetry_.fallback_low,published_telemetry_.fallback_high);
    result.max_prediction_error=published_telemetry_.max_error.load(std::memory_order_relaxed);
    after=published_telemetry_.sequence.load(std::memory_order_acquire);
  } while(before!=after || (after&1U)); return result;
}
