#pragma once

// DispersionBook — a vega-weighted straddle dispersion book with an
// implied-correlation signal, built directly on the PricedSurface / SurfaceSet /
// Position portfolio layer (portfolio_pricer.hpp).
//
// ## The trade
//
// A dispersion trade sells index volatility against a basket of single-name
// volatilities (or the reverse), sized vega-neutral: the index straddle's gross
// vega is matched by the basket's gross vega, so a parallel vol move nets to
// (approximately) zero and the position isolates the SPREAD between index and
// average single-name vol — i.e. the market's implied correlation.
//
// ## The signal: implied correlation
//
// With index ATM vol sigma_idx, constituent ATM vols sigma_i, and index weights
// w_i, the market-implied average pairwise correlation is
//
//   rho_imp = (sigma_idx^2 - Σ w_i^2 sigma_i^2)
//             / ((Σ w_i sigma_i)^2 - Σ w_i^2 sigma_i^2)
//
// The denominator is the closed form of Σ_{i≠j} w_i w_j sigma_i sigma_j (the
// cross-term sum): (Σ w_i sigma_i)^2 - Σ w_i^2 sigma_i^2 == Σ_{i≠j} .... The
// closed form is O(n) and numerically identical.
//
// ## uid discipline (why `with_uid` exists)
//
// SurfaceSet resolves a Position by `contract.uid == PricedSurface::uid()` and
// rejects duplicate uids. Synthetically-built surfaces frequently share uid=0, so
// a caller assembling a universe must stamp a DISTINCT uid on each member's
// surface. `with_uid` returns a copy of a surface with only its `pricing().uid`
// replaced (curves + context cloned; everything else — and therefore every priced
// value — identical), so the universe's `DispersionMember::uid` binds end-to-end
// to a resolvable surface.
//
// ## Purity
//
// Every function here is pure and stateless: it reads a `SurfaceSet` and returns a
// value (signal or book). No backtest engine, no clock, no I/O. The emitted
// `Position`s are ready to hand to `Portfolio::create` / `PortfolioPricer`.

#include <cstdint>
#include <string>
#include <vector>

#include "atx/vol/portfolio_pricer.hpp"  // Position, SurfaceSet
#include "atx/vol/priced_surface.hpp"    // PricedSurface
#include "atx/vol/types.hpp"             // Result

namespace atx::vol {

// One member of a dispersion universe: a symbol, the uid its PricedSurface
// carries (matched by SurfaceSet), and its index weight w_i. The index member's
// weight is ignored (only the constituents' weights enter the signal / sizing).
struct DispersionMember {
  std::string symbol;
  std::uint32_t uid{0};
  double weight{0.0};
};

// A dispersion universe: one index leg plus its basket constituents (>= 2).
struct DispersionUniverse {
  DispersionMember index;
  std::vector<DispersionMember> names;
};

// Which side of the dispersion the book takes. Signs the index vs. name legs.
enum class DispersionSide : std::uint8_t {
  ShortIndexLongNames = 0,  // classic long-dispersion: sell index vol, buy names
  LongIndexShortNames = 1,
};

// Sizing / construction policy for a dispersion book.
struct DispersionConfig {
  double target_T{30.0 / 365.25};  // straddle tenor (year-fraction), > 0
  double target_vega{10000.0};     // index-leg gross vega the book scales to, > 0
  DispersionSide side{DispersionSide::ShortIndexLongNames};
  double multiplier{100.0};        // option contract multiplier, > 0
};

// The implied-correlation signal at one snapshot (cheap; no positions). The
// per-name vectors are parallel to `DispersionUniverse::names`. Basket weights are
// NORMALIZED internally (w_hat_i = w_i / Σw), so the signal is scale-invariant and
// correct for any positive-weight vector; the sums below are over w_hat.
struct DispersionSignal {
  double T_used{0.0};
  double sigma_index{0.0};
  std::vector<double> sigma_names;
  double sum_w_sigma{0.0};      // Σ w_hat_i sigma_i   (normalized weights)
  double sum_w2_sigma2{0.0};    // Σ w_hat_i^2 sigma_i^2
  double implied_corr{0.0};     // rho_imp
};

// Per-leg sizing diagnostic. `straddle_vega` is the per-share ATM straddle vega
// (call vega + put vega); `straddle_qty` is the signed contract count on EACH of
// the two legs (call and put) of this straddle.
struct DispersionLeg {
  std::string symbol;
  std::uint32_t uid{0};
  double K{0.0};
  double T{0.0};
  double sigma{0.0};
  double straddle_vega{0.0};
  double straddle_qty{0.0};
};

// A fully-sized dispersion book: the signal, the per-leg diagnostics, and the
// flat position list (two positions — a Call then a Put at the same K/T/qty — per
// straddle, so positions.size() == 2 * (1 + names.size())), ready for
// Portfolio::create / PortfolioPricer.
struct DispersionBook {
  DispersionSignal signal;
  DispersionLeg index_leg;
  std::vector<DispersionLeg> name_legs;
  std::vector<Position> positions;
};

// Compute the implied-correlation signal at tenor `T` (year-fraction). Resolves
// each member's ATM straddle from its uid in `surfaces` (K = forward_at(T),
// sigma = iv(K, T)).
//
// Errors:
//   InvalidArgument — fewer than two names; any weight non-finite; Σ weights <= 0;
//                     T non-finite or <= 0; degenerate correlation denominator.
//   NotFound        — a member's uid is not registered in `surfaces`.
//   Unavailable     — a member's ATM vol is NaN (tenor outside its surface domain).
// The message names the offending symbol on a per-member failure.
[[nodiscard]] Result<DispersionSignal> dispersion_signal(const DispersionUniverse& universe,
                                                         const SurfaceSet& surfaces, double T);

// Build the full vega-neutral dispersion book (signal + sized straddle positions)
// at `cfg.target_T`, scaled so the index leg carries `cfg.target_vega` of gross
// vega and the weighted basket matches it.
//
// Sizing (vega-neutral); basket weights normalized internally, w_hat_i = w_i / Σw:
//   n_idx = ± target_vega / (straddle_vega_idx · multiplier)
//   n_i   = ∓ w_hat_i · target_vega / (straddle_vega_i · multiplier)
// with the sign set by `side` (ShortIndexLongNames => index short, names long).
// The sign applies to BOTH legs of each straddle. Because Σ w_hat_i = 1, the basket
// gross vega equals the index leg exactly for ANY positive-weight input.
//
// Errors: those of `dispersion_signal`, plus InvalidArgument if target_T,
// target_vega, or multiplier is non-finite or <= 0, and Unavailable if any
// member's ATM straddle vega is non-finite or <= 0 (message names the symbol).
[[nodiscard]] Result<DispersionBook> build_dispersion_book(const DispersionUniverse& universe,
                                                           const SurfaceSet& surfaces,
                                                           const DispersionConfig& cfg);

// Return a copy of `src` with only its `pricing().uid` replaced by `uid` (curves
// and per-slice context deep-cloned; spot / rate / pricer / AL preset unchanged),
// so it prices bit-identically to `src` but resolves under a distinct uid. The
// remap tool the universe binding depends on.
[[nodiscard]] Result<PricedSurface> with_uid(const PricedSurface& src, std::uint32_t uid);

}  // namespace atx::vol
