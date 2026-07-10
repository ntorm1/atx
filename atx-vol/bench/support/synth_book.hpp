#pragma once

// Data-free synthetic market + position book for the portfolio throughput bench.
//
// WHY LOCAL (fixture-reuse note): the brief asks to reuse an existing synthetic
// fixture (tests/support/, corpus.cpp, panel.cpp, make_index_spec). Those all
// build a market from a FIT — `build_corpus` de-Americanizes and fits real/loaded
// OPRA boards through a VolaSession, which needs data on disk and pays a full
// solve per board: unsuitable for a fast, deterministic, data-free throughput
// bench. The only synthetic *PricedSurface* builder in the tree is inline in
// examples/portfolio_pricer_bench.cpp (make_convex / make_essvi) — an example,
// not a shared library fixture. So per the brief's fallback ("otherwise factor
// the fixture into bench/ locally") this header lifts that exact construction
// into one reusable place, parameterized by underlying/slice/strike counts so a
// single generator drives the whole {n_unique} x {dedup ratio} matrix.
//
// The surfaces mix ConvexDenseCurve (index/override, cold-ALO served) and eSSVI
// slices, matching the production cold path the portfolio pricer runs.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "atx/vol/black76.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

namespace atx::vol::bench {

inline constexpr double kSpot = 100.0;
inline constexpr double kRate = 0.043;

// The realistic strike ladder (per underlying, per slice). Puts below spot,
// calls above — the OTM leg each side, the way a real book is struck.
inline constexpr double kStrikes[] = {85.0, 92.0, 98.0, 100.0, 102.0, 108.0, 115.0};
inline constexpr int kNumStrikes = 7;

[[nodiscard]] inline PricingContext pc_of(std::uint32_t uid) {
  PricingContext pc;
  pc.S = kSpot;
  pc.r = kRate;
  pc.now_ts_ns = 1700000000000000000LL;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();  // the resolved served preset
  pc.uid = uid;
  return pc;
}

// The i-th slice maturity (ascending). slices in [1, ~8] stay well inside a year.
[[nodiscard]] inline double slice_T(int i) { return 0.05 + 0.12 * static_cast<double>(i); }

[[nodiscard]] inline PricedSurface make_essvi(std::uint32_t uid, int n_slices) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n_slices; ++i) {
    const double T = slice_T(i);
    EssviParams e{};
    e.theta = 0.04 + 0.006 * static_cast<double>(i);
    e.phi = 1.4 - 0.04 * static_cast<double>(i);
    e.rho = -0.35 + 0.015 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = kSpot;
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kRate * T)));
    ctx.push_back(SliceContext{T, kSpot, 0.0, 0.02, 200, 5});
  }
  return PricedSurface::create(std::move(cs), std::move(ctx), pc_of(uid)).value();
}

[[nodiscard]] inline PricedSurface make_convex(std::uint32_t uid, int n_slices, int nodes) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  for (int i = 0; i < n_slices; ++i) {
    const double T = slice_T(i);
    const double df = std::exp(-kRate * T);
    const double sigma = 0.18 + 0.01 * static_cast<double>(i);
    ConvexSliceFit fit;
    fit.T = T;
    fit.F = kSpot;
    fit.df = df;
    fit.rmse_price = 0.3;
    fit.n_obs = static_cast<std::size_t>(nodes);
    fit.n_active = 4;
    fit.u.resize(static_cast<std::size_t>(nodes));
    fit.C.resize(static_cast<std::size_t>(nodes));
    for (int j = 0; j < nodes; ++j) {
      const double K =
          kSpot * (0.7 + 0.6 * static_cast<double>(j) / static_cast<double>(nodes - 1));
      fit.u[static_cast<std::size_t>(j)] = K;
      fit.C[static_cast<std::size_t>(j)] = black76_price(kSpot, K, T, sigma, df, Side::Call);
    }
    cs.push(std::make_unique<ConvexDenseCurve>(std::move(fit)));
    ctx.push_back(SliceContext{T, kSpot, 0.0, 0.02, static_cast<std::size_t>(nodes), 3});
  }
  return PricedSurface::create(std::move(cs), std::move(ctx), pc_of(uid)).value();
}

// A whole synthetic market: one surface per underlying (mixed kinds) plus a
// bumped (spot +0.4%, rate +10bp) shifted set for pnl_explain. Move-safe: the
// SurfaceSet pointers reference elements of the owned vectors, whose element
// addresses survive the vector move performed by return-by-value.
struct SynthMarket {
  std::vector<PricedSurface> base;
  std::vector<PricedSurface> shifted;
  std::vector<const PricedSurface*> base_ptrs;
  std::vector<const PricedSurface*> shifted_ptrs;
  // SurfaceSet has a private default ctor (factory-only), so hold via optional
  // and expose refs. The stored pointers reference `base`/`shifted` elements,
  // whose addresses survive the vector move on return-by-value.
  std::optional<SurfaceSet> base_set_;
  std::optional<SurfaceSet> shifted_set_;

  [[nodiscard]] const SurfaceSet& base_set() const { return *base_set_; }
  [[nodiscard]] const SurfaceSet& shifted_set() const { return *shifted_set_; }
};

[[nodiscard]] inline SynthMarket build_market(int n_underlyings, int n_slices, int convex_nodes) {
  SynthMarket m;
  m.base.reserve(static_cast<std::size_t>(n_underlyings));
  m.shifted.reserve(static_cast<std::size_t>(n_underlyings));
  for (int u = 1; u <= n_underlyings; ++u) {
    const auto uid = static_cast<std::uint32_t>(u);
    m.base.push_back((u & 1) ? make_convex(uid, n_slices, convex_nodes)
                             : make_essvi(uid, n_slices));
  }
  for (const PricedSurface& s : m.base) {
    CurveSurface c = s.surface().clone();
    std::vector<SliceContext> ctx(s.context().begin(), s.context().end());
    PricingContext pc = s.pricing();
    pc.S += 0.004 * pc.S;
    pc.r += 0.001;
    m.shifted.push_back(PricedSurface::create(std::move(c), std::move(ctx), pc).value());
  }
  m.base_ptrs.reserve(m.base.size());
  m.shifted_ptrs.reserve(m.shifted.size());
  for (const PricedSurface& s : m.base) {
    m.base_ptrs.push_back(&s);
  }
  for (const PricedSurface& s : m.shifted) {
    m.shifted_ptrs.push_back(&s);
  }
  m.base_set_ = SurfaceSet::create(m.base_ptrs).value();
  m.shifted_set_ = SurfaceSet::create(m.shifted_ptrs).value();
  return m;
}

// The full grid of unique contracts across the market (uid x slice x strike),
// in a stable order. Take the first `n_unique` for a target unique count.
[[nodiscard]] inline std::vector<OptionContract> unique_contracts(int n_underlyings,
                                                                  int n_slices) {
  std::vector<OptionContract> out;
  out.reserve(static_cast<std::size_t>(n_underlyings) * static_cast<std::size_t>(n_slices) *
              static_cast<std::size_t>(kNumStrikes));
  for (int u = 1; u <= n_underlyings; ++u) {
    for (int i = 0; i < n_slices; ++i) {
      const double T = slice_T(i);
      for (int s = 0; s < kNumStrikes; ++s) {
        const double K = kStrikes[s];
        const Side side = (K <= kSpot) ? Side::Put : Side::Call;
        out.push_back(OptionContract{static_cast<std::uint32_t>(u), K, T, side});
      }
    }
  }
  return out;
}

// A position book with EXACTLY `n_unique` distinct contracts and
// `n_unique * positions_per_unique` positions (dedup collapses the repeats). The
// dedup ratio is therefore `positions_per_unique:1`, always known to the caller
// that emits the paired positions/s + unique_contracts/s counters.
[[nodiscard]] inline std::vector<Position> make_book(int n_underlyings, int n_slices,
                                                     std::size_t n_unique,
                                                     std::size_t positions_per_unique) {
  const std::vector<OptionContract> all = unique_contracts(n_underlyings, n_slices);
  const std::size_t uniq = std::min(n_unique, all.size());
  std::vector<Position> book;
  book.reserve(uniq * positions_per_unique);
  std::uint64_t id = 0;
  for (std::size_t rep = 0; rep < positions_per_unique; ++rep) {
    for (std::size_t c = 0; c < uniq; ++c) {
      book.push_back(Position{id++, all[c], 5.0, 100.0});
    }
  }
  return book;
}

}  // namespace atx::vol::bench
