#include "scale.h"
#include <array>
#include <cmath>
#include <limits>

namespace {
constexpr std::array<int, 7> major{0,2,4,5,7,9,11};
constexpr std::array<int, 7> minor{0,2,3,5,7,8,10};
const std::array<int,7> &steps(ScaleType t) { return t == ScaleType::Major ? major : minor; }
int floor_div(int a, int b) { int q=a/b, r=a%b; return r < 0 ? q-1 : q; }
}
int chromatic_class(int note) { int r=note%12; return r < 0 ? r+12 : r; }
bool scale_contains(const ScaleConfig &s, int note) {
  const int relative=chromatic_class(note-static_cast<int>(s.root%12));
  for (int x:steps(s.type)) if (x==relative) return true;
  return false;
}
int nearest_scale_note(const ScaleConfig &s, float note) {
  int best=static_cast<int>(std::lround(note)); float cost=std::numeric_limits<float>::max();
  const int center=best;
  for(int n=center-2;n<=center+2;++n) if(scale_contains(s,n)) {
    float c=std::fabs(note-n); if(c<cost){cost=c;best=n;}
  }
  return best;
}
int transpose_scale_degrees(const ScaleConfig &s, int note, int degrees) {
  const auto &v=steps(s.type); int octave=floor_div(note-static_cast<int>(s.root%12),12);
  const int rel=chromatic_class(note-static_cast<int>(s.root%12)); int degree=0;
  for(size_t i=0;i<v.size();++i) if(std::abs(v[i]-rel)<std::abs(v[degree]-rel)) degree=static_cast<int>(i);
  const int total=degree+degrees; octave+=floor_div(total,7);
  const int wrapped=total-7*floor_div(total,7);
  return static_cast<int>(s.root%12)+12*octave+v[static_cast<size_t>(wrapped)];
}

