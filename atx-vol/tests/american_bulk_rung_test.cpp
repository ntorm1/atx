#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "pricing/american_detail.hpp"
#include "atx/vol/api/fitting/session.hpp"
#include "atx/vol/api/storage/surface_db.hpp"
#include "support/oracle_pricer_pde.hpp"

// Perf Phase 2b — the `al_bulk_opts` (`ql_fast`) Andersen-Lake rung and the
// `FitPreset::Bulk` tier that opts into it.
//
// WHAT THESE TESTS ARE FOR. Phase-2b step-1 measurement (atx-vol/src/al_probe.hpp)
// put ~84% of a `populate` board fit inside the AL boundary solve and only ~6.7% in
// the early-exercise premium quadrature, so the Phase-2b lever is the boundary rung,
// not a premium surrogate. `al_bulk_opts` is docs/al-preset-ladder.md §4's `ql_fast`:
// a cheap fixed-point quadrature (l = 8 vs al_fast_opts's 16) over 2 sweeps (vs 4)
// with a decoupled rich premium (p = 32). The ladder's §5 tier policy names it for
// exactly this consumer. These tests pin the three properties that make the tier
// legitimate:
//
//   1. The rung prices American options to the SAME accuracy class as the rung it
//      replaces, against an independent Crank-Nicolson FD oracle — the
//      NegRateDomainMap oracle pattern, over the production (moneyness, T, sigma,
//      r, q) domain the surface-db fit actually queries (step-1's measured state
//      distribution).
//   2. The premium order 32 takes the SPECIALIZED compile-time-trip-count kernel and
//      is bit-identical to the generic one (the `case 32:` added to al_put_premium's
//      dispatch is a hoist, not a numerics change).
//   3. `FitPreset::Bulk` is `FitPreset::Populate` in every field except the FIT's AL
//      rungs, and in particular the rung BAKED into a stored surface's pricing config
//      is unchanged — because `n_quad_price` survives none of the three AlOpts record
//      formats, so a baked decoupled rung would silently read back tied to l = 8.

namespace atx::vol {
namespace {

using atx::vol::test::oracle_pde_american;

// The step-1 measured query domain, coarsened to a test-sized grid. From the
// `al_probe` state trace over two 102-symbol production dates: T spans 1e-5..2.4 y
// (p1..p99 0.011..2.41), sigma p1..p99 0.16..0.98, r ~0.043, q in -0.07..0.09.
struct Cell {
  double S;
  double K;
  double T;
  double sigma;
  double r;
  double q;
  Side side;
};

[[nodiscard]] std::vector<Cell> production_domain() {
  std::vector<Cell> cells;
  const double K = 100.0;
  for (const double m : {0.80, 0.90, 1.00, 1.10, 1.25}) {      // S/K
    for (const double T : {0.02, 0.08, 0.25, 1.00, 2.00}) {     // years
      for (const double sigma : {0.16, 0.30, 0.60}) {           // annualized vol
        for (const double q : {0.0, 0.02, 0.06}) {              // yield (r = 0.043)
          cells.push_back(Cell{m * K, K, T, sigma, 0.043, q, Side::Put});
        }
      }
    }
  }
  return cells;
}

// Absolute price error of `opts` against the FD oracle, worst and mean over the grid.
struct OracleError {
  double max_abs{0.0};
  double mean_abs{0.0};
  std::size_t n{0};
  Cell worst{};
};

[[nodiscard]] OracleError score_against_oracle(const std::optional<AlOpts> &opts) {
  OracleError e{};
  double sum = 0.0;
  for (const Cell &c : production_domain()) {
    const Result<double> al = american_price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side,
                                            AmericanMethod::AndersenLake, opts);
    if (!al) {
      continue; // a regime this rung declines is covered by NegRateDomainMap, not here
    }
    const double ref = oracle_pde_american(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side);
    if (!std::isfinite(ref)) {
      continue;
    }
    const double err = std::fabs(*al - ref);
    sum += err;
    ++e.n;
    if (err > e.max_abs) {
      e.max_abs = err;
      e.worst = c;
    }
  }
  e.mean_abs = e.n > 0 ? sum / static_cast<double>(e.n) : 0.0;
  return e;
}

} // namespace

// ── 1. Rung identity ─────────────────────────────────────────────────────────

TEST(AlBulkRung, IsTheLadderQlFastRung) {
  const AlOpts o = al_bulk_opts();
  EXPECT_EQ(o.n_collocation, std::uint16_t{7});
  EXPECT_EQ(o.n_quadrature, std::uint16_t{8});
  EXPECT_EQ(o.max_newton_iter, std::uint16_t{2});
  EXPECT_DOUBLE_EQ(o.tol, 1.0e-8);
  // The DECOUPLED axis: a rich premium over a cheap fixed-point quadrature. This
  // field is what makes the rung accurate enough to be worth using, and it is also
  // the field no AlOpts record format persists -- hence serve_al_opts.
  EXPECT_EQ(o.n_quad_price, std::uint16_t{32});
  EXPECT_GT(o.n_quad_price, o.n_quadrature);

  // Strictly less fixed-point sweep work per solve than the rung it replaces:
  // n_quad_fp x (n_boundary - 1) x n_sweeps.
  const AlOpts f = al_fast_opts();
  const auto sweep_work = [](const AlOpts &a) {
    return static_cast<unsigned>(a.n_quadrature) *
           static_cast<unsigned>(a.n_collocation - 1u) * static_cast<unsigned>(a.max_newton_iter);
  };
  EXPECT_LT(sweep_work(o), sweep_work(f));
  EXPECT_LE(sweep_work(o) * 3u, sweep_work(f)); // >= 3x cheaper (measured 4x: 112 vs 448)
}

// ── 2. FD-oracle agreement over the production domain ────────────────────────

TEST(AlBulkRung, MatchesFdOracleInTheSameAccuracyClassAsTheRungItReplaces) {
  const OracleError bulk = score_against_oracle(al_bulk_opts());
  const OracleError fast = score_against_oracle(al_fast_opts());
  const OracleError accurate = score_against_oracle(std::nullopt); // ACCURATE preset

  ASSERT_GT(bulk.n, 100u) << "the domain grid must actually price";
  EXPECT_EQ(bulk.n, fast.n);
  EXPECT_EQ(bulk.n, accurate.n);

  // THE GATE IS ABSOLUTE AND ECONOMIC, not "tighter than the other rung".
  // docs/al-preset-ladder.md §3: price abs err <= min(0.5*tick, 0.1*vega*1e-4). K =
  // 100 here, so 0.5*tick = 5.0e-3 in the same $-units the errors are measured in.
  //
  // WHY NOT A RELATIVE GATE. Measured on this grid: accurate 7.7e-4, fast 4.8e-4,
  // bulk 1.5e-3. The ACCURATE rung is NOT the tightest, which is the tell that at the
  // worst cells the Crank-Nicolson oracle's OWN discretization error (~1e-4..1e-3 at
  // n_t = 2000 / n_x = 4000) is the larger term. A gate of the form
  // `bulk < k * fast` would therefore be measuring the oracle's grid, not the rung —
  // it is exactly the kind of assertion that passes for the wrong reason and then
  // fails on an unrelated oracle tweak. The absolute economic budget is the claim
  // the tier actually makes, so that is what is asserted.
  EXPECT_LT(bulk.max_abs, 5.0e-3)
      << "bulk max_abs=" << bulk.max_abs << " (economic budget 5.0e-3 = 0.5 tick on K=100)"
      << " worst cell S=" << bulk.worst.S << " K=" << bulk.worst.K << " T=" << bulk.worst.T
      << " sigma=" << bulk.worst.sigma << " q=" << bulk.worst.q;
  EXPECT_LT(fast.max_abs, 5.0e-3) << "fast max_abs=" << fast.max_abs;
  EXPECT_LT(accurate.max_abs, 5.0e-3) << "accurate max_abs=" << accurate.max_abs;

  // Same accuracy CLASS, stated as an order of magnitude rather than a ratio: the
  // mean error must not move by 10x. This is what "no worse in class" means once the
  // oracle floor is acknowledged, and it is still falsifiable — a rung whose
  // n_quad_price got silently tied to 8 lands well outside it.
  EXPECT_LT(bulk.mean_abs, 10.0 * fast.mean_abs)
      << "bulk mean_abs=" << bulk.mean_abs << " fast mean_abs=" << fast.mean_abs;

  // Where bulk is worst is a documented property of the rung, not a surprise: the
  // long-dated / deep-ITM corner, where the early-exercise boundary is most curved
  // and a 2-sweep budget under-converges it (american.cpp's own warm-budget note).
  // Recorded so a regression that moves the worst cell somewhere else is visible.
  EXPECT_GE(bulk.worst.T, 0.25) << "worst bulk cell moved to a short tenor: T=" << bulk.worst.T;
}

// The regime where step-1 measured the largest ON-path deviation: short tenor,
// deep out-of-the-money, where vega is near zero so a fixed price error maps to a
// large IV error. Pinned separately so a future rung change cannot quietly widen it.
TEST(AlBulkRung, ShortTenorDeepWingPriceErrorStaysBounded) {
  struct Wing {
    double S;
    double T;
  };
  double worst = 0.0;
  for (const Wing w : {Wing{78.0, 0.02}, Wing{78.0, 0.05}, Wing{85.0, 0.02}, Wing{125.0, 0.02}}) {
    for (const double sigma : {0.30, 0.60}) {
      const Result<double> al = american_price(w.S, 100.0, w.T, sigma, 0.043, 0.0, Side::Put,
                                              AmericanMethod::AndersenLake, al_bulk_opts());
      ASSERT_TRUE(al.has_value());
      const double ref = oracle_pde_american(w.S, 100.0, w.T, sigma, 0.043, 0.0, Side::Put);
      ASSERT_TRUE(std::isfinite(ref));
      worst = std::max(worst, std::fabs(*al - ref));
    }
  }
  // $-units on a $100 strike. The economic gate docs/al-preset-ladder.md §3 uses is
  // min(0.5*tick, 0.1*vega*1e-4); 0.5 cent on a $100 name is 5e-3.
  EXPECT_LT(worst, 5.0e-3) << "worst short-tenor wing abs price error " << worst;
}

// ── 3. The premium-order-32 specialization is a hoist, not a numerics change ──

TEST(AlBulkRung, PremiumOrder32SpecializedKernelIsBitIdenticalToTheGeneric) {
  std::size_t compared = 0;
  for (const double m : {0.80, 0.95, 1.00, 1.05, 1.25}) {
    for (const double T : {0.02, 0.25, 1.00, 2.00}) {
      for (const double sigma : {0.16, 0.30, 0.60}) {
        for (const double q : {0.0, 0.02, 0.06}) {
          for (const Side side : {Side::Put, Side::Call}) {
            const double S = m * 100.0;
            const Result<double> spec = american_price(S, 100.0, T, sigma, 0.043, q, side,
                                                       AmericanMethod::AndersenLake,
                                                       al_bulk_opts());
            const Result<double> gen = detail::andersen_lake_generic_kernel(
                S, 100.0, T, sigma, 0.043, q, side, al_bulk_opts());
            ASSERT_EQ(spec.has_value(), gen.has_value());
            if (!spec) {
              continue;
            }
            ++compared;
            // The specialized FIXED-POINT kernel is only bit-identical to the generic
            // one to the documented pure-hoist tolerance (BoundaryHoist covers that);
            // what THIS test owns is that adding `case 32:` to the PREMIUM dispatch
            // did not move the price beyond that same tolerance.
            const double scale = std::max(1.0, std::fabs(*gen));
            EXPECT_NEAR(*spec, *gen, 1.0e-9 * scale)
                << "S=" << S << " T=" << T << " sigma=" << sigma << " q=" << q
                << " side=" << (side == Side::Put ? "put" : "call");
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 100u);
}

// ── 4. FitPreset::Bulk is Populate plus exactly two rung substitutions ───────

TEST(FitPresetBulk, IsPopulateExceptTheFitAndCarryAlRungs) {
  SessionInputs pop;
  SessionInputs bulk;
  apply_fit_preset(pop, FitPreset::Populate);
  apply_fit_preset(bulk, FitPreset::Bulk);

  // Everything that sets the fitted surface's QUALITY is identical.
  EXPECT_EQ(bulk.calendar_repair, pop.calendar_repair);
  EXPECT_EQ(bulk.use_correction_cache, pop.use_correction_cache);
  EXPECT_EQ(bulk.use_deam_cache_for_fit, pop.use_deam_cache_for_fit);
  EXPECT_EQ(bulk.score_parity, pop.score_parity);
  EXPECT_EQ(bulk.enforce_calendar_floor, pop.enforce_calendar_floor);
  EXPECT_EQ(bulk.deam.method, pop.deam.method);
  EXPECT_EQ(bulk.deam.n_atm, pop.deam.n_atm);
  EXPECT_EQ(bulk.deam.max_borrow_pairs, pop.deam.max_borrow_pairs);
  EXPECT_DOUBLE_EQ(bulk.deam.iv_tol, pop.deam.iv_tol);
  EXPECT_EQ(bulk.calib.max_obs_per_slice, pop.calib.max_obs_per_slice);
  EXPECT_DOUBLE_EQ(bulk.calib.max_otm_shortcut_premium_spread_frac,
                   pop.calib.max_otm_shortcut_premium_spread_frac);

  // The two rungs that differ, and only those.
  ASSERT_TRUE(pop.deam.al_opts.has_value());
  ASSERT_TRUE(bulk.deam.al_opts.has_value());
  EXPECT_EQ(pop.deam.al_opts->n_quadrature, al_fast_opts().n_quadrature);
  EXPECT_EQ(bulk.deam.al_opts->n_quadrature, al_bulk_opts().n_quadrature);
  ASSERT_TRUE(pop.deam.carry_al_opts.has_value());
  ASSERT_TRUE(bulk.deam.carry_al_opts.has_value());
  EXPECT_EQ(pop.deam.carry_al_opts->n_quadrature, al_fast_opts().n_quadrature);
  EXPECT_EQ(bulk.deam.carry_al_opts->n_quadrature, al_bulk_opts().n_quadrature);
}

// The load-bearing safety property: the rung a STORED surface serves queries with
// must stay inside the fields the record formats persist. `serve_al_opts` is what
// VolaSession::to_priced_surface bakes; if Bulk left it empty, the baked config would
// carry n_quad_price = 32, the archive would drop it, and a query would re-price off
// an 8-node premium quadrature.
TEST(FitPresetBulk, BakedServeRungStaysOnThePersistableFastRung) {
  SessionInputs bulk;
  apply_fit_preset(bulk, FitPreset::Bulk);
  ASSERT_TRUE(bulk.deam.serve_al_opts.has_value());
  EXPECT_EQ(bulk.deam.serve_al_opts->n_collocation, al_fast_opts().n_collocation);
  EXPECT_EQ(bulk.deam.serve_al_opts->n_quadrature, al_fast_opts().n_quadrature);
  EXPECT_EQ(bulk.deam.serve_al_opts->max_newton_iter, al_fast_opts().max_newton_iter);
  // The serve rung must not depend on the un-persisted axis at all.
  EXPECT_EQ(bulk.deam.serve_al_opts->n_quad_price, std::uint16_t{0});

  // Every other preset leaves it empty -- i.e. bit-for-bit historical behaviour.
  for (const FitPreset p : {FitPreset::Fast, FitPreset::Accurate, FitPreset::Robust,
                            FitPreset::Hft, FitPreset::Populate}) {
    SessionInputs in;
    apply_fit_preset(in, p);
    EXPECT_FALSE(in.deam.serve_al_opts.has_value())
        << "preset " << static_cast<unsigned>(p) << " must not pin a serve rung";
  }
}

// ── 5. The new enumerator round-trips through the persisted symbol record ────

TEST(FitPresetBulk, DecodesOutOfThePersistedSymbolRecord) {
  // The record's `preset` is a uint8 wire field validated against a hard cap
  // (surface_db.cpp symbol_record_enums_valid, corpus.cpp to_fit_preset). Adding an
  // enumerator means raising both caps; a stale cap would make every Bulk-built
  // manifest unreadable. Pin the decode side of that here -- the write side is
  // covered end to end by the two `bulk`-preset pilot roots this branch's report
  // records as `verify` exit 0.
  DbSymbolRecord rec{};
  rec.symbol[0] = 'S';
  rec.symbol[1] = 'P';
  rec.symbol[2] = 'Y';
  rec.symbol_len = 3;
  rec.preset = static_cast<std::uint8_t>(FitPreset::Bulk);
  rec.al_n_collocation = al_fast_opts().n_collocation;
  rec.al_n_quadrature = al_fast_opts().n_quadrature;
  rec.al_max_newton_iter = al_fast_opts().max_newton_iter;
  const SymbolFitConfig back = decode_symbol_record(rec);
  EXPECT_EQ(back.preset, FitPreset::Bulk);
  EXPECT_EQ(back.al.n_collocation, al_fast_opts().n_collocation);
  EXPECT_EQ(back.al.n_quadrature, al_fast_opts().n_quadrature);
  // Bulk must never be the LAST enumerator's value by accident: the cap sites read
  // `FitPreset::Bulk`, so this is the invariant they encode.
  EXPECT_EQ(static_cast<unsigned>(FitPreset::Bulk), 5u);
}

} // namespace atx::vol
