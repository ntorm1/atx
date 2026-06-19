// atx::engine::factory — NSGA-II multi-objective SearchDriver tests (S4.1, §4.1).
//
// Three load-bearing checks:
//   (a) BOUNDARY PIN — with objective_mode == ScalarRaw, novelty off, seed 777,
//       the frozen 96x6 fixture reproduces the pre-S4 golden digest
//       0xa83f0d3e0b41a18d BYTE-IDENTICALLY. If an S4 edit changes this, the pin
//       is broken (fix the code, never the constant).
//   (b) DETPOOL INVARIANCE — the ScalarRaw digest is identical across {1,2,4,8}
//       DetPool workers (F2: worker count never enters the math).
//   (c) MULTI-OBJECTIVE NON-VACUOUS — a 2-objective panel where a high-wq/low-
//       diversify candidate and a low-wq/high-diversify candidate CO-OCCUPY Pareto
//       front 0 (front size > 1), proving the scalar collapse is genuinely
//       replaced; and MultiObjective selection diverges from ScalarRaw on the
//       frozen fixture (the mode actually changes which genomes survive).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/pareto.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace {

using atx::f64;
using atx::u16;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::combine::AlphaStore;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::fast_nondominated_sort;
using atx::engine::factory::ObjectiveMode;
using atx::engine::factory::ObjMatrix;
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchResult;

// The frozen pre-S4 boundary-pin digest (captured by the zzz_golden_capture
// harness on the fixture below; see the sprint brief). ScalarRaw + novelty off +
// seed 777 MUST reproduce this byte-for-byte.
constexpr atx::u64 kGoldenDigest = 0xa83f0d3e0b41a18dULL;

// ===========================================================================
//  Frozen fixture — lifted VERBATIM from zzz_golden_capture_test.cpp.
// ===========================================================================
[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

[[nodiscard]] Panel make_panel(usize dates, usize insts, std::vector<std::string> fields,
                               std::vector<std::vector<f64>> cols) {
  auto r = Panel::create(dates, insts, std::move(fields), std::move(cols), {});
  EXPECT_TRUE(r.has_value());
  return std::move(r.value());
}

struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

[[nodiscard]] std::vector<f64> noisy_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> drift(insts);
  for (usize j = 0; j < insts; ++j) {
    drift[j] = 0.006 - 0.0024 * static_cast<f64>(j);
  }
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + 0.010 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

[[nodiscard]] Panel fixture_panel(usize dates, usize insts) {
  const std::vector<f64> close = noisy_close(dates, insts, 0xA11Cu);
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  return make_panel(dates, insts, {"close", "rev"}, {close, rev});
}

[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)",
          "rank(rev)",
          "ts_mean(close, 5)",
          "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))",
          "delta(close, 2)"};
}

// The frozen boundary-pin config: ScalarRaw + every quality knob at its LEGACY
// value, so kGoldenDigest stays byte-identical as new knobs are added. Each new
// task appends ONE line here pinning its knob's legacy value.
[[nodiscard]] SearchConfig legacy_pin_cfg(atx::u64 seed) {
  SearchConfig c;
  c.master_seed = seed;
  c.population = 16;
  c.generations = 5;
  c.elites = 2;
  c.k_tournament = 3;
  c.p_cross = 0.5;
  c.objective_mode = ObjectiveMode::ScalarRaw;   // legacy ranking
  c.enable_behavioral_novelty = false;           // Task 1: novelty off
  c.enable_parsimony = false;                    // Task 2: parsimony off on the boundary pin
  c.seed_from_grammar = false;                   // Task 3: legacy cycle-fill on the boundary pin
  c.n_immigrants = 0;          // Task 4: no immigrants on the boundary pin
  c.stagnation_patience = 0;   // Task 4: full budget on the boundary pin
  // (Task 5 appends: c.adaptive_operators = false; c.jitter_anneal = false;)
  return c;
}

// ---------------------------------------------------------------------------
//  (a) Boundary pin.
// ---------------------------------------------------------------------------
TEST(NsgaSearch, ScalarRaw_ReproducesGoldenDigest) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  AlphaStore pool{};
  const SearchResult r = driver.run(legacy_pin_cfg(777), pool);
  EXPECT_EQ(r.digest, kGoldenDigest)
      << "ScalarRaw boundary pin broke: an S4 edit perturbed the pre-S4 path.";
}

// ---------------------------------------------------------------------------
//  (b) DetPool worker-count invariance (ScalarRaw).
// ---------------------------------------------------------------------------
TEST(NsgaSearch, ScalarRaw_DigestInvariantAcrossWorkers) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  const std::array<usize, 4> worker_counts{1, 2, 4, 8};
  atx::u64 first_digest = 0;
  for (usize wi = 0; wi < worker_counts.size(); ++wi) {
    SearchConfig cfg = legacy_pin_cfg(777);
    cfg.n_workers = worker_counts[wi];
    AlphaStore pool{};
    const SearchResult r = driver.run(cfg, pool);
    if (wi == 0) {
      first_digest = r.digest;
      EXPECT_EQ(first_digest, kGoldenDigest);
    } else {
      EXPECT_EQ(r.digest, first_digest) << "digest changed at n_workers=" << worker_counts[wi];
    }
  }
}

// ---------------------------------------------------------------------------
//  (c) Multi-objective non-vacuous: front 0 size > 1.
// ---------------------------------------------------------------------------
TEST(NsgaSearch, MultiObjective_FrontZeroHoldsTradeOffPair) {
  // Two objectives (wq, diversify), maximization. A high-wq/low-diversify and a
  // low-wq/high-diversify candidate must BOTH be non-dominated (front 0) — the
  // exact co-occupancy NSGA-II preserves and scalar-raw collapse would destroy.
  //   g0 high wq, low diversify
  //   g1 low wq, high diversify   (trade-off with g0 -> both front 0)
  //   g2 dominated by both        -> front 1
  const std::vector<f64> data{
      0.9,  0.1, // g0
      0.1,  0.9, // g1
      0.05, 0.05 // g2
  };
  const ObjMatrix obj{data, 3, 2};
  const std::vector<usize> canon{0, 1, 2};
  const std::vector<u16> fronts = fast_nondominated_sort(obj, canon);
  // g0 and g1 co-occupy front 0 (the non-vacuous trade-off); g2 is dominated.
  EXPECT_EQ(fronts[0], 0u);
  EXPECT_EQ(fronts[1], 0u);
  EXPECT_GT(fronts[2], 0u);
  usize front0_size = 0;
  for (const u16 f : fronts) {
    if (f == 0u) {
      ++front0_size;
    }
  }
  EXPECT_GT(front0_size, 1u) << "front 0 must be multi-membered (non-vacuous)";
}

// ---------------------------------------------------------------------------
//  (c') The objective_mode actually changes selection on the frozen fixture:
//       MultiObjective diverges from ScalarRaw (different survivors -> digest).
//       (Both are internally deterministic; we only assert they are NOT equal,
//       proving the NSGA path is live and not a no-op alias of the scalar path.)
// ---------------------------------------------------------------------------
TEST(NsgaSearch, MultiObjective_DivergesFromScalarRaw) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  SearchConfig scalar = legacy_pin_cfg(777);
  AlphaStore pool_s{};
  const SearchResult rs = driver.run(scalar, pool_s);

  SearchConfig multi = legacy_pin_cfg(777);
  multi.objective_mode = ObjectiveMode::MultiObjective;
  multi.seed_from_grammar = false; // hold gen-0 fixed; only the selection mode differs
  AlphaStore pool_m{};
  const SearchResult rm = driver.run(multi, pool_m);

  // Both runs are internally deterministic; the multi-objective survivor set is
  // genuinely different from the scalar collapse, so at least one of digest /
  // admitted ordering diverges.
  const bool diverged =
      (rm.digest != rs.digest) || (rm.admitted_candidates.size() != rs.admitted_candidates.size());
  EXPECT_TRUE(diverged || rm.trial_count != rs.trial_count)
      << "MultiObjective produced an identical run to ScalarRaw — NSGA path inert.";
}

// ---------------------------------------------------------------------------
//  (b') MultiObjective is itself worker-count-invariant (the F2 contract holds
//       for the new selection path too).
// ---------------------------------------------------------------------------
TEST(NsgaSearch, MultiObjective_DigestInvariantAcrossWorkers) {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};

  const std::array<usize, 4> worker_counts{1, 2, 4, 8};
  atx::u64 first_digest = 0;
  for (usize wi = 0; wi < worker_counts.size(); ++wi) {
    SearchConfig cfg = legacy_pin_cfg(777);
    cfg.objective_mode = ObjectiveMode::MultiObjective;
    cfg.seed_from_grammar = true; // exercise the generation wire under invariance
    cfg.n_workers = worker_counts[wi];
    AlphaStore pool{};
    const SearchResult r = driver.run(cfg, pool);
    if (wi == 0) {
      first_digest = r.digest;
    } else {
      EXPECT_EQ(r.digest, first_digest)
          << "MultiObjective digest changed at n_workers=" << worker_counts[wi];
    }
  }
}

} // namespace
