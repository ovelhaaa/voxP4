// B4C.7 host tests: exact source-grain/slice identity and reuse audit.
#include "td_psola.h"
#include "vocal_fx_types.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void require(bool ok, const char *msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    std::exit(1);
  }
}

SourceGrainKey make_key(uint64_t center, uint32_t half, uint64_t ts,
                        uint16_t order) {
  SourceGrainKey k;
  k.mark_center = center;
  k.half_window = half;
  k.lpc_timestamp = ts;
  k.lpc_order = order;
  k.lambda_bits = 0x3F800000U;
  k.gamma_bits = 0x3F7FFFFFU;
  k.norm_strategy = 2;
  return k;
}

void test_key_identity() {
  const SourceGrainKey a = make_key(1000, 200, 7, 16);
  const SourceGrainKey b = make_key(1000, 200, 7, 16);
  require(a == b, "identical keys equal");
  for (int f = 0; f < 7; ++f) {
    SourceGrainKey c = b;
    switch (f) {
      case 0: c.mark_center++; break;
      case 1: c.half_window++; break;
      case 2: c.lpc_timestamp++; break;
      case 3: c.lpc_order++; break;
      case 4: c.lambda_bits++; break;
      case 5: c.gamma_bits++; break;
      case 6: c.norm_strategy++; break;
    }
    require(!(a == c), "differing field breaks identity");
  }
}

void test_slice_recorder_disabled() {
  SharedPitchShiftResources r;
  r.reset_slice_audit();
  // Disabled by default: records are ignored.
  r.record_slice_render(make_key(1, 2, 3, 16), 0, 10, 0, 11, 176);
  require(r.slice_audit_requested_ == 0, "disabled recorder ignores");
}

void test_slice_recorder_cross_voice() {
  SharedPitchShiftResources r;
  r.reset_slice_audit();
  r.set_slice_audit_enabled(true);
  r.next_slice_audit_block();
  const SourceGrainKey k = make_key(5000, 300, 9, 16);
  r.record_slice_render(k, -10, 10, 0, 21, 336);  // V0 slice
  r.record_slice_render(k, -10, 10, 1, 21, 336);  // V1 identical slice
  require(r.slice_audit_requested_ == 2, "two slices requested");
  require(r.slice_audit_duplicates_ == 1, "one duplicate");
  require(r.slice_audit_cross_voice_ == 1, "cross-voice counted");
  require(r.slice_audit_same_voice_ == 0, "no same-voice");
  require(r.slice_audit_reusable_samples_ == 21, "reusable samples");
  require(r.slice_audit_reusable_taps_ == 336, "reusable taps");
  require(r.slice_audit_taps_ == 672, "total taps");
  // Same key but different clip is a different exact identity.
  r.record_slice_render(k, -10, 11, 1, 22, 352);
  require(r.slice_audit_requested_ == 3, "third slice requested");
  require(r.slice_audit_duplicates_ == 1, "clip change is unique");
  // Same voice repeat is same-voice reuse.
  r.record_slice_render(k, -10, 10, 0, 21, 336);
  require(r.slice_audit_same_voice_ == 1, "same-voice counted");
  require(r.slice_audit_duplicates_ == 2, "two duplicates");
  // Next block: prior entries must not match (generation guard).
  r.next_slice_audit_block();
  r.record_slice_render(k, -10, 10, 1, 21, 336);
  require(r.slice_audit_duplicates_ == 2, "no cross-block match");
  require(r.slice_audit_requested_ == 5, "five requested total");
}

void test_slice_tap_math() {
  // FIR taps per slice = slice_samples * order; non-LPC slices carry 0.
  SharedPitchShiftResources r;
  r.reset_slice_audit();
  r.set_slice_audit_enabled(true);
  r.next_slice_audit_block();
  r.record_slice_render(make_key(1, 100, 1, 16), 0, 63, 0, 64, 64 * 16);
  r.record_slice_render(make_key(2, 100, 1, 0), 0, 63, 1, 64, 0);
  require(r.slice_audit_taps_ == 1024, "tap accumulation");
  require(r.slice_audit_samples_ == 128, "sample accumulation");
}

void test_audit_struct_defaults() {
  PsolaSourceGrainAudit a;
  require(a.reuse_ratio_pct() == 0.0, "empty reuse 0");
  require(a.weighted_reusable_samples_pct() == 0.0, "empty weighted 0");
  B4C7SliceAuditSnapshot s;
  require(s.slices_requested == 0 && s.fir_taps == 0, "snapshot zero-init");
}

}  // namespace

int main() {
  std::printf("=== B4C.7 reuse-audit host tests ===\n");
  test_key_identity();
  test_slice_recorder_disabled();
  test_slice_recorder_cross_voice();
  test_slice_tap_math();
  test_audit_struct_defaults();
  std::printf("=== ALL B4C.7 REUSE TESTS PASSED ===\n");
  return 0;
}
