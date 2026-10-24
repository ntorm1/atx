#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <thread>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"

// Coverage for the dated-cash lattice KERNEL objects
// (api/pricing/american_discrete_div_kernel.hpp, src/pricing/american_discrete_div_kernel.cpp):
//
//   * DiscreteDivWorkspace prices BIT-IDENTICALLY to the allocating entry on every
//     scheme over a grid that includes the schedule corners (ex-date on expiry, on
//     the first lattice step, cash exhausting the low nodes, empty), refuses what it
//     was not sized for, and gives the same bits from N threads with their own
//     workspaces as the serial loop.
//   * The Screen tier refuses a first-step ex-date (typed), prices as the BBSR
//     lattice at its step count, and its error against the CONVERGED lattice on an
//     SPY-shaped dividend ladder and the schedule corners stays inside the pinned
//     bounds (DiscreteDivScreenTier.*; the bench prints the full tables).
//   * The seeded inversion returns the ACCURATE lattice's root (vs brute-force
//     bisection to 1e-10) within tol, seeded and unseeded, with its typed
//     refusals and fallbacks (DiscreteDivKernelIv.*).
//
// Pinned bounds are 2-figure round-ups of the values measured by this file's own
// loops on the lane's final binary (printed under [bound] so a reviewer can read
// them back); each fails when the kernel it pins is reverted (lane report).
// Allocation is pinned in discrete_div_kernel_alloc_test.cpp.

namespace {

using atx::vol::american_discrete_div_converged_price;
using atx::vol::american_discrete_div_implied_vol;
using atx::vol::american_discrete_div_price;
using atx::vol::american_discrete_div_screen_price;
using atx::vol::CashDividend;
using atx::vol::discrete_div_coarsest_steps;
using atx::vol::discrete_div_screen_steps;
using atx::vol::DiscreteDivConverged;
using atx::vol::DiscreteDivIvFallback;
using atx::vol::DiscreteDivIvOpts;
using atx::vol::DiscreteDivIvResult;
using atx::vol::DiscreteDivScheme;
using atx::vol::DiscreteDivTier;
using atx::vol::DiscreteDivWorkspace;
using atx::vol::ErrorCode;
using atx::vol::ExerciseStyle;
using atx::vol::kDiscreteDivConvergedSteps;
using atx::vol::kDiscreteDivScreenScheme;
using atx::vol::kDiscreteDivScreenSteps;
using atx::vol::kIvMax;
using atx::vol::kIvMin;
using atx::vol::Result;
using atx::vol::Side;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

constexpr DiscreteDivScheme kAllSchemes[] = {
    DiscreteDivScheme::SplicedCrr, DiscreteDivScheme::SplicedBbs, DiscreteDivScheme::SplicedBbsr,
    DiscreteDivScheme::EscrowedBbs, DiscreteDivScheme::EscrowedLeisenReimer};

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

struct Contract {
  double S, K, T, sigma, r, q;
  Side side;
  std::vector<CashDividend> divs;
};

// ── The grid the workspace is proven on ──────────────────────────────────────
//
// Six schedules on each (S, K, T, sigma, q, side): none; one event ON expiry (the
// SPY case, analytic terminal treatment); one on the FIRST lattice step (the
// never-converging splice — still a price, still bit-identical); a cash amount
// that exhausts the low nodes (the quadratic grid-bottom branch); two events
// seven days apart; nine quarterly events.
[[nodiscard]] std::vector<std::vector<CashDividend>> schedules_for(double T, int steps) {
  const double dt = T / static_cast<double>(steps);
  return {
      {},
      {{T, 1.8311}},
      {{dt, 2.0}},
      {{0.5 * T, 60.0}},
      {{0.4 * T, 1.0}, {0.4 * T + 7.0 / 365.0, 1.0}},
      {{0.1 * T, 1.0}, {0.2 * T, 1.0}, {0.3 * T, 1.0}, {0.4 * T, 1.0}, {0.5 * T, 1.0},
       {0.6 * T, 1.0}, {0.7 * T, 1.0}, {0.8 * T, 1.0}, {0.9 * T, 1.0}},
  };
}

struct GridCase {
  Contract c;
  int steps;
  DiscreteDivScheme scheme;
};

[[nodiscard]] std::vector<GridCase> identity_grid() {
  std::vector<GridCase> out;
  constexpr double kS = 100.0;
  constexpr double kR = 0.045;
  for (const double K : {70.0, 90.0, 100.0, 110.0, 130.0}) {
    for (const double T : {0.05, 0.25, 1.0, 2.26}) {
      for (const double sigma : {0.12, 0.2, 0.35}) {
        for (const double q : {-0.002, 0.0}) {
          for (const Side side : {Side::Call, Side::Put}) {
            for (const int steps : {101, 301}) {
              for (const auto &divs : schedules_for(T, steps)) {
                for (const DiscreteDivScheme scheme : kAllSchemes) {
                  out.push_back(GridCase{Contract{kS, K, T, sigma, kR, q, side, divs}, steps,
                                         scheme});
                }
              }
            }
          }
        }
      }
    }
  }
  return out;
}

[[nodiscard]] Result<double> allocating(const GridCase &g) {
  return american_discrete_div_price(g.c.S, g.c.K, g.c.T, g.c.sigma, g.c.r, g.c.q, g.c.side,
                                     g.c.divs, g.steps, ExerciseStyle::American, g.scheme);
}

[[nodiscard]] Result<double> on_workspace(const GridCase &g, DiscreteDivWorkspace &ws) {
  return american_discrete_div_price(g.c.S, g.c.K, g.c.T, g.c.sigma, g.c.r, g.c.q, g.c.side,
                                     g.c.divs, g.steps, ExerciseStyle::American, g.scheme, ws);
}

// ═══════════════════════════════════════════════════════════════════════════
//  1. The workspace
// ═══════════════════════════════════════════════════════════════════════════

TEST(DiscreteDivWorkspace, PriceIsBitIdenticalToTheAllocatingEntryOnEveryScheme) {
  const std::vector<GridCase> grid = identity_grid();
  ASSERT_GE(grid.size(), 2000U);
  DiscreteDivWorkspace ws(301);
  std::size_t priced = 0;
  std::size_t refused = 0;
  for (const GridCase &g : grid) {
    const Result<double> a = allocating(g);
    const Result<double> b = on_workspace(g, ws);
    ASSERT_EQ(a.has_value(), b.has_value())
        << atx::vol::to_string(g.scheme) << " K " << g.c.K << " T " << g.c.T;
    if (!a) {
      EXPECT_EQ(a.error().code(), b.error().code());
      EXPECT_EQ(a.error().message(), b.error().message());
      ++refused;
      continue;
    }
    EXPECT_TRUE(bits_equal(*a, *b))
        << atx::vol::to_string(g.scheme) << " K " << g.c.K << " T " << g.c.T << " sigma "
        << g.c.sigma << " divs " << g.c.divs.size() << ": " << *a << " vs " << *b;
    ++priced;
  }
  std::printf("[grid] cases %zu priced %zu refused %zu\n", grid.size(), priced, refused);
  EXPECT_GE(priced, 2000U);
}

TEST(DiscreteDivWorkspace, RefusesStepsBeyondItsSizeAndNeverGrows) {
  DiscreteDivWorkspace ws(100);
  const std::size_t bytes = ws.reserved_bytes();
  EXPECT_EQ(bytes, 4U * 102U * sizeof(double));
  const std::vector<CashDividend> one{{0.5, 2.0}};
  const auto ok = american_discrete_div_price(100.0, 100.0, 1.0, 0.2, 0.04, 0.0, Side::Put, one,
                                              100, ExerciseStyle::American,
                                              DiscreteDivScheme::SplicedCrr, ws);
  EXPECT_TRUE(ok.has_value());
  const auto too_big = american_discrete_div_price(100.0, 100.0, 1.0, 0.2, 0.04, 0.0, Side::Put,
                                                   one, 101, ExerciseStyle::American,
                                                   DiscreteDivScheme::SplicedCrr, ws);
  ASSERT_FALSE(too_big.has_value());
  EXPECT_EQ(too_big.error().code(), ErrorCode::InvalidArgument);
  // The Leisen-Reimer odd rounding (100 -> 101 steps) still fits the +2 reserve.
  const auto lr = american_discrete_div_price(100.0, 100.0, 1.0, 0.2, 0.04, 0.0, Side::Put, one,
                                              100, ExerciseStyle::American,
                                              DiscreteDivScheme::EscrowedLeisenReimer, ws);
  EXPECT_TRUE(lr.has_value());
  EXPECT_EQ(ws.reserved_bytes(), bytes);
  // Bad inputs are the allocating entry's refusals, text for text.
  const auto bad = american_discrete_div_price(-1.0, 100.0, 1.0, 0.2, 0.04, 0.0, Side::Put, one,
                                               100, ExerciseStyle::American,
                                               DiscreteDivScheme::SplicedCrr, ws);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().message(), "spot must be finite and > 0");
}

TEST(DiscreteDivWorkspace, MoveLeavesAnEmptySourceThatRefuses) {
  DiscreteDivWorkspace a(301);
  DiscreteDivWorkspace b(std::move(a));
  EXPECT_EQ(b.max_steps(), 301);
  EXPECT_TRUE(b.admits(301));
  // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is the contract under test.
  EXPECT_EQ(a.max_steps(), 0);
  EXPECT_FALSE(a.admits(1));
  EXPECT_EQ(a.reserved_bytes(), 0U);
  const std::vector<CashDividend> none;
  const auto refused = american_discrete_div_price(100.0, 100.0, 1.0, 0.2, 0.04, 0.0, Side::Put,
                                                   none, 10, ExerciseStyle::American,
                                                   DiscreteDivScheme::SplicedCrr, a);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  DiscreteDivWorkspace c(10);
  c = std::move(b);
  EXPECT_EQ(c.max_steps(), 301);
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(b.max_steps(), 0);
}

TEST(DiscreteDivWorkspace, ConcurrentWorkspacesMatchTheSerialLoopBitForBit) {
  const std::vector<GridCase> grid = identity_grid();
  std::vector<double> serial(grid.size(), kNaN);
  {
    DiscreteDivWorkspace ws(301);
    for (std::size_t i = 0; i < grid.size(); ++i) {
      const Result<double> p = on_workspace(grid[i], ws);
      serial[i] = p ? *p : kNaN;
    }
  }
  constexpr std::size_t kThreads = 4;
  std::vector<double> parallel(grid.size(), kNaN);
  {
    std::vector<std::jthread> pool;
    pool.reserve(kThreads);
    for (std::size_t t = 0; t < kThreads; ++t) {
      pool.emplace_back([&, t] {
        DiscreteDivWorkspace ws(301); // one per thread: the documented ownership
        for (std::size_t i = t; i < grid.size(); i += kThreads) {
          const Result<double> p = on_workspace(grid[i], ws);
          parallel[i] = p ? *p : kNaN;
        }
      });
    }
  }
  std::size_t compared = 0;
  for (std::size_t i = 0; i < grid.size(); ++i) {
    if (std::isnan(serial[i])) {
      EXPECT_TRUE(std::isnan(parallel[i]));
      continue;
    }
    EXPECT_TRUE(bits_equal(serial[i], parallel[i])) << "case " << i;
    ++compared;
  }
  EXPECT_GE(compared, 2000U);
}

// ═══════════════════════════════════════════════════════════════════════════
//  2. The Screen tier
// ═══════════════════════════════════════════════════════════════════════════

TEST(DiscreteDivScreenTier, StepsRefuseAnExDateBeforeTheCoarseSecondStep) {
  // T = 1 y, the accurate lattice at 301 steps. An event on the accurate FIRST step
  // (tau = 1/301) sits on step 0.25 of the Screen's coarse lattice: refused.
  const std::vector<CashDividend> first_step{{1.0 / 301.0, 2.0}};
  const auto refused = discrete_div_screen_steps(1.0, first_step, 301);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), ErrorCode::OutOfRange);
  // One day ahead on a one-year chain: 1/365 y -> coarse step 0.2 at 75 steps.
  const std::vector<CashDividend> tomorrow{{1.0 / 365.0, 2.0}};
  EXPECT_FALSE(discrete_div_screen_steps(1.0, tomorrow, 301).has_value());
  // Mid-life and on-expiry events are admitted; out-of-window events are ignored.
  const std::vector<CashDividend> fine{{0.5, 2.0}, {1.0, 2.0}, {1.5, 99.0}, {-0.1, 99.0}};
  const auto ok = discrete_div_screen_steps(1.0, fine, 301);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(*ok, kDiscreteDivScreenSteps);
  // Nothing to screen with when the accurate count is not above the screen's.
  EXPECT_FALSE(discrete_div_screen_steps(1.0, fine, kDiscreteDivScreenSteps).has_value());
  EXPECT_EQ(discrete_div_screen_steps(1.0, fine, kDiscreteDivScreenSteps).error().code(),
            ErrorCode::OutOfRange);
  EXPECT_FALSE(discrete_div_screen_steps(0.0, fine, 301).has_value());
  // The exact boundary: an event on coarse step 2 is the first admitted one.
  const int coarse = discrete_div_coarsest_steps(kDiscreteDivScreenSteps, kDiscreteDivScreenScheme);
  const std::vector<CashDividend> step_two{{2.0 / static_cast<double>(coarse), 1.0}};
  EXPECT_TRUE(discrete_div_screen_steps(1.0, step_two, 301).has_value());
  const std::vector<CashDividend> step_one{{1.0 / static_cast<double>(coarse), 1.0}};
  EXPECT_FALSE(discrete_div_screen_steps(1.0, step_one, 301).has_value());
}

TEST(DiscreteDivScreenTier, ScreenPriceIsTheScreenSchemeAtTheScreenSteps) {
  DiscreteDivWorkspace ws(301);
  const std::vector<CashDividend> two{{0.182, 1.8311}, {0.432, 1.8311}};
  for (const Side side : {Side::Call, Side::Put}) {
    const auto screen = american_discrete_div_screen_price(772.98, 760.0, 0.432, 0.2, 0.047,
                                                           -0.0009, side, two,
                                                           kDiscreteDivScreenSteps, ws);
    const auto direct = american_discrete_div_price(772.98, 760.0, 0.432, 0.2, 0.047, -0.0009,
                                                    side, two, kDiscreteDivScreenSteps,
                                                    ExerciseStyle::American,
                                                    kDiscreteDivScreenScheme);
    ASSERT_TRUE(screen.has_value());
    ASSERT_TRUE(direct.has_value());
    EXPECT_TRUE(bits_equal(*screen, *direct));
  }
  // The coarse CRR window closes before the accurate one: typed OutOfRange.
  const auto closed = american_discrete_div_screen_price(772.98, 760.0, 2.0, 0.006, 0.05, -0.05,
                                                         Side::Put, two, kDiscreteDivScreenSteps,
                                                         ws);
  ASSERT_FALSE(closed.has_value());
  EXPECT_EQ(closed.error().code(), ErrorCode::OutOfRange);
}

// ── The SPY-shaped dividend ladder: quarterly 1.8311 from 0.182 y, every expiry
//    ON an ex-date (the cohort5 SPY property), 772.98 spot, r 4.7 %, q -9 bp. ──
[[nodiscard]] std::vector<Contract> spy_like_ladder(std::span<const double> sigmas) {
  std::vector<Contract> out;
  constexpr double kS = 772.98;
  constexpr double kR = 0.047;
  constexpr double kQ = -0.0009;
  constexpr double kFirstEx = 0.182;
  constexpr double kCadence = 0.25;
  constexpr double kCash = 1.8311;
  constexpr double kLogMoneyness[] = {-0.30, -0.20, -0.12, -0.06, -0.02,
                                      0.0,   0.02,  0.06,  0.12,  0.20};
  for (const int quarters : {0, 1, 3, 8}) {
    const double T = kFirstEx + kCadence * static_cast<double>(quarters);
    std::vector<CashDividend> divs;
    for (int i = 0; i <= quarters; ++i) {
      divs.push_back({kFirstEx + kCadence * static_cast<double>(i), kCash});
    }
    const double forward = atx::vol::discrete_div_dated_forward(kS, T, kR, kQ, divs);
    for (const double k : kLogMoneyness) {
      const double K = std::round(forward * std::exp(k));
      for (const Side side : {Side::Call, Side::Put}) {
        for (const double sigma : sigmas) {
          out.push_back(Contract{kS, K, T, sigma, kR, kQ, side, divs});
        }
      }
    }
  }
  return out;
}

// The schedule corners the brief names, each at a sigma the tier admits.
[[nodiscard]] std::vector<Contract> corner_set() {
  std::vector<Contract> out;
  constexpr double kS = 100.0;
  constexpr double kR = 0.045;
  // Cash exhausting the low nodes (the quadratic grid-bottom branch binds).
  out.push_back({kS, 100.0, 1.0, 0.3, kR, 0.0, Side::Put, {{0.5, 60.0}}});
  out.push_back({kS, 100.0, 1.0, 0.3, kR, 0.0, Side::Call, {{0.5, 60.0}}});
  // Ex-date ON expiry (SPY normal case).
  out.push_back({kS, 95.0, 0.5, 0.2, kR, 0.0, Side::Put, {{0.5, 2.0}}});
  out.push_back({kS, 95.0, 0.5, 0.2, kR, 0.0, Side::Call, {{0.5, 2.0}}});
  // Deep-ITM put, ex-date one day ahead on a three-month chain (the boundary
  // collapses to 0 at t_d-, Jourdain-Vellekoop): coarse step 75 * (1/365) / 0.25 = 0.8
  // -> refused by the Screen; the accurate lattice prices it. Kept here for the
  // accurate-vs-converged columns.
  out.push_back({kS, 130.0, 0.25, 0.25, kR, 0.0, Side::Put, {{1.0 / 365.0, 3.0}}});
  // ITM call one day before an ex-date (cum-dividend exercise), ex-date mid-life.
  out.push_back({kS, 80.0, 0.5, 0.25, kR, 0.0, Side::Call, {{0.25, 3.0}}});
  // Two ex-dates within seven days.
  out.push_back({kS, 100.0, 1.0, 0.2, kR, 0.0, Side::Put, {{0.4, 1.0}, {0.4 + 7.0 / 365.0, 1.0}}});
  out.push_back({kS, 100.0, 1.0, 0.2, kR, 0.0, Side::Call, {{0.4, 1.0}, {0.4 + 7.0 / 365.0, 1.0}}});
  // T = 2.26 y with nine events.
  std::vector<CashDividend> nine;
  for (int i = 1; i <= 9; ++i) {
    nine.push_back({0.226 * static_cast<double>(i), 1.0});
  }
  out.push_back({kS, 100.0, 2.26, 0.2, kR, 0.0, Side::Put, nine});
  out.push_back({kS, 100.0, 2.26, 0.2, kR, 0.0, Side::Call, nine});
  return out;
}

struct ErrorStats {
  std::vector<double> abs_err;
  std::vector<double> rel_err;
  std::vector<double> iv_bp;
  double worst_abs = 0.0;
  Contract worst{};
  void add(double err, double reference, double vega, const Contract &c) {
    const double a = std::abs(err);
    abs_err.push_back(a);
    if (reference >= 0.01) {
      rel_err.push_back(a / reference);
    }
    // Vega-bearing: the sigma +/- 1e-3 chord moves the price by >= 1e-2 * max(P, 1)
    // per unit vol (the L2f convention) — elsewhere the IV is not identifiable.
    if (vega >= 1.0e-2 * std::max(reference, 1.0)) {
      iv_bp.push_back(a / vega * 1.0e4);
    }
    if (a > worst_abs) {
      worst_abs = a;
      worst = c;
    }
  }
  [[nodiscard]] static double quantile(std::vector<double> v, double q) {
    if (v.empty()) {
      return 0.0;
    }
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
    return v[idx];
  }
  [[nodiscard]] double max_abs() const { return quantile(abs_err, 1.0); }
  [[nodiscard]] double p99_abs() const { return quantile(abs_err, 0.99); }
  [[nodiscard]] double max_rel() const { return quantile(rel_err, 1.0); }
  [[nodiscard]] double p99_rel() const { return quantile(rel_err, 0.99); }
  [[nodiscard]] double max_iv_bp() const { return quantile(iv_bp, 1.0); }
  [[nodiscard]] double p99_iv_bp() const { return quantile(iv_bp, 0.99); }
  void print(const char *label) const {
    std::printf("[err] %-14s n %4zu max_abs %.3e p99_abs %.3e max_rel %.3e p99_rel %.3e "
                "iv_bp max %.4f p99 %.4f (n %zu) worst K %.2f T %.3f sigma %.2f %s\n",
                label, abs_err.size(), max_abs(), p99_abs(), max_rel(), p99_rel(), max_iv_bp(),
                p99_iv_bp(), iv_bp.size(), worst.K, worst.T, worst.sigma,
                worst.side == Side::Call ? "call" : "put");
  }
};

struct Reference {
  double converged = kNaN;
  double uncertainty = kNaN; // fine - coarse of the converged pair
  double default301 = kNaN;  // SplicedCrr at 301, the vendor-reproducing default
  double vega = kNaN;        // central chord on SplicedBbs 301, h = 1e-3
};

[[nodiscard]] Reference reference_for(const Contract &c, DiscreteDivWorkspace &ws) {
  Reference ref;
  const auto conv = american_discrete_div_converged_price(c.S, c.K, c.T, c.sigma, c.r, c.q,
                                                          c.side, c.divs, ExerciseStyle::American,
                                                          ws);
  if (conv) {
    ref.converged = conv->price;
    ref.uncertainty = conv->fine - conv->coarse;
  }
  const auto d301 = american_discrete_div_price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, c.divs,
                                                301, ExerciseStyle::American,
                                                DiscreteDivScheme::SplicedCrr, ws);
  if (d301) {
    ref.default301 = *d301;
  }
  constexpr double h = 1.0e-3;
  const auto up = american_discrete_div_price(c.S, c.K, c.T, c.sigma + h, c.r, c.q, c.side,
                                              c.divs, 301, ExerciseStyle::American,
                                              DiscreteDivScheme::SplicedBbs, ws);
  const auto dn = american_discrete_div_price(c.S, c.K, c.T, c.sigma - h, c.r, c.q, c.side,
                                              c.divs, 301, ExerciseStyle::American,
                                              DiscreteDivScheme::SplicedBbs, ws);
  if (up && dn) {
    ref.vega = (*up - *dn) / (2.0 * h);
  }
  return ref;
}

// The ladder's references, computed once per process (two 2,401 / 1,201-step
// rollbacks per contract) and shared by the tier tests below.
struct LadderReferences {
  std::vector<Contract> contracts;
  std::vector<Reference> refs;
};

[[nodiscard]] const LadderReferences &ladder_references() {
  static const LadderReferences table = [] {
    LadderReferences t;
    constexpr double kSigmas[] = {0.12, 0.20, 0.35};
    t.contracts = spy_like_ladder(kSigmas);
    DiscreteDivWorkspace ws(kDiscreteDivConvergedSteps);
    t.refs.reserve(t.contracts.size());
    for (const Contract &c : t.contracts) {
      t.refs.push_back(reference_for(c, ws));
    }
    return t;
  }();
  return table;
}

// Pinned bounds: 2-figure round-ups of the values this file's loops measured on the
// lane's final binary (lane note §4; printed under [err] / [bound] above), vs the
// CONVERGED lattice. The Screen tier (SplicedCrr at 101 steps) is a seed, not a
// served price: 6.4e-2 relative / 20.4 bp worst on the ladder is what the accurate
// confirm + polish removes. The 301 SplicedCrr default itself sits 2.7e-2 relative
// / 9.1 bp from the converged lattice on the same ladder (its own column).
constexpr double kScreenLadderMaxRel = 6.5e-2;     // measured 6.4282e-2
constexpr double kScreenLadderMaxIvBp = 21.0;      // measured 20.381 bp
constexpr double kScreenCornerMaxRel = 2.8e-2;     // measured 2.7390e-2
constexpr double kDefault301LadderMaxRel = 2.8e-2; // measured 2.7073e-2

TEST(DiscreteDivScreenTier, BoundVsTheConvergedLatticeOnTheSpyLikeLadder) {
  const LadderReferences &table = ladder_references();
  DiscreteDivWorkspace ws(301);
  ErrorStats screen;
  ErrorStats default301;
  double max_uncertainty = 0.0;
  std::size_t refused = 0;
  for (std::size_t i = 0; i < table.contracts.size(); ++i) {
    const Contract &c = table.contracts[i];
    const Reference &ref = table.refs[i];
    ASSERT_TRUE(std::isfinite(ref.converged)) << "K " << c.K << " T " << c.T;
    max_uncertainty = std::max(max_uncertainty, std::abs(ref.uncertainty));
    const auto ns = discrete_div_screen_steps(c.T, c.divs, 301);
    if (!ns) {
      ++refused;
      continue;
    }
    const auto ps = american_discrete_div_screen_price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side,
                                                       c.divs, *ns, ws);
    ASSERT_TRUE(ps.has_value()) << "K " << c.K << " T " << c.T;
    screen.add(*ps - ref.converged, ref.converged, ref.vega, c);
    default301.add(ref.default301 - ref.converged, ref.converged, ref.vega, c);
  }
  std::printf("[bound] ladder contracts %zu screen-refused %zu converged uncertainty max %.3e\n",
              table.contracts.size(), refused, max_uncertainty);
  screen.print("screen");
  default301.print("spliced301");
  EXPECT_EQ(refused, 0U);
  EXPECT_LE(screen.max_rel(), kScreenLadderMaxRel);
  EXPECT_LE(screen.max_iv_bp(), kScreenLadderMaxIvBp);
  EXPECT_LE(default301.max_rel(), kDefault301LadderMaxRel);
}

TEST(DiscreteDivScreenTier, BoundOnTheScheduleCorners) {
  const std::vector<Contract> corners = corner_set();
  DiscreteDivWorkspace ws(kDiscreteDivConvergedSteps);
  ErrorStats screen;
  ErrorStats default301;
  std::size_t refused = 0;
  for (const Contract &c : corners) {
    const Reference ref = reference_for(c, ws);
    ASSERT_TRUE(std::isfinite(ref.converged)) << "K " << c.K << " T " << c.T;
    default301.add(ref.default301 - ref.converged, ref.converged, ref.vega, c);
    const auto ns = discrete_div_screen_steps(c.T, c.divs, 301);
    if (!ns) {
      ++refused;
      std::printf("[corner] refused by the Screen tier: K %.2f T %.4f tau0 %.5f (%s)\n", c.K,
                  c.T, c.divs.front().tau, ns.error().message().c_str());
      continue;
    }
    const auto ps = american_discrete_div_screen_price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side,
                                                       c.divs, *ns, ws);
    ASSERT_TRUE(ps.has_value());
    screen.add(*ps - ref.converged, ref.converged, ref.vega, c);
    std::printf("[corner] K %.2f T %.3f %s divs %zu: screen %.6f conv %.6f (unc %.1e) d301 %.6f\n",
                c.K, c.T, c.side == Side::Call ? "call" : "put", c.divs.size(), *ps,
                ref.converged, ref.uncertainty, ref.default301);
  }
  screen.print("screen-corner");
  default301.print("spl301-corner");
  // Exactly the one-day-ahead ex-date on the three-month chain is refused.
  EXPECT_EQ(refused, 1U);
  EXPECT_LE(screen.max_rel(), kScreenCornerMaxRel);
}

// ═══════════════════════════════════════════════════════════════════════════
//  3. The seeded inversion
// ═══════════════════════════════════════════════════════════════════════════

// Brute-force bisection root of the ACCURATE lattice, to 1e-10 in sigma.
[[nodiscard]] double bisection_root(double price, const Contract &c, int steps,
                                    DiscreteDivScheme scheme, DiscreteDivWorkspace &ws) {
  const int coarsest = discrete_div_coarsest_steps(steps, scheme);
  double lo = std::max(kIvMin, std::abs(c.r - c.q) *
                                   std::sqrt(c.T / static_cast<double>(coarsest)) *
                                   (1.0 + 1.0e-6));
  double hi = 3.0;
  for (int i = 0; i < 48 && (hi - lo) > 1.0e-10; ++i) {
    const double mid = 0.5 * (lo + hi);
    const auto p = american_discrete_div_price(c.S, c.K, c.T, mid, c.r, c.q, c.side, c.divs,
                                               steps, ExerciseStyle::American, scheme, ws);
    if (!p) {
      return kNaN;
    }
    (*p < price ? lo : hi) = mid;
  }
  return 0.5 * (lo + hi);
}

struct IvStats {
  std::vector<double> distance;
  std::vector<double> equivalents; // accurate-lattice equivalents per root
  std::size_t roots = 0;
  std::size_t fallbacks[7] = {};
  std::size_t errors = 0;
  double worst = 0.0;
  Contract worst_c{};
  void add(const Result<DiscreteDivIvResult> &iv, double root, const Contract &c,
           double screen_equiv) {
    if (!iv) {
      ++errors;
      return;
    }
    ++roots;
    ++fallbacks[static_cast<std::size_t>(iv->fallback)];
    const double d = std::abs(iv->sigma - root);
    distance.push_back(d);
    equivalents.push_back(static_cast<double>(iv->accurate_lattices) +
                          screen_equiv * static_cast<double>(iv->screen_lattices));
    if (d > worst) {
      worst = d;
      worst_c = c;
    }
  }
  void print(const char *label) const {
    std::printf("[iv] %-9s roots %zu errors %zu dist max %.3e p99 %.3e | equiv p50 %.2f p99 %.2f "
                "max %.2f | fallbacks none %zu n/a %zu edge %zu notinc %zu volb %zu pricer %zu "
                "noconv %zu | worst K %.2f T %.3f sigma %.2f %s\n",
                label, roots, errors, ErrorStats::quantile(distance, 1.0),
                ErrorStats::quantile(distance, 0.99), ErrorStats::quantile(equivalents, 0.5),
                ErrorStats::quantile(equivalents, 0.99), ErrorStats::quantile(equivalents, 1.0),
                fallbacks[0], fallbacks[1], fallbacks[2], fallbacks[3], fallbacks[4],
                fallbacks[5], fallbacks[6], worst_c.K, worst_c.T, worst_c.sigma,
                worst_c.side == Side::Call ? "call" : "put");
  }
};

constexpr double kIvTol = 1.0e-7;
// Pins: the distance to the bisection root is the CONTRACT (<= tol; measured max
// 8.3990e-8 seeded / 9.9810e-8 unseeded on the ladder), the p99 accurate-lattice
// equivalents per seeded root is the measured cost (4.7881; p50 3.68 — the brief's
// <= 4 target holds at the median, not at p99; `discrete_div_screen_equivalent`).
constexpr double kIvMaxDistance = kIvTol;
constexpr double kIvSeededP99Equivalents = 4.8;

[[nodiscard]] double screen_equivalent() {
  return atx::vol::discrete_div_screen_equivalent(kDiscreteDivScreenSteps, 301);
}

TEST(DiscreteDivKernelIv, RootIsTheAccurateLatticesWithinTolSeededAndUnseeded) {
  const LadderReferences &table = ladder_references();
  DiscreteDivWorkspace ws(301);
  IvStats seeded;
  IvStats unseeded;
  std::size_t skipped = 0;
  for (std::size_t i = 0; i < table.contracts.size(); ++i) {
    const Contract &c = table.contracts[i];
    const Reference &ref = table.refs[i];
    // The quote: the 301 default lattice's own price at the contract's sigma.
    const double quote = ref.default301;
    if (!(ref.vega >= 1.0e-2 * std::max(quote, 1.0))) {
      ++skipped; // not vega-bearing: the IV is not identifiable (L1c)
      continue;
    }
    const double root = bisection_root(quote, c, 301, DiscreteDivScheme::SplicedCrr, ws);
    ASSERT_TRUE(std::isfinite(root));
    DiscreteDivIvOpts opts;
    opts.tol = kIvTol;
    opts.seed_sigma = c.sigma * 1.05; // a seed 5 % off, the funding-pv IV's class
    seeded.add(american_discrete_div_implied_vol(quote, c.S, c.K, c.T, c.r, c.q, c.side, c.divs,
                                                 301, DiscreteDivScheme::SplicedCrr, opts, ws),
               root, c, screen_equivalent());
    opts.seed_sigma = 0.0;
    unseeded.add(american_discrete_div_implied_vol(quote, c.S, c.K, c.T, c.r, c.q, c.side,
                                                   c.divs, 301, DiscreteDivScheme::SplicedCrr,
                                                   opts, ws),
                 root, c, screen_equivalent());
  }
  std::printf("[iv] ladder contracts %zu skipped (no vega) %zu screen-equivalent %.3f\n",
              table.contracts.size(), skipped, screen_equivalent());
  seeded.print("seeded");
  unseeded.print("unseeded");
  EXPECT_GE(seeded.roots, 150U);
  EXPECT_EQ(seeded.errors, 0U);
  EXPECT_EQ(unseeded.errors, 0U);
  EXPECT_LE(ErrorStats::quantile(seeded.distance, 1.0), kIvMaxDistance);
  EXPECT_LE(ErrorStats::quantile(unseeded.distance, 1.0), kIvMaxDistance);
  EXPECT_LE(ErrorStats::quantile(seeded.equivalents, 0.99), kIvSeededP99Equivalents);
}

TEST(DiscreteDivKernelIv, RefusesAQuoteAtOrBelowTheLowerLatticePrice) {
  DiscreteDivWorkspace ws(301);
  const std::vector<CashDividend> two{{0.182, 1.8311}, {0.432, 1.8311}};
  // Deep-ITM put quoted UNDER its intrinsic: no sigma reproduces it, dated_cash_iv's
  // "at or below the identifiable range". (A quote AT intrinsic is not a refusal
  // on either path: the lattice's root-node level carries ~1e-13 relative
  // rounding, so P(lo) sits a few 1e-11 under intrinsic and the flat region
  // inverts to some sigma — the L1c intrinsic-region property, shared with
  // dated_cash_iv.)
  const double below_intrinsic = (900.0 - 772.98) - 1.0e-3;
  DiscreteDivIvOpts opts;
  for (const DiscreteDivTier tier : {DiscreteDivTier::Screen, DiscreteDivTier::Accurate}) {
    opts.tier = tier;
    const auto iv = american_discrete_div_implied_vol(below_intrinsic, 772.98, 900.0, 0.432,
                                                      0.047, -0.0009, Side::Put, two, 301,
                                                      DiscreteDivScheme::SplicedCrr, opts, ws);
    ASSERT_FALSE(iv.has_value());
    EXPECT_EQ(iv.error().code(), ErrorCode::OutOfRange);
  }
  const auto zero = american_discrete_div_implied_vol(0.0, 772.98, 900.0, 0.432, 0.047, -0.0009,
                                                      Side::Put, two, 301,
                                                      DiscreteDivScheme::SplicedCrr, opts, ws);
  ASSERT_FALSE(zero.has_value());
  EXPECT_EQ(zero.error().code(), ErrorCode::OutOfRange);
}

TEST(DiscreteDivKernelIv, RefusesAQuoteAboveTheMaximumVolatilityPrice) {
  DiscreteDivWorkspace ws(301);
  const std::vector<CashDividend> two{{0.182, 1.8311}, {0.432, 1.8311}};
  DiscreteDivIvOpts opts;
  for (const DiscreteDivTier tier : {DiscreteDivTier::Screen, DiscreteDivTier::Accurate}) {
    opts.tier = tier;
    // A call worth more than the spot has no volatility.
    const auto iv = american_discrete_div_implied_vol(800.0, 772.98, 760.0, 0.432, 0.047, -0.0009,
                                                      Side::Call, two, 301,
                                                      DiscreteDivScheme::SplicedCrr, opts, ws);
    ASSERT_FALSE(iv.has_value());
    EXPECT_EQ(iv.error().code(), ErrorCode::OutOfRange);
  }
}

TEST(DiscreteDivKernelIv, AFirstStepExDateNeverTouchesTheScreenAndStillFindsTheRoot) {
  DiscreteDivWorkspace ws(301);
  // An ex-date one day ahead on a three-month chain: coarse step 75 / 365 / 0.25 =
  // 0.8 -> the Screen refuses; the strike is near the money so the IV is
  // identifiable (a deep-ITM put there sits flat at intrinsic and has no root).
  const Contract c{100.0, 105.0, 0.25, 0.25, 0.045, 0.0, Side::Put, {{1.0 / 365.0, 3.0}}};
  ASSERT_FALSE(discrete_div_screen_steps(c.T, c.divs, 301).has_value());
  const auto quote = american_discrete_div_price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, c.divs,
                                                 301, ExerciseStyle::American,
                                                 DiscreteDivScheme::SplicedCrr, ws);
  ASSERT_TRUE(quote.has_value());
  EXPECT_GT(*quote, (c.K - c.S) + 0.5); // time value: the quote is not at intrinsic
  const double root = bisection_root(*quote, c, 301, DiscreteDivScheme::SplicedCrr, ws);
  DiscreteDivIvOpts opts;
  opts.seed_sigma = 0.26;
  const auto iv = american_discrete_div_implied_vol(*quote, c.S, c.K, c.T, c.r, c.q, c.side,
                                                    c.divs, 301, DiscreteDivScheme::SplicedCrr,
                                                    opts, ws);
  ASSERT_TRUE(iv.has_value()) << iv.error().message();
  EXPECT_EQ(iv->fallback, DiscreteDivIvFallback::NotApplicable);
  EXPECT_EQ(iv->screen_lattices, 0);
  EXPECT_GT(iv->accurate_lattices, 0);
  EXPECT_NEAR(iv->sigma, root, kIvTol);
}

TEST(DiscreteDivKernelIv, TierAccurateIsTheBracketedSecantAlone) {
  DiscreteDivWorkspace ws(301);
  const Contract c{772.98, 760.0, 0.432, 0.2, 0.047, -0.0009, Side::Put,
                   {{0.182, 1.8311}, {0.432, 1.8311}}};
  const auto quote = american_discrete_div_price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, c.divs,
                                                 301, ExerciseStyle::American,
                                                 DiscreteDivScheme::SplicedCrr, ws);
  ASSERT_TRUE(quote.has_value());
  const double root = bisection_root(*quote, c, 301, DiscreteDivScheme::SplicedCrr, ws);
  DiscreteDivIvOpts opts;
  opts.tier = DiscreteDivTier::Accurate;
  const auto iv = american_discrete_div_implied_vol(*quote, c.S, c.K, c.T, c.r, c.q, c.side,
                                                    c.divs, 301, DiscreteDivScheme::SplicedCrr,
                                                    opts, ws);
  ASSERT_TRUE(iv.has_value()) << iv.error().message();
  EXPECT_EQ(iv->fallback, DiscreteDivIvFallback::NotApplicable);
  EXPECT_EQ(iv->screen_lattices, 0);
  EXPECT_NEAR(iv->sigma, root, kIvTol);
  std::printf("[iv] accurate-only secant: %u lattices for this root\n",
              static_cast<unsigned>(iv->accurate_lattices));
  // The same root on the Richardson accurate scheme (its coarse lattice sets the
  // lower endpoint) and through a workspace sized exactly for it.
  DiscreteDivWorkspace tight(301);
  opts.tier = DiscreteDivTier::Screen;
  const auto bbsr_quote = american_discrete_div_price(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side,
                                                      c.divs, 301, ExerciseStyle::American,
                                                      DiscreteDivScheme::SplicedBbsr, tight);
  ASSERT_TRUE(bbsr_quote.has_value());
  const auto bbsr = american_discrete_div_implied_vol(*bbsr_quote, c.S, c.K, c.T, c.r, c.q,
                                                      c.side, c.divs, 301,
                                                      DiscreteDivScheme::SplicedBbsr, opts, tight);
  ASSERT_TRUE(bbsr.has_value()) << bbsr.error().message();
  EXPECT_NEAR(bbsr->sigma, c.sigma, 5.0 * kIvTol);
}

TEST(DiscreteDivKernelIv, RefusesBadInputsAndAWorkspaceTooSmall) {
  DiscreteDivWorkspace small(100);
  const std::vector<CashDividend> none;
  DiscreteDivIvOpts opts;
  const auto too_small = american_discrete_div_implied_vol(5.0, 100.0, 100.0, 1.0, 0.04, 0.0,
                                                           Side::Put, none, 301,
                                                           DiscreteDivScheme::SplicedCrr, opts,
                                                           small);
  ASSERT_FALSE(too_small.has_value());
  EXPECT_EQ(too_small.error().code(), ErrorCode::InvalidArgument);
  DiscreteDivWorkspace ws(301);
  const auto bad_spot = american_discrete_div_implied_vol(5.0, -1.0, 100.0, 1.0, 0.04, 0.0,
                                                          Side::Put, none, 301,
                                                          DiscreteDivScheme::SplicedCrr, opts, ws);
  ASSERT_FALSE(bad_spot.has_value());
  EXPECT_EQ(bad_spot.error().code(), ErrorCode::InvalidArgument);
  opts.tol = 0.0;
  const auto bad_tol = american_discrete_div_implied_vol(5.0, 100.0, 100.0, 1.0, 0.04, 0.0,
                                                         Side::Put, none, 301,
                                                         DiscreteDivScheme::SplicedCrr, opts, ws);
  ASSERT_FALSE(bad_tol.has_value());
  EXPECT_EQ(bad_tol.error().code(), ErrorCode::InvalidArgument);
}

TEST(DiscreteDivKernelIv, FallbackNamesAreStable) {
  EXPECT_STREQ(atx::vol::to_string(DiscreteDivIvFallback::None), "none");
  EXPECT_STREQ(atx::vol::to_string(DiscreteDivIvFallback::NotApplicable), "not_applicable");
  EXPECT_STREQ(atx::vol::to_string(DiscreteDivIvFallback::RangeEdge), "range_edge");
  EXPECT_STREQ(atx::vol::to_string(DiscreteDivIvFallback::NotIncreasing), "not_increasing");
  EXPECT_STREQ(atx::vol::to_string(DiscreteDivIvFallback::VolBound), "vol_bound");
  EXPECT_STREQ(atx::vol::to_string(DiscreteDivIvFallback::PricerFailure), "pricer_failure");
  EXPECT_STREQ(atx::vol::to_string(DiscreteDivIvFallback::NoConvergence), "no_convergence");
}

} // namespace
