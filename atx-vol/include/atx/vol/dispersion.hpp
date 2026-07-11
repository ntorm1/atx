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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/contract_projection.hpp" // ProjectedMaturitySpec, definitions
#include "atx/vol/portfolio_pricer.hpp"    // Position, SurfaceSet
#include "atx/vol/priced_surface.hpp"      // PricedSurface
#include "atx/vol/types.hpp"               // Result

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

// ── Missing-name handling (S1-3) ────────────────────────────────────────────
//
// Over a long horizon some basket member is periodically unavailable on some
// date (halted, delisted, no fittable board). `MissingNamePolicy` picks what
// happens: abort the whole run (Error, the pre-S1-3 behaviour) or drop the
// offending name, renormalize the surviving basket weights (Σŵ = 1 over
// survivors) and continue (DropRenormalize). The INDEX leg is never droppable.
enum class MissingNamePolicy : std::uint8_t {
  Error = 0,           // any missing/unusable name is a hard Err (pre-S1-3 behaviour)
  DropRenormalize = 1, // drop it, renormalize over survivors, continue
};

// Why a basket member was dropped (recorded verbatim — a drop is never silent).
enum class DropReason : std::uint8_t {
  NotInSnapshot = 0,   // symbol absent from the snapshot directory (resolve step)
  SurfaceNotFound = 1, // resolved uid not registered in the SurfaceSet
  Unavailable = 2,     // no ATM forward / ATM vol NaN / degenerate straddle vega
};

// One dropped member: the symbol, why it went, and the underlying error message
// verbatim, so every drop is auditable.
struct DroppedName {
  std::string symbol;
  DropReason reason{DropReason::NotInSnapshot};
  std::string detail; // the underlying error message, verbatim
};

// Missing-name policy + the minimum SURVIVING basket size. Below `min_names`
// survivors a date has no tradeable book (the implied-correlation identity needs
// >= 2 names, so `min_names < 2` is InvalidArgument).
struct MissingNameSpec {
  MissingNamePolicy policy{MissingNamePolicy::Error};
  std::size_t min_names{2};
};

// A symbol -> uid lookup (typically MarketSnapshot::uid_of, bound by the caller).
// Kept as a callable seam so this module never learns about MarketSnapshot / the
// backtest layer — see the "Purity" note above. Called once per snapshot, never
// in a hot loop, so `std::function` is fine.
using SymbolUidLookup = std::function<std::optional<std::uint32_t>(std::string_view)>;

// Return a copy of `universe` with every member's `uid` rebound from its `symbol`
// via `lookup`. Symbols and weights are untouched, so a universe authored purely
// in symbols (uid=0) becomes resolvable against any snapshot regardless of its
// uid scheme. The index weight stays as authored (it is ignored downstream).
//
// Fails loudly rather than silently dropping or double-counting a leg:
//   InvalidArgument — any member symbol is empty; the same symbol appears twice;
//                     two members resolve to the SAME uid; or a symbol resolves
//                     to the reserved uid 0.
//   NotFound        — `lookup(symbol)` is nullopt for the index or any name.
// The message names the offending symbol(s).
[[nodiscard]] Result<DispersionUniverse> resolve_universe_uids(const DispersionUniverse &universe,
                                                               const SymbolUidLookup &lookup);

// The result of a policy-aware resolve: the survivors (input order) plus the
// names dropped because their symbol was absent from the snapshot directory
// (reason == NotInSnapshot). Under the Error policy `dropped` is always empty.
struct ResolvedUniverse {
  DispersionUniverse universe;      // survivors only, in input order
  std::vector<DroppedName> dropped; // reason == NotInSnapshot
};

// Policy-aware resolve. Under `Error` this is the 2-arg overload's behaviour
// exactly (any missing name is NotFound). Under `DropRenormalize` an unknown
// NAME symbol is dropped (recorded NotInSnapshot) and resolution continues; the
// INDEX symbol is never droppable (an unknown index stays a hard NotFound).
// Authoring bugs — an empty symbol, a symbol listed twice, two symbols resolving
// to the same uid, or a symbol resolving to the reserved uid 0 — stay hard
// InvalidArgument under BOTH policies. The 2-arg overload delegates here with
// `MissingNameSpec{Error, 2}`.
[[nodiscard]] Result<ResolvedUniverse> resolve_universe_uids(const DispersionUniverse &universe,
                                                             const SymbolUidLookup &lookup,
                                                             MissingNameSpec missing);

// Which side of the dispersion the book takes. Signs the index vs. name legs.
enum class DispersionSide : std::uint8_t {
  ShortIndexLongNames = 0, // classic long-dispersion: sell index vol, buy names
  LongIndexShortNames = 1,
};

// Sizing / construction policy for a dispersion book.
struct DispersionConfig {
  double target_T{30.0 / 365.25}; // straddle tenor (year-fraction), > 0
  double target_vega{10000.0};    // index-leg gross vega the book scales to, > 0
  DispersionSide side{DispersionSide::ShortIndexLongNames};
  double multiplier{100.0};  // option contract multiplier, > 0
  MissingNameSpec missing{}; // missing-name policy (default Error => pre-S1-3 book)
  // When set, build the surface-only book from exact projected definitions. The
  // index projection fixes the absolute expiry and every constituent is then
  // projected to that same timestamp. nullopt preserves the legacy target_T path.
  std::optional<ProjectedMaturitySpec> projected_maturity{};
  bool record_diagnostics{false};
};

// Opt-in implied-correlation diagnostic for one snapshot. It resolves forwards
// and ATM IV only; no option mark or American Greek is evaluated. Under
// DropRenormalize, used_names and sigma_names describe the surviving basket.
struct DispersionSignal {
  double T_used{0.0};
  double sigma_index{0.0};
  std::vector<double> sigma_names;     // parallel to `used_names`
  double sum_w_sigma{0.0};             // Σ w_hat_i sigma_i   (normalized weights)
  double sum_w2_sigma2{0.0};           // Σ w_hat_i^2 sigma_i^2
  double implied_corr{0.0};            // rho_imp
  std::vector<std::size_t> used_names; // survivor indices into names (ascending)
  std::vector<DroppedName> dropped;    // input order; empty under Error
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
  double call_mark{0.0};
  double put_mark{0.0};
  ProjectedOptionDefinition call_definition{};
  ProjectedOptionDefinition put_definition{};
};

// A fully-sized dispersion book. Book construction does not compute implied
// correlation. Each sizing leg is evaluated once, and entry_marks carries the
// already-produced per-share marks in parallel with positions. Under
// DropRenormalize, used_names contains survivor indices and dropped records
// removed names.
struct DispersionBook {
  DispersionLeg index_leg;
  std::vector<DispersionLeg> name_legs;
  std::vector<std::size_t> used_names;
  std::vector<Position> positions;
  std::vector<double> entry_marks;
  std::vector<DroppedName> dropped;
};

// Compute the implied-correlation signal at tenor `T` (year-fraction). Resolves
// each member's ATM straddle from its uid in `surfaces` (K = forward_at(T),
// sigma = iv(K, T)). `missing` selects the missing-name policy (default Error is
// pre-S1-3 behaviour, so the result is bit-identical when nothing is missing).
//
// The INDEX leg is never droppable — if it fails the error propagates unchanged
// under BOTH policies. Under DropRenormalize a NAME whose leg is NotFound /
// Unavailable is dropped (recorded in `dropped`) and the surviving basket weights
// are renormalized over the survivors; a non-finite weight is ALWAYS
// InvalidArgument (never hidden behind a drop). Below `missing.min_names`
// survivors the date has no tradeable book -> Unavailable.
//
// Errors:
//   InvalidArgument — fewer than two names (Error policy); any weight non-finite;
//                     Σ weights <= 0; T non-finite or <= 0; min_names < 2;
//                     degenerate correlation denominator.
//   NotFound        — a member's uid is not registered in `surfaces` (Error), or
//                     the index uid is missing (both policies).
//   Unavailable     — a member's ATM straddle is unusable (Error), or fewer than
//                     min_names names survived (DropRenormalize).
// The message names the offending symbol on a per-member failure.
[[nodiscard]] Result<DispersionSignal> dispersion_signal(const DispersionUniverse &universe,
                                                         const SurfaceSet &surfaces, double T,
                                                         MissingNameSpec missing = {});

// Build the full vega-neutral dispersion book without evaluating the optional
// implied-correlation diagnostic. Each surviving leg is resolved exactly once;
// its vega sizes the position and its mark is retained for entry materialization.
// Basket weights are normalized over used_names, preserving exact vega neutrality.
//
// Errors include invalid sizing configuration, an unavailable index, insufficient
// surviving names, or a surviving basket with non-positive total weight.
[[nodiscard]] Result<DispersionBook> build_dispersion_book(const DispersionUniverse &universe,
                                                           const SurfaceSet &surfaces,
                                                           const DispersionConfig &cfg);

// Return a copy of `src` with only its `pricing().uid` replaced by `uid` (curves
// and per-slice context deep-cloned; spot / rate / pricer / AL preset unchanged),
// so it prices bit-identically to `src` but resolves under a distinct uid. The
// remap tool the universe binding depends on.
[[nodiscard]] Result<PricedSurface> with_uid(const PricedSurface &src, std::uint32_t uid);

} // namespace atx::vol
