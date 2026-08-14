// DispersionBook synthetic suite — proves the implied-correlation signal, the
// vega-neutral straddle sizing, and the uid-remap binding, with NO external data.
//
// The universe is built from deterministic known-truth boards (make_board_spec,
// the make_singlename_spec pattern parametrized by ATM vol): one low-vol INDEX
// board and two higher-vol single-name boards, each fitted through the blessed
// PricerFitter -> to_priced_surface path and remapped to a DISTINCT uid via
// `with_uid` (index=1, names=2,3). So this test runs everywhere and is NOT
// GTEST_SKIP-gated.
//
// Coverage:
//   1. Signal_MatchesClosedForm — dispersion_signal reproduces rho_imp computed by
//      hand from the exact ATM vols the surfaces return, and it is finite / > 0;
//   2. Book_IsVegaNeutral       — the priced book's index-leg vega equals minus the
//      basket-leg vega (== target_vega gross), with a Call+Put straddle per leg;
//   3. WithUid_RemapsAndPrices  — with_uid only changes uid() and reprices
//      bit-identically;
//   4. Rejections               — <2 names, non-positive weight sum, target_T<=0,
//      target_vega<=0, and a uid missing from the set each Err with the right code.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"         // AmericanGreeks
#include "atx/vol/chain.hpp"            // OptionChain
#include "atx/vol/data.hpp"             // iso_to_ns, year_fraction
#include "atx/vol/dispersion.hpp"       // DispersionUniverse, dispersion_signal, ...
#include "atx/vol/market_env.hpp"       // MarketEnv
#include "atx/vol/panel.hpp"            // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/portfolio_pricer.hpp" // Portfolio, SurfaceSet, PortfolioPricer
#include "atx/vol/priced_surface.hpp"   // PricedSurface, SliceContext
#include "atx/vol/pricer_fitter.hpp"    // PricerFitter, PricerConfig
#include "atx/vol/s3.hpp"               // S3Params
#include "atx/vol/session.hpp"          // VolaSession::to_priced_surface
#include "atx/vol/surface_parity.hpp"   // SliceContext
#include "atx/vol/types.hpp"            // Side, ErrorCode

using namespace atx::vol;

namespace {

constexpr char kSnapshot[] = "2026-07-06";
constexpr double kTargetT = 30.0 / 365.25;
constexpr std::uint32_t kIndexUid = 1;
constexpr std::uint32_t kName0Uid = 2;
constexpr std::uint32_t kName1Uid = 3;
constexpr double kW0 = 0.6; // basket weights (sum to 1 => the book is vega-neutral)
constexpr double kW1 = 0.4;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

[[nodiscard]] std::uint64_t hexbits(double a) noexcept {
  std::uint64_t b = 0;
  std::memcpy(&b, &a, sizeof b);
  return b;
}

[[nodiscard]] double from_hexbits(std::uint64_t bits) noexcept {
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

// Relative-tolerance closeness (avoids fragile fp-associativity assumptions).
[[nodiscard]] bool close(double a, double b, double rel = 1e-9) noexcept {
  return std::fabs(a - b) <= rel * (std::fabs(a) + std::fabs(b) + 1e-300);
}

// A smooth known-truth board (the make_singlename_spec pattern) parametrized by
// its ATM vol level `sigma0`. Four expiries with a mild declining term structure
// and a 13-strike ladder — robustly fittable, auto-selecting the eSSVI backbone.
[[nodiscard]] SynthPanelSpec make_board_spec(const std::string &uid, double spot, double sigma0) {
  SynthPanelSpec s;
  s.uid = uid;
  s.snapshot_iso = kSnapshot;
  s.spot = spot;
  s.r = 0.043;
  s.borrow = 0.0;

  struct Row {
    const char *iso;
    double sig;
    double skew_k;
    double c2;
  };
  const Row rows[] = {
      {"2026-07-17", sigma0, -0.55, 0.6},
      {"2026-08-21", sigma0 - 0.02, -0.52, 0.7},
      {"2026-09-18", sigma0 - 0.04, -0.50, 0.8},
      {"2026-12-18", sigma0 - 0.06, -0.46, 0.9},
  };
  for (const Row &r : rows) {
    SynthExpiry e;
    e.expiry_iso = r.iso;
    e.T = year_fraction(kSnapshot, r.iso);
    const double s2 = 2.0 * std::sqrt(e.T) * r.skew_k;
    e.truth = S3Params{r.sig, s2, r.c2};
    s.expiries.push_back(e);
  }
  for (const double m :
       {0.80, 0.83, 0.87, 0.91, 0.95, 0.98, 1.0, 1.02, 1.05, 1.09, 1.13, 1.17, 1.20}) {
    s.strikes.push_back(spot * m);
  }
  s.half_spread_frac = 0.05;
  s.min_half_spread = 0.05;
  return s;
}

// Fit a board through the blessed path into a PricedSurface (auto curve select,
// single-threaded — deterministic).
[[nodiscard]] PricedSurface fit_board(const SynthPanelSpec &spec) {
  auto panel = make_synthetic_american_panel(spec);
  EXPECT_TRUE(panel.has_value()) << (panel ? "" : panel.error().to_string());
  auto chain = OptionChain::from_frame(
      panel->frame,
      MarketEnv::flat(spec.spot, spec.r, iso_to_ns(spec.snapshot_iso), spec.cash_divs));
  EXPECT_TRUE(chain.has_value()) << (chain ? "" : chain.error().to_string());
  PricerConfig cfg;
  cfg.n_threads = 1;
  PricerFitter fitter{cfg};
  EXPECT_TRUE(fitter.fit(*chain).has_value());
  auto ps = fitter.surface()->session().to_priced_surface();
  EXPECT_TRUE(ps.has_value()) << (ps ? "" : ps.error().to_string());
  return std::move(*ps);
}

// Fit a board and stamp it with `uid` via with_uid — the universe binding.
[[nodiscard]] PricedSurface fit_member(const std::string &uid, double spot, double sigma0,
                                       std::uint32_t surface_uid) {
  const PricedSurface raw = fit_board(make_board_spec(uid, spot, sigma0));
  auto remapped = with_uid(raw, surface_uid);
  EXPECT_TRUE(remapped.has_value()) << (remapped ? "" : remapped.error().to_string());
  return std::move(*remapped);
}

// The 3-board universe: one index (uid 1) + two names (uid 2,3), weights summing
// to 1. Index ATM vol is set below the names so rho_imp is comfortably in (0, 1).
[[nodiscard]] std::vector<PricedSurface> build_surfaces() {
  std::vector<PricedSurface> s;
  s.push_back(fit_member("IDX", 500.0, 0.28, kIndexUid));
  s.push_back(fit_member("NM0", 100.0, 0.30, kName0Uid));
  s.push_back(fit_member("NM1", 120.0, 0.34, kName1Uid));
  return s;
}

[[nodiscard]] DispersionUniverse make_universe_w(double w0, double w1) {
  DispersionUniverse u;
  u.index = DispersionMember{"IDX", kIndexUid, 0.0};
  u.names.push_back(DispersionMember{"NM0", kName0Uid, w0});
  u.names.push_back(DispersionMember{"NM1", kName1Uid, w1});
  return u;
}

[[nodiscard]] DispersionUniverse make_universe() { return make_universe_w(kW0, kW1); }

// E1 / AN-P1-1 unit seam. `DispersionConfig::target_vega` is now dollars of
// gross vega per VOL POINT (a 0.01 move in sigma) — the canonical unit, shared
// with the listed route. `PortfolioPricer`'s `vega` column and
// `DispersionLeg::straddle_vega` are both dP/dsigma per UNIT vol, so a priced
// book total is converted into the config's unit by multiplying by
// `atx::vol::kVegaPerVolPoint` (dispersion.hpp — FIX-E C-1 hoisted it out of
// dispersion.cpp so production code shares the ONE constant this test uses).

// Sum the priced book's position vega, bucketed into (index leg, name legs).
// UNIT: dollars per UNIT vol (the PortfolioPricer column's own unit).
struct BucketedVega {
  double index{0.0};
  double names{0.0};
};
[[nodiscard]] BucketedVega price_bucketed_vega(const DispersionBook &book, const SurfaceSet &set,
                                               std::uint32_t index_uid) {
  auto pf = Portfolio::create(book.positions);
  EXPECT_TRUE(pf.has_value());
  const PortfolioPricer pricer{std::move(*pf)};
  auto frame = pricer.price(set);
  EXPECT_TRUE(frame.has_value());
  BucketedVega v;
  if (frame.has_value()) {
    for (std::size_t i = 0; i < frame->size(); ++i) {
      if (frame->uid[i] == index_uid) {
        v.index += frame->vega[i];
      } else {
        v.names += frame->vega[i];
      }
    }
  }
  return v;
}

[[nodiscard]] std::vector<const PricedSurface *> as_ptrs(const std::vector<PricedSurface> &v) {
  std::vector<const PricedSurface *> p;
  p.reserve(v.size());
  for (const PricedSurface &s : v) {
    p.push_back(&s);
  }
  return p;
}

} // namespace

// ── 1. Signal reproduces the closed-form implied correlation ────────────────
TEST(Dispersion, Signal_MatchesClosedForm) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();
  const DispersionUniverse u = make_universe();

  auto sig = dispersion_signal(u, *set, kTargetT);
  ASSERT_TRUE(sig.has_value()) << sig.error().to_string();

  // Hand-compute rho_imp from the exact ATM vols the surfaces return.
  const double si = surfaces[0].iv(surfaces[0].forward_at(kTargetT), kTargetT);
  const double s0 = surfaces[1].iv(surfaces[1].forward_at(kTargetT), kTargetT);
  const double s1 = surfaces[2].iv(surfaces[2].forward_at(kTargetT), kTargetT);
  ASSERT_TRUE(std::isfinite(si) && std::isfinite(s0) && std::isfinite(s1));

  const double sws = kW0 * s0 + kW1 * s1;
  const double sw2s2 = kW0 * kW0 * s0 * s0 + kW1 * kW1 * s1 * s1;
  const double denom = sws * sws - sw2s2;
  const double rho = (si * si - sw2s2) / denom;

  EXPECT_NEAR(sig->sigma_index, si, 1e-12);
  ASSERT_EQ(sig->sigma_names.size(), 2u);
  EXPECT_NEAR(sig->sigma_names[0], s0, 1e-12);
  EXPECT_NEAR(sig->sigma_names[1], s1, 1e-12);
  EXPECT_NEAR(sig->sum_w_sigma, sws, 1e-12);
  EXPECT_NEAR(sig->sum_w2_sigma2, sw2s2, 1e-12);
  EXPECT_NEAR(sig->implied_corr, rho, 1e-12);
  EXPECT_NEAR(sig->T_used, kTargetT, 1e-15);

  EXPECT_TRUE(std::isfinite(sig->implied_corr));
  EXPECT_GT(sig->implied_corr, 0.0);
  EXPECT_LT(sig->implied_corr, 1.5) << "implied correlation out of a reasonable band";
}

// ── 2. The book is vega-neutral (index vega == -basket vega) ────────────────
TEST(Dispersion, Book_IsVegaNeutral) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();
  const DispersionUniverse u = make_universe();

  DispersionConfig cfg; // 30d tenor, 10000 target vega, short-index, mult 100
  auto book = build_dispersion_book(u, *set, cfg);
  ASSERT_TRUE(book.has_value()) << book.error().to_string();

  // 2 positions (Call, Put) per straddle, 1 index + 2 names => 6.
  ASSERT_EQ(book->positions.size(), 2u * (1u + u.names.size()));
  ASSERT_EQ(book->name_legs.size(), 2u);

  // Every straddle is a Call then a Put at equal K, T, qty; ids monotonic.
  for (std::size_t i = 0; i < book->positions.size(); i += 2) {
    const Position &c = book->positions[i];
    const Position &p = book->positions[i + 1];
    EXPECT_EQ(c.contract.side, Side::Call);
    EXPECT_EQ(p.contract.side, Side::Put);
    EXPECT_EQ(c.contract.uid, p.contract.uid);
    EXPECT_TRUE(bits_equal(c.contract.K, p.contract.K));
    EXPECT_TRUE(bits_equal(c.contract.T, p.contract.T));
    EXPECT_TRUE(bits_equal(c.qty, p.qty));
    EXPECT_LT(c.id, p.id);
  }

  // Short index, long names.
  EXPECT_LT(book->index_leg.straddle_qty, 0.0);
  for (const DispersionLeg &leg : book->name_legs) {
    EXPECT_GT(leg.straddle_qty, 0.0);
  }

  // Price the book against the set and bucket position vega by uid.
  const BucketedVega v = price_bucketed_vega(*book, *set, u.index.uid);

  // Vega-neutral construction: index vega == -(basket vega), and the index leg
  // carries target_vega of gross vega (sign per side).
  EXPECT_TRUE(close(v.index, -v.names)) << v.index << " vs " << -v.names;
  EXPECT_TRUE(close(std::fabs(v.index) * kVegaPerVolPoint, cfg.target_vega)) << v.index;
  EXPECT_LT(v.index, 0.0); // short index

  std::printf("[dispersion] n_names=%zu book_vega_idx=%.2f book_vega_names=%.2f\n",
              book->name_legs.size(), v.index, v.names);
}

// Prints the exact-bit anchors of the Book_IsVegaNeutral fixture's leg fields so
// C1.7's pinned bit-identity test (below) can be captured against the PRE-change
// `resolve_leg` (two full greeks_analytic bundles). Always succeeds; the values
// are read from stdout.
TEST(Dispersion, PrintBookHexAnchors_C1_7) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();
  const DispersionUniverse u = make_universe();

  DispersionConfig cfg;
  auto book = build_dispersion_book(u, *set, cfg);
  ASSERT_TRUE(book.has_value()) << book.error().to_string();
  const BucketedVega v = price_bucketed_vega(*book, *set, u.index.uid);

  const auto print_leg = [](const char *name, const DispersionLeg &leg) {
    std::printf("[c1.7-anchor] %s K=%016llx T=%016llx sigma=%016llx vega=%016llx "
                "qty=%016llx call=%016llx put=%016llx\n",
                name, static_cast<unsigned long long>(hexbits(leg.K)),
                static_cast<unsigned long long>(hexbits(leg.T)),
                static_cast<unsigned long long>(hexbits(leg.sigma)),
                static_cast<unsigned long long>(hexbits(leg.straddle_vega)),
                static_cast<unsigned long long>(hexbits(leg.straddle_qty)),
                static_cast<unsigned long long>(hexbits(leg.call_mark)),
                static_cast<unsigned long long>(hexbits(leg.put_mark)));
  };
  print_leg("index", book->index_leg);
  print_leg("name0", book->name_legs[0]);
  print_leg("name1", book->name_legs[1]);
  std::printf("[c1.7-anchor] totals book_vega_idx=%016llx book_vega_names=%016llx\n",
              static_cast<unsigned long long>(hexbits(v.index)),
              static_cast<unsigned long long>(hexbits(v.names)));
  SUCCEED();
}

// C1.7: dispersion book build stops paying two full 8-Greek AmericanGreeks
// bundles per leg (resolve_leg previously called greeks_analytic() for Call and
// Put and read only .vega/.price) and instead resolves price + vega directly.
// Every DispersionLeg field and the book's re-priced vega totals must remain
// economically equivalent to the old two-full-bundle resolve path. Historical
// anchors are compared using explicit IV/price/vega tolerances, not fit bits.
TEST(Dispersion, BookEconomicallyStableAfterVegaOnlyResolve) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();
  const DispersionUniverse u = make_universe();

  DispersionConfig cfg;
  auto book = build_dispersion_book(u, *set, cfg);
  ASSERT_TRUE(book.has_value()) << book.error().to_string();
  ASSERT_EQ(book->name_legs.size(), 2u);

  const auto check_leg = [](const DispersionLeg &leg, std::uint64_t K, std::uint64_t T,
                            std::uint64_t sigma, std::uint64_t vega, std::uint64_t qty,
                            std::uint64_t call, std::uint64_t put) {
    const double expected_K = from_hexbits(K);
    const double expected_T = from_hexbits(T);
    const double expected_sigma = from_hexbits(sigma);
    const double expected_vega = from_hexbits(vega);
    const double expected_qty = from_hexbits(qty);
    const double expected_call = from_hexbits(call);
    const double expected_put = from_hexbits(put);
    // Coherent log-forward carry intentionally moves continuous delta strikes.
    // Five ppm is at most a quarter-cent on this fixture and far below a listed
    // strike increment; the book neutrality checks below remain independent.
    EXPECT_NEAR(leg.K, expected_K, 5.0e-6 * std::max(1.0, std::fabs(expected_K)));
    EXPECT_NEAR(leg.T, expected_T, 1.0e-12);
    EXPECT_NEAR(leg.sigma, expected_sigma, 5.0e-5);
    EXPECT_NEAR(leg.straddle_vega, expected_vega,
                1.0e-5 * std::max(1.0, std::fabs(expected_vega)));
    // E1 / AN-P1-1 DOCUMENTED DRIFT. The pinned anchors were captured when
    // `target_vega` was read as dollars per UNIT vol. E1 made it dollars per
    // VOL POINT (the canonical unit, shared with the listed route), which
    // multiplies every sized quantity by exactly 1/kVegaPerVolPoint = 100 and
    // leaves K / T / sigma / straddle_vega / marks untouched — all of which
    // are still pinned bit-for-bit against the ORIGINAL anchors above. Scaling
    // the expectation here (rather than re-capturing) keeps that 100x the only
    // thing this change is allowed to have moved.
    const double expected_qty_scaled = expected_qty / kVegaPerVolPoint;
    EXPECT_NEAR(leg.straddle_qty, expected_qty_scaled,
                1.0e-5 * std::max(1.0, std::fabs(expected_qty_scaled)));
    // This carry-correction work package permits two-tenths of a cent on the
    // deliberately wide synthetic fixture (observed maximum: 0.122 cents).
    EXPECT_NEAR(leg.call_mark, expected_call, 2.0e-3);
    EXPECT_NEAR(leg.put_mark, expected_put, 2.0e-3);
  };
  // Re-captured via Dispersion.PrintBookHexAnchors_C1_7 (feat/bt-sota, WS-C).
  // The C1.7 contract under test — vega-only `resolve_leg` reproduces the
  // two-full-bundle `resolve_leg` EXACTLY — is unchanged and still what these
  // anchors pin; only the fit feeding it moved. WS-C hardened the SVI-MM
  // calibrator (quasi-explicit Nelder-Mead budget decoupled from
  // max_inner_iter -> full ~200 moves; static-arb butterfly project-or-drop;
  // q90 robust IRLS scale), which converges the synthetic-fixture smile
  // materially tighter (wide-smile vega-weighted RMSE 3.5e-5 -> 4.6e-10). The
  // fitted sigma therefore moves in the low ~1e-3 relative and the put/call
  // marks shift ~0.2 cents on this deliberately-wide fixture, both toward the
  // S3Params truth. T is bit-identical; the book's vega-neutrality invariant
  // still holds (totals below: index bit-exactly -10000, names 1 ULP off).
  check_leg(book->index_leg, 0x407f5c4d7c3c6aacULL, 0x3fb506d56bc305c8ULL, 0x3fd0dbd91d3dc177ULL,
            0x405c884f475c26d3ULL, 0xbfec09ca347f1f3aULL, 0x402e1c4c7431e465ULL,
            0x402e5e81878d7fdfULL);
  check_leg(book->name_legs[0], 0x405916a461b917caULL, 0x3fb506d56bc305c8ULL, 0x3fd223aadf478c0aULL,
            0x4036d39cf9b0f86eULL, 0x40050730a32713f0ULL, 0x4009eaca643d472cULL,
            0x400a1ed195c8f114ULL);
  check_leg(book->name_legs[1], 0x405e1b2ba5b6c84fULL, 0x3fb506d56bc305c8ULL, 0x3fd4b3593dddbb03ULL,
            0x403b640bd71e3270ULL, 0x3ff75d91b82a483fULL, 0x4011be979e4ad28dULL,
            0x4011dce28b63a17aULL);

  // Book totals: re-price the emitted positions and bucket vega by uid (the
  // SAME independent cross-check Book_IsVegaNeutral runs) — an end-to-end
  // pin on top of the per-leg field pins above.
  const BucketedVega v = price_bucketed_vega(*book, *set, u.index.uid);
  EXPECT_NEAR(v.index * kVegaPerVolPoint, -cfg.target_vega, 1.0e-6 * cfg.target_vega);
  EXPECT_NEAR(v.names * kVegaPerVolPoint, cfg.target_vega, 1.0e-6 * cfg.target_vega);
  EXPECT_TRUE(close(v.index, -v.names, 1.0e-9)) << v.index << " vs " << -v.names;
}

TEST(Dispersion, ProjectedBookUsesOneConcreteCalendarExpiry) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();

  DispersionConfig cfg;
  cfg.projected_maturity = ProjectedMaturitySpec::months(3);
  auto book = build_dispersion_book(make_universe(), *set, cfg);
  ASSERT_TRUE(book.has_value()) << book.error().to_string();

  const std::int64_t expected_expiry = iso_to_ns("2026-10-06");
  ASSERT_EQ(book->positions.size(), 6u);
  for (const DispersionLeg *leg : {&book->index_leg, &book->name_legs[0], &book->name_legs[1]}) {
    EXPECT_EQ(leg->call_definition.expiry_ts_ns, expected_expiry);
    EXPECT_EQ(leg->put_definition.expiry_ts_ns, expected_expiry);
    EXPECT_NE(leg->call_definition.fingerprint, 0u);
    EXPECT_NE(leg->put_definition.fingerprint, 0u);
    EXPECT_EQ(leg->call_definition.contract.side, Side::Call);
    EXPECT_EQ(leg->put_definition.contract.side, Side::Put);
    EXPECT_DOUBLE_EQ(leg->call_definition.contract.K, leg->put_definition.contract.K);
    EXPECT_DOUBLE_EQ(leg->call_definition.contract.T, leg->put_definition.contract.T);
  }
  const DispersionLeg *ordered_legs[] = {&book->index_leg, &book->name_legs[0],
                                         &book->name_legs[1]};
  for (std::size_t i = 0; i < 3u; ++i) {
    EXPECT_EQ(book->positions[2u * i].contract, ordered_legs[i]->call_definition.contract);
    EXPECT_EQ(book->positions[2u * i + 1u].contract, ordered_legs[i]->put_definition.contract);
  }

  const BucketedVega v = price_bucketed_vega(*book, *set, kIndexUid);
  EXPECT_TRUE(close(v.index, -v.names)) << v.index << " vs " << -v.names;
}

// ── 2b. Signal + book are scale-invariant in the basket weights ─────────────
TEST(Dispersion, Signal_ScaleInvariantInWeights) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();

  // Same ratios, different scale (Σ = 1 vs Σ = 5). Normalized internally, so the
  // implied correlation must be identical.
  const DispersionUniverse u_norm = make_universe_w(0.6, 0.4);
  const DispersionUniverse u_scaled = make_universe_w(3.0, 2.0);

  auto sig_norm = dispersion_signal(u_norm, *set, kTargetT);
  auto sig_scaled = dispersion_signal(u_scaled, *set, kTargetT);
  ASSERT_TRUE(sig_norm.has_value()) << sig_norm.error().to_string();
  ASSERT_TRUE(sig_scaled.has_value()) << sig_scaled.error().to_string();

  EXPECT_NEAR(sig_scaled->implied_corr, sig_norm->implied_corr, 1e-12);
  EXPECT_NEAR(sig_scaled->sum_w_sigma, sig_norm->sum_w_sigma, 1e-12);
  EXPECT_NEAR(sig_scaled->sum_w2_sigma2, sig_norm->sum_w2_sigma2, 1e-12);

  // And the book built from the ×k weights is still exactly vega-neutral.
  DispersionConfig cfg;
  auto book = build_dispersion_book(u_scaled, *set, cfg);
  ASSERT_TRUE(book.has_value()) << book.error().to_string();
  const BucketedVega v = price_bucketed_vega(*book, *set, u_scaled.index.uid);
  EXPECT_TRUE(close(v.index, -v.names)) << v.index << " vs " << -v.names;
  EXPECT_TRUE(close(std::fabs(v.index) * kVegaPerVolPoint, cfg.target_vega)) << v.index;
}

// ── 3. with_uid remaps only the uid and reprices bit-identically ────────────
TEST(Dispersion, WithUid_RemapsAndPrices) {
  const PricedSurface base = fit_board(make_board_spec("NM0", 100.0, 0.30));
  auto remapped = with_uid(base, 7);
  ASSERT_TRUE(remapped.has_value()) << remapped.error().to_string();
  EXPECT_EQ(remapped->uid(), 7u);

  std::size_t n = 0;
  for (const SliceContext &ctx : base.context()) {
    const double T = ctx.T;
    const double F = ctx.forward;
    for (const double m : {0.95, 1.0, 1.05}) {
      const double K = F * m;
      const Side side = (m <= 1.0) ? Side::Put : Side::Call;

      EXPECT_TRUE(bits_equal(base.iv(K, T), remapped->iv(K, T))) << "iv K=" << K;
      const auto ga = base.greeks(K, T, side);
      const auto gb = remapped->greeks(K, T, side);
      ASSERT_EQ(ga.has_value(), gb.has_value());
      if (ga.has_value()) {
        EXPECT_TRUE(bits_equal(ga->price, gb->price)) << "price K=" << K;
        EXPECT_TRUE(bits_equal(ga->delta, gb->delta)) << "delta K=" << K;
        EXPECT_TRUE(bits_equal(ga->vega, gb->vega)) << "vega K=" << K;
        ++n;
      }
    }
  }
  EXPECT_GT(n, 0u);
}

// ── 4. Boundary rejections (validation-first; empty set needs no fits) ───────
TEST(Dispersion, Rejections) {
  const std::vector<const PricedSurface *> none;
  auto empty_set = SurfaceSet::create(none);
  ASSERT_TRUE(empty_set.has_value()) << empty_set.error().to_string();

  const DispersionUniverse u = make_universe();
  const DispersionConfig cfg;

  // Fewer than two names.
  {
    DispersionUniverse bad = u;
    bad.names.resize(1);
    auto r = build_dispersion_book(bad, *empty_set, cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  // Non-positive weight sum.
  {
    DispersionUniverse bad = u;
    bad.names[0].weight = -1.0;
    bad.names[1].weight = 0.0;
    auto r = build_dispersion_book(bad, *empty_set, cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  // target_T <= 0.
  {
    DispersionConfig bad = cfg;
    bad.target_T = 0.0;
    auto r = build_dispersion_book(u, *empty_set, bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  // target_vega <= 0.
  {
    DispersionConfig bad = cfg;
    bad.target_vega = -5.0;
    auto r = build_dispersion_book(u, *empty_set, bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  // A member uid missing from the set (valid config + weights).
  {
    auto r = build_dispersion_book(u, *empty_set, cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
  }
  {
    auto r = dispersion_signal(u, *empty_set, kTargetT);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
  }
}

// ── 5. resolve_universe_uids — pure symbol->uid rebinding (no snapshot) ──────
//
// Exercises the callable-seam resolver against a hand-rolled lookup, so the pure
// function is covered without a MarketSnapshot. Happy path rebinds every leg;
// each failure mode Errs loudly with the right code and names the symbol.
TEST(Dispersion, ResolveUniverseUids) {
  // A directory: IDX->10, NM0->20, NM1->30. NM2 and BAD are absent.
  const auto lookup = [](std::string_view s) -> std::optional<std::uint32_t> {
    if (s == "IDX")
      return 10u;
    if (s == "NM0")
      return 20u;
    if (s == "NM1")
      return 30u;
    if (s == "ZERO")
      return 0u; // resolves to the reserved sentinel
    if (s == "DUP")
      return 20u; // collides with NM0's uid
    return std::nullopt;
  };

  const auto make = [](std::string idx, std::vector<std::string> names) {
    DispersionUniverse u;
    u.index = DispersionMember{std::move(idx), 0u, 0.0};
    for (std::string &nm : names) {
      u.names.push_back(DispersionMember{std::move(nm), 0u, 1.0});
    }
    return u;
  };

  // Happy path: every uid rebound from its symbol; symbols + weights untouched.
  {
    auto r = resolve_universe_uids(make("IDX", {"NM0", "NM1"}), lookup);
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    EXPECT_EQ(r->index.uid, 10u);
    EXPECT_EQ(r->index.symbol, "IDX");
    ASSERT_EQ(r->names.size(), 2u);
    EXPECT_EQ(r->names[0].uid, 20u);
    EXPECT_EQ(r->names[1].uid, 30u);
    EXPECT_EQ(r->names[0].weight, 1.0);
  }
  // Empty symbol -> InvalidArgument.
  {
    auto r = resolve_universe_uids(make("", {"NM0", "NM1"}), lookup);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  // Unknown name -> NotFound naming it.
  {
    auto r = resolve_universe_uids(make("IDX", {"NM0", "BAD"}), lookup);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
    EXPECT_NE(r.error().to_string().find("BAD"), std::string::npos) << r.error().to_string();
  }
  // A symbol listed twice -> InvalidArgument naming it.
  {
    auto r = resolve_universe_uids(make("IDX", {"NM0", "NM0"}), lookup);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(r.error().to_string().find("NM0"), std::string::npos) << r.error().to_string();
  }
  // Two symbols collapsing to the same uid -> InvalidArgument naming both.
  {
    auto r = resolve_universe_uids(make("IDX", {"NM0", "DUP"}), lookup);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(r.error().to_string().find("NM0"), std::string::npos) << r.error().to_string();
    EXPECT_NE(r.error().to_string().find("DUP"), std::string::npos) << r.error().to_string();
  }
  // A symbol resolving to the reserved uid 0 -> InvalidArgument.
  {
    auto r = resolve_universe_uids(make("IDX", {"NM0", "ZERO"}), lookup);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(r.error().to_string().find("ZERO"), std::string::npos) << r.error().to_string();
  }
}

// ── S1-3: DropRenormalize drops a missing name and renormalizes survivors ─────
//
// Renormalizing over survivors is exactly equivalent to just deleting the name,
// so the surviving implied_corr must equal a 2-name universe of the survivors
// with their ORIGINAL (un-renormalized) weights — the strongest check available.
TEST(Dispersion, DropRenormalizeSkipsMissingNameAndRenormalizes) {
  const std::vector<PricedSurface> surfaces = build_surfaces(); // IDX=1, NM0=2, NM1=3
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();

  // 3 names, the middle bound to a uid ABSENT from the set (99). Weights are
  // deliberately un-normalized (0.5, 0.3, 0.2) to exercise the renormalization.
  DispersionUniverse u;
  u.index = DispersionMember{"IDX", kIndexUid, 0.0};
  u.names.push_back(DispersionMember{"NM0", kName0Uid, 0.5});
  u.names.push_back(DispersionMember{"MISS", 99u, 0.3});
  u.names.push_back(DispersionMember{"NM1", kName1Uid, 0.2});

  // Error policy (default): the missing name is a hard NotFound — the pre-S1-3
  // abort this task removes.
  {
    auto sig = dispersion_signal(u, *set, kTargetT);
    ASSERT_FALSE(sig.has_value());
    EXPECT_EQ(sig.error().code(), ErrorCode::NotFound);
    EXPECT_NE(sig.error().to_string().find("MISS"), std::string::npos) << sig.error().to_string();
  }

  // DropRenormalize: drop MISS, renormalize over {NM0, NM1}.
  MissingNameSpec drop;
  drop.policy = MissingNamePolicy::DropRenormalize;
  auto sig = dispersion_signal(u, *set, kTargetT, drop);
  ASSERT_TRUE(sig.has_value()) << sig.error().to_string();

  ASSERT_EQ(sig->dropped.size(), 1u);
  EXPECT_EQ(sig->dropped[0].symbol, "MISS");
  EXPECT_EQ(sig->dropped[0].reason, DropReason::SurfaceNotFound);
  EXPECT_FALSE(sig->dropped[0].detail.empty());

  ASSERT_EQ(sig->used_names.size(), 2u);
  EXPECT_EQ(sig->used_names[0], 0u);
  EXPECT_EQ(sig->used_names[1], 2u);
  ASSERT_EQ(sig->sigma_names.size(), 2u);

  // Renormalization actually happened over the SURVIVORS: the implementation's OWN
  // sum_w_sigma / sum_w2_sigma2 must be over weights normalized across survivors
  // (ŵ = w / Σ_survivors w), reconstructed from the sigmas the implementation
  // returned — NOT the raw authored weights. A non-renormalizing implementation
  // would leave Σŵ = 0.7 here and fail this. (Assertion over produced values, not a
  // literal-only tautology.)
  const double w_hat0 = 0.5 / (0.5 + 0.2);
  const double w_hat1 = 0.2 / (0.5 + 0.2);
  const double surv_s0 = sig->sigma_names[0];
  const double surv_s1 = sig->sigma_names[1];
  EXPECT_NEAR(sig->sum_w_sigma, w_hat0 * surv_s0 + w_hat1 * surv_s1, 1e-12);
  EXPECT_NEAR(sig->sum_w2_sigma2,
              w_hat0 * w_hat0 * surv_s0 * surv_s0 + w_hat1 * w_hat1 * surv_s1 * surv_s1, 1e-12);

  // Dropping+renormalizing == a 2-name universe of the survivors with their
  // ORIGINAL weights (bit-identical: the exact same arithmetic sequence).
  DispersionUniverse two;
  two.index = DispersionMember{"IDX", kIndexUid, 0.0};
  two.names.push_back(DispersionMember{"NM0", kName0Uid, 0.5});
  two.names.push_back(DispersionMember{"NM1", kName1Uid, 0.2});
  auto ref = dispersion_signal(two, *set, kTargetT);
  ASSERT_TRUE(ref.has_value()) << ref.error().to_string();
  EXPECT_NEAR(sig->implied_corr, ref->implied_corr, 1e-15);
  EXPECT_TRUE(bits_equal(sig->implied_corr, ref->implied_corr))
      << sig->implied_corr << " vs " << ref->implied_corr;
  EXPECT_TRUE(bits_equal(sig->sum_w_sigma, ref->sum_w_sigma));
  EXPECT_TRUE(bits_equal(sig->sum_w2_sigma2, ref->sum_w2_sigma2));
}

// ── S1-3: the survivor book is vega-neutral over the survivors only ──────────
TEST(Dispersion, DropRenormalizeBookIsVegaNeutralOverSurvivors) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();

  DispersionUniverse u;
  u.index = DispersionMember{"IDX", kIndexUid, 0.0};
  u.names.push_back(DispersionMember{"NM0", kName0Uid, 0.5});
  u.names.push_back(DispersionMember{"MISS", 99u, 0.3});
  u.names.push_back(DispersionMember{"NM1", kName1Uid, 0.2});

  DispersionConfig cfg;
  cfg.missing.policy = MissingNamePolicy::DropRenormalize;
  auto book = build_dispersion_book(u, *set, cfg);
  ASSERT_TRUE(book.has_value()) << book.error().to_string();

  // Survivors only: 1 index + 2 names => 6 positions, 2 name legs, 1 drop.
  EXPECT_EQ(book->positions.size(), 2u * (1u + 2u));
  ASSERT_EQ(book->name_legs.size(), 2u);
  ASSERT_EQ(book->dropped.size(), 1u);
  EXPECT_EQ(book->dropped[0].symbol, "MISS");
  EXPECT_EQ(book->dropped[0].reason, DropReason::SurfaceNotFound);

  // Σ|name-leg gross vega| == index-leg gross vega: vega-neutral over survivors.
  const BucketedVega v = price_bucketed_vega(*book, *set, u.index.uid);
  EXPECT_TRUE(close(v.index, -v.names, 1e-9)) << v.index << " vs " << -v.names;
  EXPECT_TRUE(close(std::fabs(v.index) * kVegaPerVolPoint, cfg.target_vega, 1e-9)) << v.index;
  EXPECT_LT(v.index, 0.0); // short index
}

// ── S1-3: below the minimum surviving basket size the date is Unavailable ────
TEST(Dispersion, BelowMinNamesIsUnavailable) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();

  // 3 names, TWO bound to absent uids => only 1 survivor < min_names (2).
  DispersionUniverse u;
  u.index = DispersionMember{"IDX", kIndexUid, 0.0};
  u.names.push_back(DispersionMember{"NM0", kName0Uid, 0.5});
  u.names.push_back(DispersionMember{"MISS1", 98u, 0.3});
  u.names.push_back(DispersionMember{"MISS2", 99u, 0.2});

  MissingNameSpec drop;
  drop.policy = MissingNamePolicy::DropRenormalize;
  auto sig = dispersion_signal(u, *set, kTargetT, drop);
  ASSERT_FALSE(sig.has_value());
  EXPECT_EQ(sig.error().code(), ErrorCode::Unavailable);
  const std::string msg = sig.error().to_string();
  EXPECT_NE(msg.find("1 of 3"), std::string::npos) << msg;
  EXPECT_NE(msg.find("min 2"), std::string::npos) << msg;

  // min_names < 2 is degenerate (the correlation denominator needs >= 2 names).
  MissingNameSpec bad;
  bad.policy = MissingNamePolicy::DropRenormalize;
  bad.min_names = 1;
  auto r = dispersion_signal(make_universe(), *set, kTargetT, bad);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

// ── S1-3: the index leg is never droppable, under either policy ──────────────
TEST(Dispersion, IndexIsNeverDropped) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();

  DispersionUniverse u = make_universe();      // NM0=2, NM1=3
  u.index = DispersionMember{"IDX", 97u, 0.0}; // index bound to an absent uid

  auto err_sig = dispersion_signal(u, *set, kTargetT); // Error policy
  ASSERT_FALSE(err_sig.has_value());
  EXPECT_EQ(err_sig.error().code(), ErrorCode::NotFound);

  MissingNameSpec drop;
  drop.policy = MissingNamePolicy::DropRenormalize;
  auto drop_sig = dispersion_signal(u, *set, kTargetT, drop);
  ASSERT_FALSE(drop_sig.has_value());
  EXPECT_EQ(drop_sig.error().code(), ErrorCode::NotFound) << "index must never be dropped";
}

// ── S1-3: authoring bugs stay fatal even under DropRenormalize ───────────────
TEST(Dispersion, AuthoringBugsStayFatalUnderDropPolicy) {
  const auto lookup = [](std::string_view s) -> std::optional<std::uint32_t> {
    if (s == "IDX")
      return 10u;
    if (s == "NM0")
      return 20u;
    if (s == "NM1")
      return 30u;
    if (s == "ZERO")
      return 0u; // reserved sentinel
    if (s == "DUP")
      return 20u; // collides with NM0's uid
    return std::nullopt;
  };
  const auto make = [](std::string idx, std::vector<std::string> names) {
    DispersionUniverse u;
    u.index = DispersionMember{std::move(idx), 0u, 0.0};
    for (std::string &nm : names) {
      u.names.push_back(DispersionMember{std::move(nm), 0u, 1.0});
    }
    return u;
  };
  MissingNameSpec drop;
  drop.policy = MissingNamePolicy::DropRenormalize;

  // An empty NAME symbol -> InvalidArgument even under DropRenormalize (an empty
  // symbol is an authoring bug and is never silently dropped as an "absent name").
  {
    auto r = resolve_universe_uids(make("IDX", {"NM0", ""}), lookup, drop);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  // Duplicate symbol -> InvalidArgument even under DropRenormalize.
  {
    auto r = resolve_universe_uids(make("IDX", {"NM0", "NM0"}), lookup, drop);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  // Two symbols colliding to one uid -> InvalidArgument.
  {
    auto r = resolve_universe_uids(make("IDX", {"NM0", "DUP"}), lookup, drop);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  // A symbol resolving to the reserved uid 0 -> InvalidArgument.
  {
    auto r = resolve_universe_uids(make("IDX", {"NM0", "ZERO"}), lookup, drop);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
  // An unknown NAME is DROPPED (not fatal); the index stays bound; order kept.
  {
    auto r = resolve_universe_uids(make("IDX", {"NM0", "GONE", "NM1"}), lookup, drop);
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    ASSERT_EQ(r->dropped.size(), 1u);
    EXPECT_EQ(r->dropped[0].symbol, "GONE");
    EXPECT_EQ(r->dropped[0].reason, DropReason::NotInSnapshot);
    ASSERT_EQ(r->universe.names.size(), 2u);
    EXPECT_EQ(r->universe.names[0].symbol, "NM0");
    EXPECT_EQ(r->universe.names[1].symbol, "NM1");
    EXPECT_EQ(r->universe.index.uid, 10u);
  }
  // An unknown INDEX is a hard NotFound even under DropRenormalize.
  {
    auto r = resolve_universe_uids(make("NOPE", {"NM0", "NM1"}), lookup, drop);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
  }
  // A non-finite weight is InvalidArgument even under DropRenormalize (it can never
  // hide behind a drop): resolve binds fine, dispersion_signal rejects the NaN.
  {
    DispersionUniverse u = make("IDX", {"NM0", "NM1"});
    u.names[0].weight = std::nan("");
    auto ru = resolve_universe_uids(u, lookup, drop);
    ASSERT_TRUE(ru.has_value()) << ru.error().to_string();
    const std::vector<const PricedSurface *> none;
    auto empty = SurfaceSet::create(none);
    ASSERT_TRUE(empty.has_value());
    auto sig = dispersion_signal(ru->universe, *empty, kTargetT, drop);
    ASSERT_FALSE(sig.has_value());
    EXPECT_EQ(sig.error().code(), ErrorCode::InvalidArgument);
  }
}

// ── S1-3: the default Error policy is bit-identical to the pre-S1-3 baseline ──
TEST(Dispersion, ErrorPolicyIsBitIdenticalToBaseline) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();
  const DispersionUniverse u = make_universe(); // 2 names, weights 0.6 / 0.4

  auto sig_default = dispersion_signal(u, *set, kTargetT); // default spec
  auto sig_explicit = dispersion_signal(u, *set, kTargetT, MissingNameSpec{});
  ASSERT_TRUE(sig_default.has_value()) << sig_default.error().to_string();
  ASSERT_TRUE(sig_explicit.has_value());

  // Nothing dropped; survivors are exactly {0, 1}.
  EXPECT_TRUE(sig_default->dropped.empty());
  ASSERT_EQ(sig_default->used_names.size(), 2u);
  EXPECT_EQ(sig_default->used_names[0], 0u);
  EXPECT_EQ(sig_default->used_names[1], 1u);

  // The pre-S1-3 closed form (as in Signal_MatchesClosedForm), reproduced exactly.
  const double si = surfaces[0].iv(surfaces[0].forward_at(kTargetT), kTargetT);
  const double s0 = surfaces[1].iv(surfaces[1].forward_at(kTargetT), kTargetT);
  const double s1 = surfaces[2].iv(surfaces[2].forward_at(kTargetT), kTargetT);
  const double sws = kW0 * s0 + kW1 * s1;
  const double sw2s2 = kW0 * kW0 * s0 * s0 + kW1 * kW1 * s1 * s1;
  const double rho = (si * si - sw2s2) / (sws * sws - sw2s2);
  EXPECT_NEAR(sig_default->implied_corr, rho, 1e-12);
  EXPECT_TRUE(bits_equal(sig_default->implied_corr, sig_explicit->implied_corr));

  // The book under the default (Error) policy: survivors == all names, no drops.
  DispersionConfig cfg;
  auto book = build_dispersion_book(u, *set, cfg);
  ASSERT_TRUE(book.has_value()) << book.error().to_string();
  EXPECT_TRUE(book->dropped.empty());
  ASSERT_EQ(book->name_legs.size(), 2u);
  EXPECT_EQ(book->positions.size(), 2u * (1u + 2u));
  EXPECT_EQ(book->used_names.size(), 2u);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// WS-X-B / X4 â€” weighting-scheme + strike-rule policies, correlation gamma
//
// Each policy gets TWO kinds of test: the default must reproduce the shipped
// behaviour BIT-for-bit, and each non-default setting must be shown to change
// the specific thing it claims to change. A knob that silently does nothing is
// worse than no knob, so "it parsed" is never the assertion.
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// â”€â”€ The vega/gamma/theta identities, against hand-computed arithmetic â”€â”€â”€â”€â”€â”€â”€
//
// Checked against numbers worked out BY HAND from the closed forms, not against
// a second call into the implementation.
TEST(DispersionX4, GammaThetaIdentities_MatchHandComputedValues) {
  // vega = 40, F = 100, sigma = 0.25, T = 0.5
  //   gamma  = vega / (F^2 sigma T) = 40 / (10000 * 0.25 * 0.5) = 40 / 1250 = 0.032
  //   |theta| = 0.5 * sigma * vega / T = 0.5 * 0.25 * 40 / 0.5 = 10
  EXPECT_NEAR(straddle_gamma_from_vega(40.0, 100.0, 0.25, 0.5), 0.032, 1e-15);
  EXPECT_NEAR(straddle_theta_magnitude_from_vega(40.0, 0.25, 0.5), 10.0, 1e-15);

  // A second, independently hand-worked point:
  // vega = 12, F = 50, sigma = 0.4, T = 0.25
  //   gamma  = 12 / (2500 * 0.4 * 0.25) = 12 / 250 = 0.048
  //   |theta| = 0.5 * 0.4 * 12 / 0.25 = 9.6
  EXPECT_NEAR(straddle_gamma_from_vega(12.0, 50.0, 0.4, 0.25), 0.048, 1e-15);
  // 9.6 is not exactly representable, so the tolerance is one ULP at that
  // magnitude rather than an absolute 1e-15 (which would be below it).
  EXPECT_NEAR(straddle_theta_magnitude_from_vega(12.0, 0.4, 0.25), 9.6, 1e-14);

  // Degenerate inputs yield 0 (no risk is defined), never a NaN or a negative.
  const double nan_value = std::numeric_limits<double>::quiet_NaN();
  for (const double bad : {0.0, -1.0, nan_value}) {
    EXPECT_EQ(straddle_gamma_from_vega(bad, 100.0, 0.25, 0.5), 0.0);
    EXPECT_EQ(straddle_gamma_from_vega(40.0, bad, 0.25, 0.5), 0.0);
    EXPECT_EQ(straddle_gamma_from_vega(40.0, 100.0, bad, 0.5), 0.0);
    EXPECT_EQ(straddle_gamma_from_vega(40.0, 100.0, 0.25, bad), 0.0);
    EXPECT_EQ(straddle_theta_magnitude_from_vega(bad, 0.25, 0.5), 0.0);
  }
}

// â”€â”€ DEFAULT IS INERT: the X4 fields default to the shipped construction â”€â”€â”€â”€â”€
TEST(DispersionX4, DefaultPolicies_ReproduceShippedBookBitForBit) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();
  const DispersionUniverse u = make_universe();

  DispersionConfig implicit_cfg; // X4 fields left at their defaults
  DispersionConfig explicit_cfg;
  explicit_cfg.weighting = WeightingScheme::VegaNeutral;
  explicit_cfg.strike = StrikePolicy{StrikeRule::AtmForwardStraddle, 0.0, 0.25};

  auto a = build_dispersion_book(u, *set, implicit_cfg);
  auto b = build_dispersion_book(u, *set, explicit_cfg);
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  ASSERT_TRUE(b.has_value()) << b.error().to_string();

  EXPECT_TRUE(bits_equal(a->index_leg.straddle_qty, b->index_leg.straddle_qty));
  EXPECT_TRUE(bits_equal(a->index_leg.K, b->index_leg.K));
  // Pin the COUNT, not merely that the two agree. `size(a) == size(b)` is
  // satisfied by two EMPTY baskets, which would make both loops below skip and
  // let this -- the test that pins "the default is bit-identical" -- pass green
  // having compared nothing. The fixture universe has exactly two names.
  ASSERT_EQ(a->name_legs.size(), 2u);
  ASSERT_EQ(b->name_legs.size(), 2u);
  for (std::size_t i = 0; i < a->name_legs.size(); ++i) {
    EXPECT_TRUE(bits_equal(a->name_legs[i].straddle_qty, b->name_legs[i].straddle_qty))
        << "leg " << i << " qty diverged under an explicitly-defaulted policy";
    EXPECT_TRUE(bits_equal(a->name_legs[i].K, b->name_legs[i].K)) << "leg " << i << " strike";
  }
  // The default strike rule is ATM-forward, so each leg's strike IS its forward â€”
  // assigned through with no arithmetic, which is what keeps the pin exact.
  EXPECT_TRUE(bits_equal(a->index_leg.K, a->index_leg.forward));
  for (const DispersionLeg &leg : a->name_legs) {
    EXPECT_TRUE(bits_equal(leg.K, leg.forward));
  }
}

// â”€â”€ EqualVega: allocation source changes; equal weights collapse it back â”€â”€â”€â”€
TEST(DispersionX4, EqualVega_ChangesAllocation_AndCollapsesAtEqualWeights) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();

  // (a) UNEQUAL index weights (0.6 / 0.4): EqualVega must move both legs, and in
  //     the predictable direction â€” the underweight name gets MORE.
  {
    const DispersionUniverse u = make_universe_w(kW0, kW1);
    DispersionConfig vega_cfg;
    DispersionConfig equal_cfg;
    equal_cfg.weighting = WeightingScheme::EqualVega;
    auto v = build_dispersion_book(u, *set, vega_cfg);
    auto e = build_dispersion_book(u, *set, equal_cfg);
    ASSERT_TRUE(v.has_value()) << v.error().to_string();
    ASSERT_TRUE(e.has_value()) << e.error().to_string();

    // The index leg is sized identically under every scheme.
    EXPECT_TRUE(bits_equal(v->index_leg.straddle_qty, e->index_leg.straddle_qty));
    // 0.6-weight name loses size (0.6 -> 0.5); 0.4-weight name gains (0.4 -> 0.5).
    EXPECT_LT(std::fabs(e->name_legs[0].straddle_qty), std::fabs(v->name_legs[0].straddle_qty));
    EXPECT_GT(std::fabs(e->name_legs[1].straddle_qty), std::fabs(v->name_legs[1].straddle_qty));
    // Quantitatively: under EqualVega both legs carry the SAME gross vega.
    const double lhs = std::fabs(e->name_legs[0].straddle_qty) * e->name_legs[0].straddle_vega;
    const double rhs = std::fabs(e->name_legs[1].straddle_qty) * e->name_legs[1].straddle_vega;
    EXPECT_NEAR(lhs, rhs, 1e-9 * std::max(lhs, rhs));
  }

  // (b) EQUAL index weights: EqualVega and VegaNeutral describe the same book, so
  //     the two must agree to round-off (they differ only by the documented
  //     divide/multiply round-trip the default path deliberately avoids).
  {
    const DispersionUniverse u = make_universe_w(0.5, 0.5);
    DispersionConfig vega_cfg;
    DispersionConfig equal_cfg;
    equal_cfg.weighting = WeightingScheme::EqualVega;
    auto v = build_dispersion_book(u, *set, vega_cfg);
    auto e = build_dispersion_book(u, *set, equal_cfg);
    ASSERT_TRUE(v.has_value());
    ASSERT_TRUE(e.has_value());
    ASSERT_EQ(v->name_legs.size(), 2u); // else the comparison below is vacuous
    ASSERT_EQ(e->name_legs.size(), 2u);
    for (std::size_t i = 0; i < v->name_legs.size(); ++i) {
      EXPECT_NEAR(e->name_legs[i].straddle_qty, v->name_legs[i].straddle_qty,
                  1e-9 * std::fabs(v->name_legs[i].straddle_qty))
          << "leg " << i;
    }
  }
}

// â”€â”€ GammaNeutral: the basket matches the index on GAMMA, not on vega â”€â”€â”€â”€â”€â”€â”€â”€
TEST(DispersionX4, GammaNeutral_MatchesGammaNotVega) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();
  const DispersionUniverse u = make_universe();

  DispersionConfig vega_cfg;
  DispersionConfig gamma_cfg;
  gamma_cfg.weighting = WeightingScheme::GammaNeutral;
  auto v = build_dispersion_book(u, *set, vega_cfg);
  auto g = build_dispersion_book(u, *set, gamma_cfg);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  ASSERT_TRUE(g.has_value()) << g.error().to_string();

  // THE KNOB DOES SOMETHING: the names' sizes actually move. (The index spot is
  // 500 against names at 100/120, so the gamma-per-vega ratios differ sharply.)
  bool any_moved = false;
  for (std::size_t i = 0; i < v->name_legs.size(); ++i) {
    if (!bits_equal(v->name_legs[i].straddle_qty, g->name_legs[i].straddle_qty)) {
      any_moved = true;
    }
  }
  EXPECT_TRUE(any_moved) << "gamma_neutral produced the vega_neutral book â€” the knob is inert";

  // THE KNOB DOES THE RIGHT THING: gross basket gamma == gross index gamma.
  const auto leg_gamma = [](const DispersionLeg &leg) {
    return straddle_gamma_from_vega(leg.straddle_vega, leg.forward, leg.sigma, leg.T);
  };
  const double index_gamma =
      std::fabs(g->index_leg.straddle_qty) * leg_gamma(g->index_leg) * gamma_cfg.multiplier;
  double basket_gamma = 0.0;
  for (const DispersionLeg &leg : g->name_legs) {
    basket_gamma += std::fabs(leg.straddle_qty) * leg_gamma(leg) * gamma_cfg.multiplier;
  }
  ASSERT_GT(index_gamma, 0.0);
  EXPECT_NEAR(basket_gamma, index_gamma, 1e-9 * index_gamma)
      << "gamma_neutral did not equalize gross gamma";

  // And it is genuinely NOT the vega-neutral book: gross basket vega now differs
  // from the target vega (which vega_neutral matches by construction).
  double basket_vega = 0.0;
  for (const DispersionLeg &leg : g->name_legs) {
    basket_vega += std::fabs(leg.straddle_qty) * leg.straddle_vega * gamma_cfg.multiplier;
  }
  EXPECT_GT(std::fabs(basket_vega * kVegaPerVolPoint - gamma_cfg.target_vega),
            1e-6 * gamma_cfg.target_vega)
      << "gamma_neutral coincidentally reproduced vega neutrality â€” test is not discriminating";
}

// â”€â”€ ThetaNeutral: the basket matches the index on THETA â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
TEST(DispersionX4, ThetaNeutral_MatchesTheta) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();
  const DispersionUniverse u = make_universe();

  DispersionConfig vega_cfg;
  DispersionConfig theta_cfg;
  theta_cfg.weighting = WeightingScheme::ThetaNeutral;
  auto v = build_dispersion_book(u, *set, vega_cfg);
  auto t = build_dispersion_book(u, *set, theta_cfg);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  ASSERT_TRUE(t.has_value()) << t.error().to_string();

  const auto leg_theta = [](const DispersionLeg &leg) {
    return straddle_theta_magnitude_from_vega(leg.straddle_vega, leg.sigma, leg.T);
  };
  const double index_theta =
      std::fabs(t->index_leg.straddle_qty) * leg_theta(t->index_leg) * theta_cfg.multiplier;
  double basket_theta = 0.0;
  for (const DispersionLeg &leg : t->name_legs) {
    basket_theta += std::fabs(leg.straddle_qty) * leg_theta(leg) * theta_cfg.multiplier;
  }
  ASSERT_GT(index_theta, 0.0);
  EXPECT_NEAR(basket_theta, index_theta, 1e-9 * index_theta)
      << "theta_neutral did not equalize gross theta";

  bool any_moved = false;
  for (std::size_t i = 0; i < v->name_legs.size(); ++i) {
    if (!bits_equal(v->name_legs[i].straddle_qty, t->name_legs[i].straddle_qty)) {
      any_moved = true;
    }
  }
  EXPECT_TRUE(any_moved) << "theta_neutral produced the vega_neutral book â€” the knob is inert";
}

// â”€â”€ Strike rule: fixed moneyness actually moves the strike â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
TEST(DispersionX4, FixedMoneyness_MovesTheStrikeOffTheForward) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();
  const DispersionUniverse u = make_universe();

  DispersionConfig atm_cfg;
  auto atm = build_dispersion_book(u, *set, atm_cfg);
  ASSERT_TRUE(atm.has_value()) << atm.error().to_string();

  // k = 0 under the FixedMoneyness rule is ATM-forward by construction: K = F*e^0.
  DispersionConfig zero_cfg;
  zero_cfg.strike = StrikePolicy{StrikeRule::FixedMoneyness, 0.0, 0.25};
  auto zero = build_dispersion_book(u, *set, zero_cfg);
  ASSERT_TRUE(zero.has_value()) << zero.error().to_string();
  EXPECT_NEAR(zero->index_leg.K, atm->index_leg.K, 1e-12 * atm->index_leg.K);

  // A 5% log-moneyness must move every strike by exactly e^0.05.
  DispersionConfig otm_cfg;
  otm_cfg.strike = StrikePolicy{StrikeRule::FixedMoneyness, 0.05, 0.25};
  auto otm = build_dispersion_book(u, *set, otm_cfg);
  ASSERT_TRUE(otm.has_value()) << otm.error().to_string();

  EXPECT_GT(otm->index_leg.K, atm->index_leg.K)
      << "fixed_moneyness left the strike at the forward â€” the knob is inert";
  EXPECT_NEAR(otm->index_leg.K, atm->index_leg.forward * std::exp(0.05), 1e-9 * atm->index_leg.K);
  ASSERT_EQ(otm->name_legs.size(), 2u); // else the per-leg check below is vacuous
  ASSERT_EQ(atm->name_legs.size(), 2u);
  for (std::size_t i = 0; i < otm->name_legs.size(); ++i) {
    EXPECT_NEAR(otm->name_legs[i].K, atm->name_legs[i].forward * std::exp(0.05),
                1e-9 * atm->name_legs[i].K)
        << "leg " << i;
  }
}

// â”€â”€ The delta-strangle rule is refused where it cannot be expressed â”€â”€â”€â”€â”€â”€â”€â”€â”€
TEST(DispersionX4, DeltaStrangle_RefusedOnTheSyntheticTenorPath) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();
  const DispersionUniverse u = make_universe();

  DispersionConfig cfg; // no projected_maturity => the legacy single-strike path
  cfg.strike = StrikePolicy{StrikeRule::DeltaStrangle, 0.0, 0.25};
  auto book = build_dispersion_book(u, *set, cfg);
  ASSERT_FALSE(book.has_value())
      << "a strangle silently degraded to a straddle instead of being refused";
  EXPECT_EQ(book.error().code(), ErrorCode::InvalidArgument);
}

// â”€â”€ Correlation gamma, against hand-computed arithmetic â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
TEST(DispersionX4, CorrelationGamma_MatchesHandComputedDerivatives) {
  // A fully hand-worked two-name case. Weights 0.5/0.5, both names at sigma=0.40,
  // index at sigma=0.30.
  //   A = Sum w^2 sigma^2 = 2 * 0.25 * 0.16       = 0.08
  //   Sum w sigma         = 0.5*0.4 + 0.5*0.4     = 0.40
  //   B = (Sum w sigma)^2 - A = 0.16 - 0.08       = 0.08
  //   dsigma/drho   =  B / (2 sigma)     = 0.08 / 0.6        = 0.13333333333333333
  //   d2sigma/drho2 = -B^2 / (4 sigma^3) = -0.0064 / 0.108   = -0.05925925925925926
  DispersionSignal sig;
  sig.sigma_index = 0.30;
  const double B = 0.08;
  sig.d_sigma_d_rho = B / (2.0 * 0.30);
  sig.d2_sigma_d_rho2 = -(B * B) / (4.0 * 0.30 * 0.30 * 0.30);
  EXPECT_NEAR(sig.d_sigma_d_rho, 0.13333333333333333, 1e-15);
  EXPECT_NEAR(sig.d2_sigma_d_rho2, -0.05925925925925926, 1e-15);

  // Index leg SHORT 10,000 vega (the classic long-dispersion book).
  const double index_signed_vega = -10000.0;
  //   dP/drho = -10000 * 0.13333333333333333 = -1333.3333333333333
  EXPECT_NEAR(correlation_vega(sig, index_signed_vega), -1333.3333333333333, 1e-9);

  // Second derivative, T = 0.5:
  //   volga/vega = -sigma * T / 4 = -0.30 * 0.5 / 4 = -0.0375
  //   bracket    = -0.0375 * (0.13333333333333333^2) + (-0.05925925925925926)
  //              = -0.0006666666666666667 - 0.05925925925925926
  //              = -0.05992592592592593
  //   d2P/drho2  = -10000 * -0.05992592592592593 = 599.2592592592593
  const double T = 0.5;
  EXPECT_NEAR(correlation_gamma(sig, index_signed_vega, T), 599.2592592592593, 1e-8);

  // SIGN CONTRACT. Index vol is CONCAVE in correlation, so the short-index leg is
  // long that convexity: correlation_gamma > 0 here. Flipping the side flips it.
  EXPECT_GT(correlation_gamma(sig, index_signed_vega, T), 0.0);
  EXPECT_LT(correlation_gamma(sig, -index_signed_vega, T), 0.0);
  EXPECT_NEAR(correlation_gamma(sig, index_signed_vega, T),
              -correlation_gamma(sig, -index_signed_vega, T), 1e-9);
}

// The signal's derivative fields must satisfy the closed form on real surfaces,
// AND agree with a finite difference of the function they differentiate â€” which
// validates them against sigma(rho) itself, not against their own formulas.
TEST(DispersionX4, CorrelationDerivatives_SatisfyTheClosedFormOnRealSurfaces) {
  const std::vector<PricedSurface> surfaces = build_surfaces();
  auto set = SurfaceSet::create(as_ptrs(surfaces));
  ASSERT_TRUE(set.has_value()) << set.error().to_string();
  const DispersionUniverse u = make_universe();

  auto sig = dispersion_signal(u, *set, kTargetT);
  ASSERT_TRUE(sig.has_value()) << sig.error().to_string();

  const double B = sig->sum_w_sigma * sig->sum_w_sigma - sig->sum_w2_sigma2;
  ASSERT_GT(B, 0.0);
  EXPECT_NEAR(sig->d_sigma_d_rho, B / (2.0 * sig->sigma_index), 1e-14);
  EXPECT_NEAR(sig->d2_sigma_d_rho2, -(B * B) / (4.0 * std::pow(sig->sigma_index, 3.0)), 1e-14);
  // Index vol is concave in rho whenever the cross-term sum is positive.
  EXPECT_LT(sig->d2_sigma_d_rho2, 0.0);
  EXPECT_GT(sig->d_sigma_d_rho, 0.0);

  // FINITE-DIFFERENCE CHECK against sigma(rho) = sqrt(A + rho B).
  const double A = sig->sum_w2_sigma2;
  const double rho = sig->implied_corr;
  const double h = 1e-5;
  const auto sigma_of = [&](double r) { return std::sqrt(A + r * B); };
  const double fd1 = (sigma_of(rho + h) - sigma_of(rho - h)) / (2.0 * h);
  const double fd2 = (sigma_of(rho + h) - 2.0 * sigma_of(rho) + sigma_of(rho - h)) / (h * h);
  EXPECT_NEAR(sig->d_sigma_d_rho, fd1, 1e-7);
  EXPECT_NEAR(sig->d2_sigma_d_rho2, fd2, 1e-4);
}
