// atx::engine::alpha — P3d-D4: Chan kalman known-value fixture vs numpy reference.
//
// Third independent check for the `kalman(y, x, delta, R)` op beyond:
//   D2: registry + type-checker tests (alpha_member_test.cpp)
//   D3: VM-vs-oracle differential (alpha_recurrence_test.cpp::AlphaKalmanReg)
//
// This test drives the full DSL pipeline (parse -> analyze -> compile ->
// Engine::evaluate) with a 1-instrument, 8-date panel whose inputs and expected
// outputs were pre-computed by the Python reference implementation in
// scripts/gen_kalman_ref.py and committed as a C++ header.  The fixture is
// deterministic (hardcoded literals; no RNG) and self-contained (the test never
// invokes Python).
//
// Convention verified here (matches state_ops.hpp::kalman_reg_step):
//   - Diffuse prior: a=0, b=0, P=[[1,0],[0,1]].
//   - W = (delta/(1-delta)) * I2.
//   - Every finite (y,x) date runs the full Chan update from the prior.
//   - NaN obs: predict-only (P00+=w, P11+=w; a,b unchanged); outputs NaN.
//   - Outputs: alpha=a, beta=b, resid=e/sqrt(Q_inn).

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

// Committed fixture: kRefT, kRefX[], kRefY[], kRefAlpha[], kRefBeta[], kRefResid[].
#include "fixtures/kalman_reg_reference.hpp"

namespace {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::SignalSet;
using atx::engine::alpha::kalman_ref::kRefAlpha;
using atx::engine::alpha::kalman_ref::kRefBeta;
using atx::engine::alpha::kalman_ref::kRefResid;
using atx::engine::alpha::kalman_ref::kRefT;
using atx::engine::alpha::kalman_ref::kRefX;
using atx::engine::alpha::kalman_ref::kRefY;

// Tolerance for comparing VM doubles to the numpy-derived literals.
// Both sides use IEEE 754 double arithmetic with identical operation order, so
// the maximum absolute difference should be at the sub-ULP level (well below
// 1e-12 in practice); 1e-9 is a generous gate.
constexpr double kTol = 1e-9;

TEST(KalmanRef, MatchesNumpyReference) {
  Library lib;

  // Parse/analyze/compile the DSL program: three separate roots, one per pin.
  // A single shared KalmanReg compute instruction feeds all three outputs
  // (verified in D3; here we just consume the results).
  auto ast = parse_program("al = kalman(y, x, 0.0001, 0.001).alpha\n"
                           "be = kalman(y, x, 0.0001, 0.001).beta\n"
                           "re = kalman(y, x, 0.0001, 0.001).resid\n",
                           lib);
  ASSERT_TRUE(ast) << ast.error().message();

  auto an = analyze(ast.value());
  ASSERT_TRUE(an) << an.error().message();

  auto prog = compile(ast.value(), an.value());
  ASSERT_TRUE(prog) << prog.error().message();

  // Build a 1-instrument panel from the fixture arrays.
  // Panel::create(dates, instruments, fieldNames, fieldData, universe).
  // Field data is date-major: element t*instruments + j = value at (date t, instr j).
  // For 1 instrument, the flat vector equals the per-date sequence directly.
  const atx::usize dates = kRefT;
  const atx::usize instruments = 1;

  std::vector<atx::f64> y_vec(kRefY, kRefY + kRefT);
  std::vector<atx::f64> x_vec(kRefX, kRefX + kRefT);

  auto panel_res = Panel::create(dates, instruments,
                                 /*names=*/{"y", "x"},
                                 /*data=*/{y_vec, x_vec},
                                 /*universe=*/{});
  ASSERT_TRUE(panel_res) << panel_res.error().message();
  const Panel &panel = panel_res.value();

  // Evaluate with the fast VM.
  Engine eng(panel);
  auto out = eng.evaluate(prog.value());
  ASSERT_TRUE(out) << out.error().message();

  ASSERT_EQ(out.value().alphas.size(), 3U);

  const auto &vm_alpha = out.value().alphas[0].values;
  const auto &vm_beta = out.value().alphas[1].values;
  const auto &vm_resid = out.value().alphas[2].values;

  ASSERT_EQ(vm_alpha.size(), kRefT);
  ASSERT_EQ(vm_beta.size(), kRefT);
  ASSERT_EQ(vm_resid.size(), kRefT);

  for (atx::usize t = 0; t < kRefT; ++t) {
    EXPECT_NEAR(vm_alpha[t], kRefAlpha[t], kTol)
        << "alpha mismatch at t=" << t << "  vm=" << vm_alpha[t] << "  ref=" << kRefAlpha[t];
    EXPECT_NEAR(vm_beta[t], kRefBeta[t], kTol)
        << "beta mismatch at t=" << t << "  vm=" << vm_beta[t] << "  ref=" << kRefBeta[t];
    EXPECT_NEAR(vm_resid[t], kRefResid[t], kTol)
        << "resid mismatch at t=" << t << "  vm=" << vm_resid[t] << "  ref=" << kRefResid[t];
  }
}

} // namespace
