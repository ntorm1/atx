// atx::engine::factory — grammar-typed generation (S3.5).
//
// The headline: a type-targeted sampler emits genomes that are analyze-VALID by
// construction (rejection rate ≈ 0), measured against a type-BLIND control that
// ignores dtype/shape/role (high rejection rate). The delta is the unit's payoff.
// Plus: determinism (same seed ⇒ same expression) and a differential check that
// generated genomes actually run (oracle == VM), i.e. valid-by-construction holds
// end-to-end, not just through analyze.
//
// Naming: Subject_Condition_ExpectedResult.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/factory/generate.hpp"

namespace atxtest_factory_generate_test {

using atx::core::Xoshiro256pp;
using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;
using atx::engine::factory::GenConfig;
using atx::engine::factory::generate_control_expr;
using atx::engine::factory::generate_expr;
using atx::engine::factory::generate_genome;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// Task 7: grammar-generated genomes may contain ONLINE FP Ts ops (ts_mean /
// ts_std / ...) whose VM output is within a TIGHT TOLERANCE of — not bit-
// identical to — the batch oracle. cells_conform() keeps the NaN pattern exact
// and applies atol+rtol=1e-9 (bit-exact ops pass trivially).
inline constexpr atx::f64 kOnlineAtol = 1e-9;
inline constexpr atx::f64 kOnlineRtol = 1e-9;

// Node-type classification helpers for S3-4 acceptance tests.
// Returns the name of the DSL function at the root of `src` (or "" for infix).
// We classify by checking whether the expression starts with a known op prefix.
//
// Simpler approach: count how often the prefix of the emitted string belongs to
// a cs or ts op family. We do string-prefix matching on the outer call.
[[nodiscard]] bool starts_with_any(const std::string &s,
                                   std::initializer_list<std::string_view> prefixes) {
  for (auto p : prefixes) {
    if (s.rfind(std::string{p} + "(", 0) == 0) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool is_cs_or_ts_outer(const std::string &expr) {
  // cs-simple: rank, zscore, normalize, vec_sum, vec_avg
  // cs-scalar: scale, winsorize, quantile
  // cs-group:  indneutralize, group_rank, group_zscore, group_mean, group_count,
  //            group_scale, cs_residualize
  // ts-unary:  ts_mean, ts_std, ts_sum, ts_min, ts_max, delay, delta, ts_rank,
  //            decay_linear, ema
  // ts-binary: correlation, covariance
  return starts_with_any(expr, {"rank", "zscore", "normalize", "vec_sum", "vec_avg",
                                 "scale", "winsorize", "quantile",
                                 "indneutralize", "group_rank", "group_zscore",
                                 "group_mean", "group_count", "group_scale",
                                 "cs_residualize",
                                 "ts_mean", "ts_std", "ts_sum", "ts_min", "ts_max",
                                 "delay", "delta", "ts_rank", "decay_linear", "ema",
                                 "correlation", "covariance"});
}

[[nodiscard]] bool cells_conform(atx::f64 vm, atx::f64 oracle) noexcept {
  if (std::isnan(vm) && std::isnan(oracle)) {
    return true;
  }
  if (std::isnan(vm) != std::isnan(oracle)) {
    return false;
  }
  return std::fabs(vm - oracle) <= kOnlineAtol + kOnlineRtol * std::fabs(oracle);
}

// True iff `src` parses AND analyzes (i.e. the type-checker accepts it).
[[nodiscard]] bool accepts(const std::string &src) {
  auto ast = parse_expr(src, shared_lib());
  if (!ast) {
    return false;
  }
  return analyze(ast.value()).has_value();
}

// A panel carrying every field the generator can reference (so generated genomes
// can be compiled + evaluated). Numeric fields get arbitrary values; the group
// classifiers get small integer labels.
[[nodiscard]] Panel make_panel() {
  constexpr atx::usize dates = 6;
  constexpr atx::usize instruments = 5;
  constexpr atx::usize cells = dates * instruments;
  const std::vector<std::string> names = {
      "close",          "open",           "high",  "low", "volume", "vwap", "adv20",
      "IndClass.sector", "IndClass.industry", "IndClass.subindustry"};
  std::vector<std::vector<atx::f64>> cols(names.size(), std::vector<atx::f64>(cells));
  for (atx::usize f = 0; f < names.size(); ++f) {
    const bool is_group = f >= 7;
    for (atx::usize i = 0; i < cells; ++i) {
      cols[f][i] = is_group ? static_cast<atx::f64>(i % 3)
                            : 1.0 + static_cast<atx::f64>((i * (f + 1)) % 97);
    }
  }
  auto p = Panel::create(dates, instruments, std::vector<std::string>{names},
                         std::move(cols), {});
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// ===========================================================================
//  Rejection rate — the headline: typed ≈ 0, control high.
// ===========================================================================

TEST(GrammarGen, TypedAcceptsAll_ControlRejectsMany) {
  GenConfig cfg;
  constexpr int kN = 3000;
  Xoshiro256pp rng_typed(0x6311ULL);
  Xoshiro256pp rng_ctrl(0x6311ULL);
  int typed_rejected = 0;
  int ctrl_rejected = 0;
  for (int i = 0; i < kN; ++i) {
    if (!accepts(generate_expr(cfg, rng_typed))) {
      ++typed_rejected;
    }
    if (!accepts(generate_control_expr(cfg, rng_ctrl))) {
      ++ctrl_rejected;
    }
  }
  // Valid by construction: the type-targeted sampler is NEVER rejected.
  EXPECT_EQ(typed_rejected, 0) << "typed rejection rate = " << typed_rejected << "/" << kN;
  // The type-blind control is rejected at a materially higher rate (the yield
  // delta is the unit's headline — kept well clear of noise).
  EXPECT_GT(ctrl_rejected, kN / 4) << "control rejection rate = " << ctrl_rejected << "/" << kN;
  EXPECT_GT(ctrl_rejected, typed_rejected + kN / 4);
}

// ===========================================================================
//  Determinism — same seed ⇒ byte-identical expression (composes with S4).
// ===========================================================================

TEST(GrammarGen, SameSeed_SameExpression) {
  GenConfig cfg;
  Xoshiro256pp a(0xD1CEULL);
  Xoshiro256pp b(0xD1CEULL);
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(generate_expr(cfg, a), generate_expr(cfg, b)) << "divergence at " << i;
  }
}

// ===========================================================================
//  End-to-end — generated genomes compile + run, oracle == VM bit-for-bit.
// ===========================================================================

TEST(GrammarGen, GeneratedGenomes_RunOracleEqualsVm) {
  GenConfig cfg;
  const Panel panel = make_panel();
  Xoshiro256pp rng(0x9AA9ULL);
  int evaluated = 0;
  for (int i = 0; i < 300; ++i) {
    auto g = generate_genome(cfg, shared_lib(), rng);
    ASSERT_TRUE(g.has_value()) << "generator emitted an invalid genome: "
                               << (g ? "" : g.error().message());
    auto prog = compile(g->ast, g->analysis);
    ASSERT_TRUE(prog.has_value()) << "compile: " << (prog ? "" : prog.error().message());
    Engine engine{panel};
    auto vm = engine.evaluate(prog.value_or(Program{}));
    ASSERT_TRUE(vm.has_value()) << "VM: " << (vm ? "" : vm.error().message());
    auto ref = evaluate_reference(prog.value_or(Program{}), panel);
    ASSERT_TRUE(ref.has_value()) << "oracle: " << (ref ? "" : ref.error().message());
    const SignalSet &v = vm.value();
    const SignalSet &r = ref.value();
    ASSERT_EQ(v.alphas.size(), r.alphas.size());
    for (atx::usize a = 0; a < v.alphas.size(); ++a) {
      ASSERT_EQ(v.alphas[a].values.size(), r.alphas[a].values.size());
      for (atx::usize c = 0; c < v.alphas[a].values.size(); ++c) {
        EXPECT_TRUE(cells_conform(v.alphas[a].values[c], r.alphas[a].values[c]))
            << "genome " << i << " cell " << c << ": VM=" << v.alphas[a].values[c]
            << " oracle=" << r.alphas[a].values[c];
      }
    }
    ++evaluated;
  }
  EXPECT_EQ(evaluated, 300); // every generated genome was VM-safe
}


// ===========================================================================
//  S3-4 accept (b): non-uniform production_weights biased toward cs+ts
//  increases the frequency of cross-sectional and time-series outer nodes.
// ===========================================================================

TEST(GrammarGenS34, BiasedWeights_CsTsMoreFrequent) {
  // Case indices in gen_f64's switch:
  //   0=unary-ewise  1=binary-arith  2=cs-simple  3=cs-scalar
  //   4=cs-group     5=ts-unary      6=ts-binary   7=negate
  // Bias: zero weight to cases 0,1,7 (unary/binary/negate); full weight to 2-6 (cs+ts).
  // With this cfg ALL top-level non-leaf nodes should be cs or ts.
  GenConfig uniform_cfg;

  GenConfig biased_cfg;
  // Zero out non-cs/ts cases; only cases 2-6 have weight.
  // REQUIRES: GenConfig has production_weights field (std::array<f64,8>).
  biased_cfg.production_weights = {0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0};

  constexpr int kN = 1000;
  Xoshiro256pp rng_uniform(0xBEEF1234ULL);
  Xoshiro256pp rng_biased(0xBEEF1234ULL);

  int uniform_cs_ts = 0;
  int biased_cs_ts = 0;

  for (int i = 0; i < kN; ++i) {
    const std::string u = generate_expr(uniform_cfg, rng_uniform);
    const std::string b = generate_expr(biased_cfg, rng_biased);
    if (is_cs_or_ts_outer(u)) {
      ++uniform_cs_ts;
    }
    if (is_cs_or_ts_outer(b)) {
      ++biased_cs_ts;
    }
  }

  // With biased weights, cs+ts should appear far more often.
  // Uniform: ~5/8 of non-leaf cases (cases 2-6 out of 8), but many leaves.
  // Biased: 100% of non-leaf cases → much higher cs+ts fraction.
  EXPECT_GT(biased_cs_ts, uniform_cs_ts)
      << "biased=" << biased_cs_ts << " uniform=" << uniform_cs_ts;
  // With 100% weight on cs+ts, we expect at least 50% of 1000 to be cs/ts outer.
  EXPECT_GT(biased_cs_ts, kN / 2)
      << "biased cs+ts fraction too low: " << biased_cs_ts << "/" << kN;
}

// ===========================================================================
//  S3-4 accept (c): wider scalar_pool generates values outside the default set.
// ===========================================================================

TEST(GrammarGenS34, WiderScalarPool_GeneratesNonDefaultScalars) {
  GenConfig cfg;
  // REQUIRES: GenConfig has scalar_pool field (std::vector<std::string_view>).
  // Add a value not in the default pool {"0.5","1.5","2.0","3.0"}.
  // string_view is non-owning: literals have static lifetime, safe here.
  cfg.scalar_pool = {"0.5", "1.5", "2.0", "3.0", "5.0", "10.0"};

  constexpr int kN = 3000;
  Xoshiro256pp rng(0xCAFE5678ULL);

  // Collect all emitted scalars by looking for "scale(" / "winsorize(" /
  // "quantile(" patterns (cs-scalar case, case 3). We check the full expression
  // for the new values appearing as substrings.
  bool found_new_scalar = false;
  for (int i = 0; i < kN && !found_new_scalar; ++i) {
    const std::string expr = generate_expr(cfg, rng);
    // 5.0 or 10.0 should eventually appear in the expression.
    if (expr.find("5.0") != std::string::npos || expr.find("10.0") != std::string::npos) {
      found_new_scalar = true;
    }
  }
  EXPECT_TRUE(found_new_scalar)
      << "Wider scalar_pool should produce scalars outside default set within " << kN << " trials";
}

// ===========================================================================
//  S3-4 accept (d): two runs with non-default weights produce identical results.
// ===========================================================================

TEST(GrammarGenS34, NonDefaultWeights_TwoRunsIdentical) {
  GenConfig cfg;
  cfg.production_weights = {0.0, 0.0, 2.0, 1.0, 1.0, 2.0, 1.0, 0.0};
  cfg.scalar_pool        = {"0.1", "0.25", "0.5", "1.0", "2.0"};

  constexpr int kN = 200;
  Xoshiro256pp rng_a(0xDEAD1111ULL);
  Xoshiro256pp rng_b(0xDEAD1111ULL);

  for (int i = 0; i < kN; ++i) {
    const std::string a = generate_expr(cfg, rng_a);
    const std::string b = generate_expr(cfg, rng_b);
    EXPECT_EQ(a, b) << "divergence at i=" << i;
  }
}

// ===========================================================================
//  S3-4 accept (a) coverage: explicit defaults equal implicit defaults.
//  Setting production_weights/scalar_pool explicitly to their default values
//  must produce the same RNG stream as a default-constructed GenConfig — a
//  quick local sanity check. The real pre-S3-4 byte-identity proof lives in
//  the NsgaSearch / MultiObjective golden digest tests.
// ===========================================================================

TEST(GrammarGenS34, DefaultConfig_ExplicitEqualsImplicitDefaults) {
  GenConfig cfg_a; // implicit defaults
  GenConfig cfg_b; // explicitly set to the same default values
  cfg_b.production_weights = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  cfg_b.scalar_pool        = {"0.5", "1.5", "2.0", "3.0"};

  Xoshiro256pp rng_a(0xF00D4321ULL);
  Xoshiro256pp rng_b(0xF00D4321ULL);

  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(generate_expr(cfg_a, rng_a), generate_expr(cfg_b, rng_b))
        << "explicit vs implicit defaults diverged at i=" << i;
  }
}

// ===========================================================================
//  S3-4 UB guard: an all-zero production_weights array (a misconfigured opt-in
//  knob) must NOT invoke division-by-zero UB. weighted_case falls back to the
//  uniform `raw % 8` path, so generation still succeeds and is deterministic.
//  (In a debug build ATX_ASSERT(total >= 1) would abort first; this test
//  documents the release fail-safe — we run it in whatever config the test
//  binary is built with and assert no UB / valid output either way.)
// ===========================================================================

TEST(GrammarGenS34, AllZeroWeights_DoesNotUbAndStaysDeterministic) {
  GenConfig cfg;
  // All weights round to 0 → rounded sum == 0. The release fallback must kick
  // in (raw % 8) rather than computing `raw % 0`.
  cfg.production_weights = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

#ifdef NDEBUG
  // Release: ATX_ASSERT is a no-op, so the fallback path executes. Verify it
  // does not crash and is reproducible (two seeded runs agree).
  Xoshiro256pp rng_a(0x012345ULL);
  Xoshiro256pp rng_b(0x012345ULL);
  for (int i = 0; i < 100; ++i) {
    const std::string a = generate_expr(cfg, rng_a);
    const std::string b = generate_expr(cfg, rng_b);
    EXPECT_EQ(a, b) << "all-zero-weight fallback diverged at i=" << i;
    EXPECT_FALSE(a.empty());
  }
#else
  // Debug: ATX_ASSERT(total >= 1) aborts. EXPECT_DEATH confirms the fail-loud
  // contract (the matcher is loose — the abort message is implementation-defined).
  Xoshiro256pp rng(0x012345ULL);
  EXPECT_DEATH({ static_cast<void>(generate_expr(cfg, rng)); }, ".*");
#endif
}

}  // namespace atxtest_factory_generate_test
