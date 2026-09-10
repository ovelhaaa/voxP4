#include "lpc.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int failures;
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);++failures;} } while(0)

int main(){
  constexpr size_t n=1024;
  std::vector<float> signal(n),residual(n),restored(n);
  for(size_t i=0;i<n;++i) signal[i]=.3f*std::sin(2*3.14159265358979323846*220*i/48000)+.1f*std::sin(2*3.14159265358979323846*730*i/48000);
  for(uint16_t order:{10,12,16,20}){
    SharedLpcModel m; CHECK(SharedLpcAnalysis::solve(signal.data(),n,order,.97f,&m)); CHECK(m.valid); CHECK(std::isfinite(m.prediction_error));
    std::vector<float> state(order);
    for(size_t i=0;i<n;++i){residual[i]=signal[i];for(size_t j=1;j<=order && j<=i;++j)residual[i]+=m.coefficients[j]*signal[i-j];float y=residual[i];for(size_t j=1;j<=order;++j)y-=m.coefficients[j]*state[j-1];for(size_t j=order-1;j>0;--j)state[j]=state[j-1];state[0]=y;restored[i]=y;}
    double err=0,energy=0;for(size_t i=0;i<n;++i){err+=(restored[i]-signal[i])*(restored[i]-signal[i]);energy+=signal[i]*signal[i];} CHECK(std::sqrt(err/energy)<1e-4);
  }
  std::vector<float> silence(n); SharedLpcModel bad; CHECK(!SharedLpcAnalysis::solve(silence.data(),n,16,.97f,&bad));
  std::vector<float> oversized(1025); CHECK(!SharedLpcAnalysis::solve(oversized.data(),oversized.size(),16,.97f,&bad));
  SharedLpcAnalysis analysis; LpcConfig c; CHECK(analysis.init(48000,c)); PitchResult p; p.voiced=true;p.confidence=1;
  analysis.tap(signal.data(),signal.size()); CHECK(analysis.run(4,p)>0); SharedLpcModel selected; CHECK(analysis.model_near(512,&selected));

  // Test lambda_from_semitones
  CHECK(std::fabs(SharedLpcAnalysis::lambda_from_semitones(0.0f)) < 1e-6f);
  CHECK(SharedLpcAnalysis::lambda_from_semitones(7.0f) < -0.1f);
  CHECK(SharedLpcAnalysis::lambda_from_semitones(-7.0f) > 0.1f);

  // Test warp_polynomial
  SharedLpcModel m16;
  CHECK(SharedLpcAnalysis::solve(signal.data(), n, 16, 0.97f, &m16));
  std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1> warped{};

  // Identity warp
  CHECK(SharedLpcAnalysis::warp_polynomial(m16.coefficients.data(), 16, 0.0f, 1.0f, warped.data()));
  for (size_t i = 0; i <= 16; ++i) {
    CHECK(std::fabs(warped[i] - m16.coefficients[i]) < 1e-5f);
  }

  // Bandwidth expansion only
  CHECK(SharedLpcAnalysis::warp_polynomial(m16.coefficients.data(), 16, 0.0f, 0.985f, warped.data()));
  CHECK(std::fabs(warped[0] - 1.0f) < 1e-6f);
  CHECK(std::fabs(warped[1] - m16.coefficients[1] * 0.985f) < 1e-5f);

  // All-pass frequency warping (up and down)
  const float lam_up = SharedLpcAnalysis::lambda_from_semitones(7.0f);
  CHECK(SharedLpcAnalysis::warp_polynomial(m16.coefficients.data(), 16, lam_up, 0.985f, warped.data()));
  CHECK(std::fabs(warped[0] - 1.0f) < 1e-5f);
  for (size_t i = 0; i <= 16; ++i) {
    CHECK(std::isfinite(warped[i]));
  }

  const float lam_down = SharedLpcAnalysis::lambda_from_semitones(-7.0f);
  CHECK(SharedLpcAnalysis::warp_polynomial(m16.coefficients.data(), 16, lam_down, 0.985f, warped.data()));
  CHECK(std::fabs(warped[0] - 1.0f) < 1e-5f);
  for (size_t i = 0; i <= 16; ++i) {
    CHECK(std::isfinite(warped[i]));
  }

  // Test identity for pitch=0, formant=0
  const float lam_zero = SharedLpcAnalysis::lambda_from_semitones(0.0f);
  CHECK(std::fabs(lam_zero) < 1e-7f);
  CHECK(SharedLpcAnalysis::warp_polynomial(m16.coefficients.data(), 16, lam_zero, 1.0f, warped.data()));
  for (size_t i = 0; i <= 16; ++i) {
    CHECK(std::fabs(warped[i] - m16.coefficients[i]) < 1e-6f);
  }

  // Monotonicity test of bilinear warping across frequencies
  for (float st : {-12.0f, -7.0f, -5.0f, -3.0f, 0.0f, 3.0f, 4.0f, 7.0f, 12.0f}) {
    const float lam = SharedLpcAnalysis::lambda_from_semitones(st);
    float prev_theta = -1.0f;
    for (int k = 0; k <= 100; ++k) {
      const float w = static_cast<float>(k) * 3.14159265358979323846f / 100.0f;
      const float num = std::sin(w) * (1.0f - lam * lam);
      const float den = std::cos(w) * (1.0f + lam * lam) - 2.0f * lam;
      float theta = std::atan2(num, den);
      if (theta < 0.0f) theta += 2.0f * 3.14159265358979323846f;
      if (k == 0) {
        CHECK(std::fabs(theta) < 1e-5f);
      } else if (k == 100) {
        CHECK(std::fabs(theta - 3.14159265358979323846f) < 1e-5f);
      } else {
        CHECK(theta > prev_theta);
      }
      prev_theta = theta;
    }
  }

  return failures?1:0;
}

