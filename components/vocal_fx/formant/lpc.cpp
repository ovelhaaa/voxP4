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
}
void SharedLpcAnalysis::tap(const float *x, size_t n) {
  for (size_t i=0;i<n;++i)
    (void)fifo_.push({std::isfinite(x[i]) ? x[i] : 0.0f, last_position_++});
}
bool SharedLpcAnalysis::solve(const float *x, size_t n, uint16_t order,
                              float pre, SharedLpcModel *out) {
  if (!x || !out || n < static_cast<size_t>(order) + 2 ||
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
  size_t made=0; AnalysisSample s;
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
