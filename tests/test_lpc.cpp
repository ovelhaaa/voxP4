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
  SharedLpcAnalysis analysis; LpcConfig c; CHECK(analysis.init(48000,c)); PitchResult p; p.voiced=true;p.confidence=1;
  analysis.tap(signal.data(),signal.size()); CHECK(analysis.run(4,p)>0); SharedLpcModel selected; CHECK(analysis.model_near(512,&selected));
  return failures?1:0;
}
