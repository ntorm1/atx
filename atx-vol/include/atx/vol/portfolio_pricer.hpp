#pragma once

// PortfolioPricer — a PricedSurface-native portfolio pricer + Taylor PnL-explain.
//
// The v3 curve framework's serialization currency is `PricedSurface` (a fitted,
// cache-free surface of any curve kind whose `greeks(K,T,side)` reproduce a
// session's cold served theo bit-for-bit — see priced_surface.hpp). This module
// is the portfolio layer built directly on it, replacing the legacy
// `portfolio.hpp` (a faithful C `ats-vol` port bound to
// VolSurface/Universe/MarketBinding/CorrectionCache) for the new-surface world.
// The two coexist: portfolio.hpp is untouched; nothing here depends on it.
//
// ## What it does
//
//   1. `Portfolio` — a book of `Position`s over N unique underlyings and M unique
//      contracts. It DEDUPS contracts on `(uid,K,T,side)` so each distinct
//      contract is priced exactly once (positions scale the shared result by
//      qty * multiplier) — the throughput lever behind "M unique contracts".
//   2. `SurfaceSet` — a non-owning uid -> PricedSurface resolver, built from a
//      plain vector of surfaces (each surface knows its own `uid()`).
//   3. `PortfolioPricer::price` — price the book against one surface per
//      underlying, emitting a per-position SoA frame of
//      {id, pv, price/iv, first- AND second-order Greeks} plus a portfolio total.
//   4. `PortfolioPricer::pnl_explain` — reprice the book against a BASE and a
//      SHIFTED surface per underlying and decompose each position's PnL into a
//      full Taylor expansion (delta/gamma, vega/volga, vanna, theta, rho, charm)
//      plus the unexplained residual, alongside the base/target valuations.
//
// ## Price basis: American mark, American cold-FD Greeks
//
// The reported per-share price / PV is the American Andersen-Lake `fair_value`
// (the accurate served theo — the surface reproduces its session's board accuracy
// bit-for-bit). The Greeks are `PricedSurface::greeks`, which on the cold
// (null-correction-cache) path are AMERICAN sensitivities from central finite
// differences on the same cold `american_price` the mark uses (`american_greeks_fd`),
// so `greeks().price == fair_value()` bit-for-bit and the coefficients are American.
// So `pnl_explain` decomposes the FULL American PnL (fair_value change) with
// American Greeks: the `pnl_unexplained` residual is the pure higher-order Taylor
// tail (the early-exercise premium is now carried by the American delta/gamma/…, so
// a spot-only move reconstructs to ~1e-4 relative rather than the early-exercise
// premium's full magnitude).
//
// ## Why it is SOTA-fast
//
// The expensive per-contract kernel is the SOTA cold Andersen-Lake solve: one
// `fair_value` solve plus the ~17 solves the cold-FD Greeks stencil runs (in
// `price`, per unique contract; in `pnl_explain`, ~17 base-Greeks + 2 marks). The
// pricer adds only a bit-hash dedup, a pointer lookup, and float multiplies per row,
// and fans the solves out across `std::jthread`s writing disjoint output slots. The
// output is deterministic across thread counts: the parallel section writes
// per-contract results into disjoint slots, and the position scatter + total
// reduction run serially in input order (no float-add reordering). (On the eSSVI
// hot path the served correction cache makes Greeks analytic; the FD cost is only
// the cold override/index path.)
//
// ## Greek / Taylor conventions (American, spot-based)
//
//   delta = dP/dS      gamma = d2P/dS2      vega  = dP/dsigma   volga = d2P/dsigma2
//   vanna = d2P/dS dsigma   theta = dP/dt (calendar; = -dP/dT)  rho = dP/dr
//   charm = d2P/dS dt
//
// All frame Greeks and PnL components are POSITION scaled (qty * multiplier *
// per-share); `price`/`iv` columns are per-share (`price` is the American mark).
//
// ## Thread-safety
//
// Except for `retime`, a `PortfolioPricer` is immutable after construction;
// `price` / `pnl_explain` are const, and the `PricedSurface` inputs are
// concurrent-const-safe, so one pricer may be queried from many threads. Every
// concurrent in-place call must use a distinct PortfolioWorkspace and output
// view. `retime` requires exclusive access to the pricer and every workspace
// that has priced it; a later call rebuilds each workspace's retained substrate.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/adjusted_greeks.hpp"     // StickyParams
#include "atx/vol/dividend.hpp"            // DividendEvent, HybridDivParams
#include "atx/vol/priced_surface.hpp"      // PricedSurface, PricingContext
#include "atx/vol/priced_surface_view.hpp" // PricedSurfaceView (WS-ZC1 borrowed surfaces)
#include "atx/vol/query_pricing.hpp"       // QueryExecution
#include "atx/vol/types.hpp"               // Result, Status, Side

namespace atx::vol {

class Portfolio;
struct PriceFrame;
struct PriceTotals;
enum class RiskBucketKey : std::uint8_t;
struct RiskBucket;
struct PnlFrame;
struct PnlTotals;
struct PnlRiskBucket;

[[nodiscard]] Result<std::vector<RiskBucket>>
reduce_risk_buckets(const PriceFrame &frame, const Portfolio &pf, RiskBucketKey by,
                    PriceTotals *grand);
[[nodiscard]] Result<std::vector<PnlRiskBucket>>
reduce_pnl_risk_buckets(const PnlFrame &frame, const Portfolio &pf, RiskBucketKey by,
                        PnlTotals *grand);

// Calendar year length in nanoseconds — the library's T convention
// (data.cpp `year_fraction`: 365.25 * 86400 * 1e9). Used to convert a
// base->shifted valuation-timestamp gap into the theta/charm time-roll dt.
inline constexpr double kNsPerYear = 365.25 * 86400.0 * 1.0e9;

// ── Position / contract model ────────────────────────────────────────────

// A listed option contract keyed to an underlying's PricedSurface. `uid` is
// matched against `PricedSurface::uid()`; `T` is the year-fraction to expiry at
// the pricing valuation (the shifted pass rolls it by the surfaces' time gap).
struct OptionContract {
  std::uint32_t uid{0};
  double K{0.0};
  double T{0.0};
  Side side{Side::Call};

  [[nodiscard]] bool operator==(const OptionContract &) const = default;
};

// One held position. `id` is an opaque caller key echoed in every frame row.
struct Position {
  std::uint64_t id{0};
  OptionContract contract{};
  double qty{0.0};          // signed: + long / - short
  double multiplier{100.0}; // deliverable (<=0 / non-finite -> 100)
};

struct PortfolioBuildOptions {
  // Optional dedup-table capacity hint. Zero uses a bounded automatic reserve,
  // avoiding a multi-million-bucket hash table when a million positions repeat
  // a much smaller listed-contract universe. A hint is advisory and is clamped to
  // the position count; an over-estimate costs memory, never correctness.
  std::size_t expected_unique_contracts{0};
};

namespace detail {

// Checked-narrowing seam shared by Portfolio::create and its boundary test. The
// prepared substrate stores position-to-contract and execution indices as
// uint32_t, so no allocation may begin for an unrepresentable position count.
[[nodiscard]] constexpr bool portfolio_index_count_is_representable(std::size_t count) noexcept {
  return count <= static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
}

} // namespace detail

// ── Per-lane status ───────────────────────────────────────────────────────

enum class PriceStatus : std::uint8_t {
  Ok = 0,
  ModelUnavailable = 1, // no surface registered for the contract's uid
  NumericError = 2,     // pricer/greeks failed, or IV/price non-finite
  InvalidContract = 3,  // K <= 0 or T <= 0 (or non-finite)
};

// ── Portfolio (dedups contracts) ─────────────────────────────────────────

// A book of positions. Contracts are deduped on the exact bits of
// (uid,K,T,side) so each unique contract is priced once; each position carries a
// weight (qty*multiplier) and an index into the unique-contract table. Copying
// creates a distinct logical-book identity at revision zero; moving transfers
// the identity and revision so a retained PortfolioWorkspace follows the logical
// book rather than its address.
class Portfolio {
public:
  Portfolio(const Portfolio &other);
  Portfolio &operator=(const Portfolio &other);
  Portfolio(Portfolio &&other) noexcept;
  Portfolio &operator=(Portfolio &&other) noexcept;
  ~Portfolio() = default;

  // Build from positions (any order). An empty book is valid (yields empty
  // frames). The uint32-indexed prepared substrate limits the position count to
  // UINT32_MAX; an oversized input returns InvalidArgument before allocation.
  [[nodiscard]] static Result<Portfolio> create(std::span<const Position> positions,
                                                const PortfolioBuildOptions &options = {});

  [[nodiscard]] std::size_t n_positions() const noexcept { return positions_.size(); }
  [[nodiscard]] std::size_t n_contracts() const noexcept { return contracts_.size(); }
  [[nodiscard]] std::size_t n_underlyings() const noexcept { return uids_.size(); }

  [[nodiscard]] std::span<const Position> positions() const noexcept { return positions_; }
  [[nodiscard]] std::span<const OptionContract> contracts() const noexcept { return contracts_; }
  [[nodiscard]] std::span<const std::uint32_t> uids() const noexcept { return uids_; }

  // Update residual tenor by position while preserving the dedup/group mapping.
  // Safe when time advances a fixed set of absolute-expiry lots: positions that
  // shared a contract must receive bit-identical tenors. Validation is completed
  // before any write; on InvalidArgument (or the practically unreachable Internal
  // revision-exhaustion error), every portfolio field and pricing result remains
  // unchanged. A successful changed commit advances the logical-book revision
  // exactly once, invalidating every retained workspace substrate on its next
  // use; a bit-identical no-op preserves the revision and warmed substrate. The
  // retained inverse mapping keeps validation plus commit O(n_positions +
  // n_contracts) and allocation-free.
  [[nodiscard]] Status retime(std::span<const double> position_T);

  // The unique-contract index that position `i` references (i < n_positions()).
  [[nodiscard]] std::uint32_t contract_ix(std::size_t i) const noexcept {
    return pos_contract_ix_[i];
  }

private:
  friend class PortfolioPricer;
  friend Result<std::vector<RiskBucket>>
  reduce_risk_buckets(const PriceFrame &, const Portfolio &, RiskBucketKey, PriceTotals *);
  friend Result<std::vector<PnlRiskBucket>>
  reduce_pnl_risk_buckets(const PnlFrame &, const Portfolio &, RiskBucketKey, PnlTotals *);

  Portfolio() noexcept;

  std::vector<Position> positions_;            // input order preserved
  std::vector<OptionContract> contracts_;      // unique (uid,K,T,side)
  std::vector<std::uint32_t> pos_contract_ix_; // position -> unique-contract idx
  std::vector<std::size_t> first_position_ix_; // unique-contract idx -> first position
  std::vector<std::uint32_t> uids_;            // sorted unique uids
  std::uint64_t logical_id_{0};                // process-unique; moves with this logical book
  std::uint64_t revision_{0};                  // advances after each successful retime commit
};

// ── SurfaceRef (a borrowed OWNED-or-VIEW surface handle) ──────────────────
//
// WS-ZC1. `SurfaceSet` used to resolve a uid to a `const PricedSurface *`, so the
// replay path had to RECONSTRUCT owned `PricedSurface` objects out of the mapped
// `.atxvsa` bytes on every step even though `SurfaceArchiveV2`/`SurfaceDb` already
// serve zero-copy `PricedSurfaceView`s over those same bytes. That reconstruction
// was ~49% of replay wall-clock (`archive_map`). `SurfaceRef` is the seam that lets
// the resolver hold EITHER form.
//
// It is a two-pointer (16 B) tagged handle, NOT a virtual base: every accessor is a
// non-virtual inline that branches on which pointer is set, so the hot loop stays
// devirtualized and inlinable. Nothing in the pricer resolves a surface per CONTRACT
// — resolution is per unique-uid GROUP (`solve_span`) or per unique uid — so the
// branch is amortized over a whole batch and is perfectly predicted besides.
//
// `operator->` returns `this` (the self-proxy idiom), so existing call sites that
// were written against a raw pointer (`surf->iv(K, T)`, `surf != nullptr`) keep
// their exact syntax and only their DECLARED TYPE changes.
//
// LIFETIME. A view-backed `SurfaceRef` borrows into a memory mapping. It is only
// valid while the mapping that backs it is alive. `SurfaceSet` never owns either
// form; the owner (`MarketSnapshot`) is what keeps the archive — and therefore the
// mapping — alive for at least as long as the set. See `MarketSnapshot` in
// backtest.hpp for the enforced ordering.
class SurfaceRef {
public:
  SurfaceRef() noexcept = default;
  // Implicit on purpose: every existing `SurfaceSet::create(ptrs)` caller and every
  // `const PricedSurface *` still converts with no source change.
  SurfaceRef(const PricedSurface *owned) noexcept : owned_{owned} {}       // NOLINT
  SurfaceRef(const PricedSurfaceView *view) noexcept : view_{view} {}      // NOLINT
  SurfaceRef(std::nullptr_t) noexcept {}                                   // NOLINT
  // Reference forms so a function taking `const SurfaceRef &` still accepts a plain
  // `PricedSurface` / `PricedSurfaceView` lvalue with no call-site change. Binding a
  // temporary is rejected: the handle would outlive what it borrows.
  SurfaceRef(const PricedSurface &owned) noexcept : owned_{&owned} {}      // NOLINT
  SurfaceRef(const PricedSurfaceView &view) noexcept : view_{&view} {}     // NOLINT
  SurfaceRef(const PricedSurface &&) = delete;
  SurfaceRef(const PricedSurfaceView &&) = delete;

  [[nodiscard]] bool valid() const noexcept { return owned_ != nullptr || view_ != nullptr; }
  explicit operator bool() const noexcept { return valid(); }
  [[nodiscard]] bool operator==(std::nullptr_t) const noexcept { return !valid(); }
  [[nodiscard]] bool operator!=(std::nullptr_t) const noexcept { return valid(); }
  [[nodiscard]] bool operator==(const SurfaceRef &other) const noexcept {
    return owned_ == other.owned_ && view_ == other.view_;
  }

  // Self-proxy: `ref->method(...)` and `(*ref).method(...)` both land on SurfaceRef's
  // own forwarding methods below, so pointer-style call sites need no rewrite.
  [[nodiscard]] const SurfaceRef *operator->() const noexcept { return this; }
  [[nodiscard]] const SurfaceRef &operator*() const noexcept { return *this; }

  // True when this handle borrows a mapped record rather than owning storage.
  [[nodiscard]] bool is_view() const noexcept { return view_ != nullptr; }
  // The owned surface, or nullptr when this handle is view-backed. Only for the few
  // call sites that genuinely need `PricedSurface`-only state (e.g. `surface()`,
  // `context()`); prefer the forwarding accessors.
  [[nodiscard]] const PricedSurface *owned() const noexcept { return owned_; }
  [[nodiscard]] const PricedSurfaceView *view() const noexcept { return view_; }

  // ── Forwarding accessors (identical contract on either form) ──────────────
#define ATX_VOL_SURFACE_REF_FWD(expr) return owned_ != nullptr ? owned_->expr : view_->expr

  [[nodiscard]] const PricingContext &pricing() const noexcept {
    ATX_VOL_SURFACE_REF_FWD(pricing());
  }
  [[nodiscard]] std::uint32_t uid() const noexcept { ATX_VOL_SURFACE_REF_FWD(uid()); }
  [[nodiscard]] std::uint64_t instance_id() const noexcept {
    ATX_VOL_SURFACE_REF_FWD(instance_id());
  }
  [[nodiscard]] std::size_t n_slices() const noexcept { ATX_VOL_SURFACE_REF_FWD(n_slices()); }
  [[nodiscard]] QueryPricingTier query_pricing_tier() const noexcept {
    ATX_VOL_SURFACE_REF_FWD(query_pricing_tier());
  }
  // Prepared query-accelerator size. A borrowed view carries NO accelerator by
  // construction (it is the cold route), so its pair count is structurally 0.
  [[nodiscard]] std::size_t query_cache_pair_count() const noexcept {
    return owned_ != nullptr ? owned_->query_cache_pair_count() : std::size_t{0};
  }

  [[nodiscard]] double iv(double K, double T) const noexcept { ATX_VOL_SURFACE_REF_FWD(iv(K, T)); }
  [[nodiscard]] double total_variance(double K, double T) const noexcept {
    ATX_VOL_SURFACE_REF_FWD(total_variance(K, T));
  }
  [[nodiscard]] double forward_at(double T) const noexcept {
    ATX_VOL_SURFACE_REF_FWD(forward_at(T));
  }
  [[nodiscard]] double q_eff_at(double T) const noexcept { ATX_VOL_SURFACE_REF_FWD(q_eff_at(T)); }
  [[nodiscard]] double rate_at(double T) const noexcept { ATX_VOL_SURFACE_REF_FWD(rate_at(T)); }

  [[nodiscard]] PricedSurface::ResolvedSurfacePoint resolve(double K, double T) const noexcept {
    ATX_VOL_SURFACE_REF_FWD(resolve(K, T));
  }

  [[nodiscard]] Result<double>
  fair_value(double K, double T, Side side,
             QueryExecution execution = QueryExecution::Configured) const {
    ATX_VOL_SURFACE_REF_FWD(fair_value(K, T, side, execution));
  }
  [[nodiscard]] Result<AmericanGreeks>
  greeks(double K, double T, Side side,
         QueryExecution execution = QueryExecution::Configured) const {
    ATX_VOL_SURFACE_REF_FWD(greeks(K, T, side, execution));
  }
  [[nodiscard]] Result<AmericanGreeks>
  greeks_analytic(double K, double T, Side side,
                  QueryExecution execution = QueryExecution::Configured,
                  GreekNeeds needs = {}) const {
    ATX_VOL_SURFACE_REF_FWD(greeks_analytic(K, T, side, execution, needs));
  }
  [[nodiscard]] Result<double> delta(double K, double T, Side side,
                                     QueryExecution execution = QueryExecution::Configured) const {
    ATX_VOL_SURFACE_REF_FWD(delta(K, T, side, execution));
  }
  [[nodiscard]] Result<double> vega(double K, double T, Side side,
                                    QueryExecution execution = QueryExecution::Configured) const {
    ATX_VOL_SURFACE_REF_FWD(vega(K, T, side, execution));
  }
  [[nodiscard]] Result<FullGreekSeed>
  full_greek_seed(double K, double T, Side side, bool analytic,
                  QueryExecution execution = QueryExecution::Configured) const {
    ATX_VOL_SURFACE_REF_FWD(full_greek_seed(K, T, side, analytic, execution));
  }
  [[nodiscard]] PricedSurface::FusedResult
  evaluate(double K, double T, Side side, PricedSurface::EvalField fields, bool analytic,
           QueryExecution execution = QueryExecution::Configured, GreekNeeds needs = {}) const {
    ATX_VOL_SURFACE_REF_FWD(evaluate(K, T, side, fields, analytic, execution, needs));
  }
  // The hot batch seam. Both forms route the SAME laned analytic-Greek kernels
  // (src/laned_greek_run.hpp, WS-P1v), so borrowing costs no pricing performance and
  // is bit-identical to the owned form.
  [[nodiscard]] Status evaluate_batch(std::span<const double> K, std::span<const double> T,
                                      std::span<const Side> side, PricedSurface::EvalField fields,
                                      bool analytic, PricedSurface::EvaluationSoA out,
                                      simd::SimdIsa resolved_price_isa = simd::SimdIsa::Auto,
                                      QueryExecution execution = QueryExecution::Configured,
                                      GreekNeeds needs = {}) const {
    ATX_VOL_SURFACE_REF_FWD(
        evaluate_batch(K, T, side, fields, analytic, out, resolved_price_isa, execution, needs));
  }

#undef ATX_VOL_SURFACE_REF_FWD

private:
  const PricedSurface *owned_{nullptr};
  const PricedSurfaceView *view_{nullptr};
};

[[nodiscard]] inline bool operator==(std::nullptr_t, const SurfaceRef &ref) noexcept {
  return !ref.valid();
}
[[nodiscard]] inline bool operator!=(std::nullptr_t, const SurfaceRef &ref) noexcept {
  return ref.valid();
}

// ── SurfaceSet (uid -> surface, non-owning) ──────────────────────────────

class SurfaceSet {
public:
  SurfaceSet(const SurfaceSet &) = default;
  SurfaceSet &operator=(const SurfaceSet &) = default;
  SurfaceSet(SurfaceSet &&other) noexcept;
  SurfaceSet &operator=(SurfaceSet &&other) noexcept;

  // Build from a plain vector of surfaces; each surface supplies its own uid.
  // @return InvalidArgument on a null pointer or a duplicate uid.
  [[nodiscard]] static Result<SurfaceSet> create(std::span<const PricedSurface *const> surfaces);

  // WS-ZC1: the zero-copy form. Identical contract, but each entry BORROWS a mapped
  // archive record. The caller must keep the backing mapping alive for at least as
  // long as this set and everything derived from it.
  [[nodiscard]] static Result<SurfaceSet>
  create_from_views(std::span<const PricedSurfaceView *const> views);

  // The surface for `uid`, or a null handle if none was registered. Compare against
  // `nullptr` exactly as before.
  [[nodiscard]] SurfaceRef find(std::uint32_t uid) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return by_uid_.size(); }

  // True when every registered entry borrows a mapped record (WS-ZC1 replay path).
  [[nodiscard]] bool borrows_views() const noexcept { return borrows_views_; }

private:
  friend class PortfolioPricer;

  SurfaceSet() noexcept;
  // One shared builder for both `create` overloads: sorts, rejects null handles and
  // duplicate uids, and stamps a fresh logical id.
  [[nodiscard]] static Result<SurfaceSet>
  create_from_refs(std::vector<std::pair<std::uint32_t, SurfaceRef>> &&entries, bool borrows_views);

  std::vector<std::pair<std::uint32_t, SurfaceRef>> by_uid_; // sorted by uid
  // Process-unique identity for exact retained-valuation provenance. Copies keep
  // the identity because they resolve the same immutable surface objects; moves
  // transfer it, empty the moved-from resolver, and refresh its identity to
  // close same-address ABA.
  std::uint64_t logical_id_{0};
  bool borrows_views_{false};
};

// ── Price field mask (which PriceFrame columns to materialize) ─────────────
//
// A bitmask over the PriceFrame column GROUPS — distinct from
// `PricedSurface::EvalField`, which selects surface OUTPUTS (iv/price/greeks).
// `Marks` materializes {id, uid, pv, price, iv, status} (37 B/position); the
// `Greeks` bit adds the eight Greek columns (+64 B/position -> 101 total).
// Under a mask WITHOUT the Greeks bit the eight Greek column vectors are left
// EMPTY (`size()==0`) — never resized-and-NaN-filled — which is the 64 B/pos
// saving `PriceOptions::prices_only` now buys.
enum class PriceFieldMask : std::uint32_t {
  None = 0,
  Marks = 1u << 0,                    // id, uid, pv, price, iv, status  (37 B/pos)
  Greeks = 1u << 1,                   // delta, gamma, vega, theta, rho, vanna, volga, charm
  FullGreeks = (1u << 0) | (1u << 1), // Marks + Greeks (101 B/pos)
};

[[nodiscard]] constexpr PriceFieldMask operator|(PriceFieldMask a, PriceFieldMask b) noexcept {
  return static_cast<PriceFieldMask>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr PriceFieldMask operator&(PriceFieldMask a, PriceFieldMask b) noexcept {
  return static_cast<PriceFieldMask>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool has_field(PriceFieldMask set, PriceFieldMask bit) noexcept {
  return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(bit)) != 0u;
}
// Per-position materialized byte width for a mask: id(8)+uid(4)+pv(8)+price(8)+
// iv(8)+status(1) = 37; the eight Greek columns add 8*8 = 64 -> 101. Drives the
// FrameBytes counter and lets a caller size a PriceFrameView's backing storage.
[[nodiscard]] constexpr std::size_t bytes_per_position(PriceFieldMask fields) noexcept {
  return has_field(fields, PriceFieldMask::Greeks) ? std::size_t{101} : std::size_t{37};
}

// ── Output frames (SoA, input order) ──────────────────────────────────────

// "No value" sentinel for a column a request did not compute (GR-F1 carry axis).
inline constexpr double kPriceColumnNaN = std::numeric_limits<double>::quiet_NaN();

// Portfolio-level column sums over the Ok lanes of a price frame.
struct PriceTotals {
  double pv{0.0};
  double delta{0.0};
  double gamma{0.0};
  double vega{0.0}; // NET (signed) position-scaled dP/dsigma, per UNIT vol
  // GROSS vega: Σ|position-scaled dP/dsigma| over the SAME Ok lanes, in the same
  // unit and the same fixed input order as `vega`.
  //
  // C-3 (pipeline-m production review). A vega-neutral book — every dispersion
  // book — drives the SIGNED sum to a cancellation residual BY CONSTRUCTION, so
  // a statistic that means to normalize a return by "the book's vega exposure"
  // must divide by THIS, never by |vega|. Carried alongside `vega` rather than
  // recovered downstream because the totals-only reduction (`price_totals`, the
  // route `book_greeks` takes) never materializes a per-lane frame to sum.
  //
  // NaN unless a Greek-bearing `reduce_price_totals` computed it — never 0.0,
  // which would read as a genuinely gross-vega-flat book (the same convention
  // `dP_dq` uses below). In particular `reduce_risk_buckets`' per-bucket totals
  // do NOT populate it yet and therefore report NaN, not a false zero.
  double abs_vega{kPriceColumnNaN};
  double theta{0.0};
  double rho{0.0};
  double vanna{0.0};
  double volga{0.0};
  double charm{0.0};
  // GR-F1 carry/borrow axis: sum of position-scaled ∂P/∂q (continuous-yield
  // sensitivity, "q-rho"). Populated ONLY when PriceOptions::carry_greeks is set
  // on the returning `price()`; otherwise NaN (never 0.0 — a 0 would read as a
  // genuinely carry-flat book). See PriceFrame::dP_dq.
  double dP_dq{kPriceColumnNaN};
  std::uint32_t n_ok{0};
};

// One row per position (input order). PV and all Greeks are position-scaled
// (qty * multiplier * per-share); `price`/`iv` are per-share model values.
struct PriceFrame {
  std::vector<std::uint64_t> id;
  std::vector<std::uint32_t> uid;
  std::vector<double> pv;
  std::vector<double> price; // per-share American mark (fair_value)
  std::vector<double> iv;    // per-share Euro-equiv IV
  std::vector<double> delta;
  std::vector<double> gamma;
  std::vector<double> vega;
  std::vector<double> theta;
  std::vector<double> rho;
  std::vector<double> vanna;
  std::vector<double> volga;
  std::vector<double> charm;
  // GR-F1: position-scaled ∂P/∂q (continuous carry/borrow sensitivity). EMPTY
  // unless PriceOptions::carry_greeks was set on `price()`; when present it is
  // sized to the position count (NaN on non-Ok lanes). Kept off the PriceFrameView
  // in-place API for now (additive on the returning path only), so no existing
  // caller-owned view or backtest frame changes shape.
  std::vector<double> dP_dq;
  std::vector<PriceStatus> status;
  PriceTotals total{};

  [[nodiscard]] std::size_t size() const noexcept { return id.size(); }

  // True when the eight Greek columns are populated (the FullGreeks mask). Under
  // the Marks mask they are left EMPTY, so a marks-only caller must gate any
  // Greek-column read on this. (Vacuously true for an empty frame — no rows.)
  [[nodiscard]] bool greeks_materialized() const noexcept { return delta.size() == id.size(); }

  // True when the GR-F1 carry column is populated (carry_greeks was requested).
  [[nodiscard]] bool carry_materialized() const noexcept { return dP_dq.size() == id.size(); }

private:
  friend class PortfolioPricer;
  friend Result<std::vector<RiskBucket>>
  reduce_risk_buckets(const PriceFrame &, const Portfolio &, RiskBucketKey, PriceTotals *);

  // Opaque provenance for the logical Portfolio generation that produced this
  // frame. Copies/moves of the frame preserve it; only PortfolioPricer stamps it.
  // The pair prevents an equal-sized, reordered, unrelated, or stale pre-retime
  // book from being used to attribute the frame's risk.
  std::uint64_t book_logical_id_{0};
  std::uint64_t book_revision_{0};
};

// ── Bucketed risk (GR-F1) — per-underlier / per-expiry aggregation ─────────
//
// The canonical PriceTotals is a single whole-book bucket. Desks need risk sliced
// per underlier and per expiry. `reduce_risk_buckets` is a deterministic, serial
// post-process over an Ok-lane price frame: no hot-path cost for callers that do
// not ask, and — because the source frame is bit-identical across thread counts —
// the buckets are thread-count invariant too.
enum class RiskBucketKey : std::uint8_t {
  ByUnderlier, // key = contract uid
  ByExpiry,    // key = the contract T (year-fraction), see RiskBucket::T
};

// One aggregation bucket: the per-key PriceTotals column sums over that bucket's
// Ok lanes, accumulated in input order.
struct RiskBucket {
  std::uint64_t key{0}; // ByUnderlier: uid; ByExpiry: raw bits of T (monotone for T>0)
  double T{0.0};        // ByExpiry: the expiry year-fraction (0 for ByUnderlier)
  PriceTotals totals{};
};

// Reduce `frame`'s Ok lanes into per-key buckets, returned sorted ascending by
// `key`; within each bucket, lanes accumulate in input order. `pf` supplies the
// per-position contract (uid / T) and must be the SAME logical book generation
// `frame` was priced from. A size, provenance, or frame-shape mismatch returns
// InvalidArgument without modifying `grand`. `grand` (if non-null) receives the
// sum of the bucket subtotals in that
// sorted order, so `sum_k bucket[k] == *grand` holds BIT-EXACTLY by construction
// (it is the bucket-consistent whole-book total; it agrees with PriceFrame::total
// to floating-point rounding — the two use different summation associations).
// pv and n_ok are always summed; the eight Greek columns only when the frame
// materialized them; dP_dq only when the carry column is present.
[[nodiscard]] Result<std::vector<RiskBucket>>
reduce_risk_buckets(const PriceFrame &frame, const Portfolio &pf, RiskBucketKey by,
                    PriceTotals *grand = nullptr);

// ── In-place price API: caller-owned output view + reusable workspace ──────

// A caller-owned output view for `price_into`: one span per PriceFrame column
// plus a totals sink. `price_into` writes into these spans and — given a
// reserved workspace and correctly-sized view — allocates no frame memory (see
// PortfolioWorkspace for the n_threads>1 worker-pool caveat). The eight
// Greek spans may be left EMPTY under the Marks mask; `price_into` never touches
// an empty Greek span. Every marks span (`id/uid/pv/price/iv/status`) and
// `total` must be present and sized to the position count. Every materialized
// output span and the `total` object must occupy a disjoint byte range; overlap
// is rejected before workspace mutation, pricing, counters, or output writes.
struct PriceFrameView {
  std::span<std::uint64_t> id;
  std::span<std::uint32_t> uid;
  std::span<double> pv;
  std::span<double> price;
  std::span<double> iv;
  std::span<double> delta;
  std::span<double> gamma;
  std::span<double> vega;
  std::span<double> theta;
  std::span<double> rho;
  std::span<double> vanna;
  std::span<double> volga;
  std::span<double> charm;
  std::span<PriceStatus> status;
  PriceTotals *total{nullptr};
};

// Reusable scratch for repeated pricing of a FIXED book across many surface
// snapshots. `reserve()` sizes the internal buffers once; `price_into` /
// `price_totals` then reuse the unique-result SoA, the batch-eval SoA, and a
// RETAINED PreparedPortfolio (built once, rebuilt only when the book identity
// changes — the Greek route/mask no longer forces a rebuild: the permutation,
// groups, oci, and aligned K/T/uid columns derive purely from (uid,side,T), so
// the substrate is byte-identical for Marks and FullGreeks) — performing no
// *frame* allocation on the hot path. A successful FullGreeks price also
// retains its exact per-contract risk. A following P&L call for the same
// logical SurfaceSet, every referenced PricedSurface instance, book revision,
// analytic route, and query execution reuses that bundle and solves only
// shifted IV/marks. A marks-only price or any stamp mismatch invalidates/fails
// closed to the ordinary full solve.
// (At `PriceOptions::n_threads > 1` the
// worker fan-out allocates a `std::vector<std::jthread>` plus thread stacks —
// a threading-layer cost, not a frame allocation; only `n_threads == 1` is
// fully allocation-free end to end.)
// Move-only (owns scratch); the implementation is pimpl'd so the header stays
// free of the aligned-substrate and per-contract-result types.
//
// ## CONTRACT: one workspace per concurrent call
//
// The retained-substrate cache invalidates on an exact O(1) version key: the
// Portfolio's process-unique logical-book identity plus its retime revision.
// Construction and copying create a fresh identity, moving transfers it, and a
// successful retime advances the revision only after its no-throw commit. This
// closes same-address reconstruction ABA and detects changes to every contract,
// without hashing the book or allocating on a steady-state price call.
//
// PortfolioWorkspace owns mutable scratch and is not internally synchronized:
// never share one workspace between overlapping calls. Sequential reuse across
// snapshots, successful retimes, or entirely different books is supported and
// rebuilds the retained substrate exactly when the version key changes.
class PortfolioWorkspace {
public:
  PortfolioWorkspace();
  ~PortfolioWorkspace();
  PortfolioWorkspace(PortfolioWorkspace &&) noexcept;
  PortfolioWorkspace &operator=(PortfolioWorkspace &&) noexcept;
  PortfolioWorkspace(const PortfolioWorkspace &) = delete;
  PortfolioWorkspace &operator=(const PortfolioWorkspace &) = delete;

  // Pre-size the internal scratch for a book of `n_unique` unique contracts and
  // up to `n_positions` seed candidates. Per-row output remains caller-owned.
  // Idempotent and grow-only; after this a matching `price_into`/`price_totals`
  // allocates nothing on its own buffers. The retained PreparedPortfolio is
  // still built lazily on the first call against a given book, then reused.
  void reserve(std::size_t n_unique, std::size_t n_positions);

private:
  friend class PortfolioPricer;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Portfolio-level column sums over the Ok lanes of a PnL frame.
struct PnlTotals {
  double pv_base{0.0};
  double pv_target{0.0};
  double pnl_total{0.0};
  double pnl_delta{0.0};
  double pnl_gamma{0.0};
  double pnl_vega{0.0};
  double pnl_volga{0.0};
  double pnl_vanna{0.0};
  double pnl_theta{0.0};
  double pnl_rho{0.0};
  double pnl_charm{0.0};
  double pnl_unexplained{0.0};
  std::uint32_t n_ok{0};
};

// One row per position (input order). The eight Taylor components plus
// `pnl_unexplained` sum to `pnl_total` (all position-scaled). The `d_*` columns
// are the per-share state moves the decomposition was taken over.
struct PnlFrame {
  std::vector<std::uint64_t> id;
  std::vector<std::uint32_t> uid;
  std::vector<double> pv_base;
  std::vector<double> pv_target;
  std::vector<double> pnl_total;
  std::vector<double> pnl_delta;
  std::vector<double> pnl_gamma;
  std::vector<double> pnl_vega;
  std::vector<double> pnl_volga;
  std::vector<double> pnl_vanna;
  std::vector<double> pnl_theta;
  std::vector<double> pnl_rho;
  std::vector<double> pnl_charm;
  std::vector<double> pnl_unexplained;
  std::vector<double> d_spot; // per-share state moves (base -> shifted)
  std::vector<double> d_vol;
  std::vector<double> d_time;
  std::vector<double> d_rate;
  std::vector<PriceStatus> status;
  PnlTotals total{};

  [[nodiscard]] std::size_t size() const noexcept { return id.size(); }

private:
  friend class PortfolioPricer;
  friend Result<std::vector<PnlRiskBucket>>
  reduce_pnl_risk_buckets(const PnlFrame &, const Portfolio &, RiskBucketKey, PnlTotals *);

  // Opaque logical-book generation provenance, matching PriceFrame's contract.
  // It prevents a P&L decomposition from being attributed through a different,
  // reordered, or stale post-retime Portfolio.
  std::uint64_t book_logical_id_{0};
  std::uint64_t book_revision_{0};
};

// One deterministic per-underlier/per-expiry P&L attribution bucket. Units match
// PnlTotals: every value is position-scaled cash in the portfolio currency.
struct PnlRiskBucket {
  std::uint64_t key{0}; // ByUnderlier: uid; ByExpiry: raw bits of T
  double T{0.0};        // ByExpiry: residual year-fraction; 0 for ByUnderlier
  PnlTotals totals{};
};

// Reduce the Ok lanes of a returning `pnl_explain` frame into deterministic
// per-underlier or per-expiry PnlTotals. Rows accumulate in portfolio input order
// and buckets are returned by ascending key. `pf` must be the exact logical book
// generation that produced `frame`; malformed shape/identity/provenance or an
// invalid key returns InvalidArgument without modifying `grand`. When provided,
// `grand` is summed from the sorted bucket subtotals, so it is bit-exact to their
// ordered sum (and agrees with PnlFrame::total within normal association rounding).
[[nodiscard]] Result<std::vector<PnlRiskBucket>>
reduce_pnl_risk_buckets(const PnlFrame &frame, const Portfolio &pf, RiskBucketKey by,
                        PnlTotals *grand = nullptr);

struct UnderlierDividendScheduleView {
  std::uint32_t uid{0};
  std::span<const DividendEvent> events{};
  // Must match the surface-build cash/proportional convention. The call infers
  // the remaining continuous borrow from served F for each expiry, then holds
  // that residual fixed while bumping each cash event.
  HybridDivParams hybrid{};
};

enum class DividendSensitivityStatus : std::uint8_t {
  Ok = 0,               // every portfolio lane for this uid evaluated
  Partial = 1,          // at least one lane evaluated and at least one failed
  ModelUnavailable = 2, // all relevant lanes failed because no surface was available
  NumericError = 3,     // all relevant lanes failed contract/carry/economics validation
};

// Per-event portfolio cash sensitivity. `schedule_index` preserves duplicate
// same-date events as distinct inputs; dP_dDiv is cash P&L per cash-unit dividend.
struct DividendSensitivityRow {
  std::uint32_t uid{0};
  std::uint32_t schedule_index{0};
  std::int64_t ex_date_ns{0};
  double amount{0.0};
  double dP_dDiv{0.0};
  DividendSensitivityStatus status{DividendSensitivityStatus::Ok};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint32_t n_exposed{0};
};

// Rows preserve schedule/event order. The process-local opaque provenance ids
// identify the exact logical book generation and SurfaceSet evaluated.
struct DividendSensitivityFrame {
  std::vector<DividendSensitivityRow> rows;
  std::uint64_t book_logical_id{0};
  std::uint64_t book_revision{0};
  std::uint64_t surface_logical_id{0};
};

// ── In-place P&L API: caller-owned output view ─────────────────────────────
//
// A caller-owned output view for `pnl_explain_into`: one span per PnlFrame column
// plus a totals sink. Unlike price's PriceFrameView (whose eight Greek spans may
// be empty under the Marks mask), P&L has NO field mask — all 19 columns are
// always materialized, so every span (and `total`) must be present and sized to
// the position count. With a reserved workspace and a correctly-sized view,
// `pnl_explain_into` allocates no frame memory on the hot path (the retained
// PreparedPortfolio and the P&L solve scratch in `ws` are reused across
// snapshots). (At `opts.n_threads > 1` the worker fan-out itself allocates a
// thread vector; see PortfolioWorkspace.) Every column and the `total` object
// must occupy a disjoint byte range; overlap is rejected before observable work.
struct PnlFrameView {
  std::span<std::uint64_t> id;
  std::span<std::uint32_t> uid;
  std::span<double> pv_base;
  std::span<double> pv_target;
  std::span<double> pnl_total;
  std::span<double> pnl_delta;
  std::span<double> pnl_gamma;
  std::span<double> pnl_vega;
  std::span<double> pnl_volga;
  std::span<double> pnl_vanna;
  std::span<double> pnl_theta;
  std::span<double> pnl_rho;
  std::span<double> pnl_charm;
  std::span<double> pnl_unexplained;
  std::span<double> d_spot;
  std::span<double> d_vol;
  std::span<double> d_time;
  std::span<double> d_rate;
  std::span<PriceStatus> status;
  PnlTotals *total{nullptr};
};

// Minimal caller-owned output for handing exact shifted marks to a downstream
// backtest ledger. Rows preserve portfolio input order. `price_target` is the
// raw, unweighted per-contract mark (including for zero-quantity positions), not
// PnlFrame::pv_target. `base_vega_proxy` is the already-computed prior-date raw
// vega: downstream ledgers may use it for turnover telemetry without another
// target-date solve, but never for target-date friction. Failed rows carry their
// exact P&L status and NaN numeric outputs.
struct TargetMarkView {
  std::span<std::uint64_t> id;
  std::span<double> price_target;
  std::span<PriceStatus> status;
  std::span<double> base_vega_proxy;
};

// L2 (AL-solve-wall sprint): one retained per-unique-contract base mark exported
// from the most recent FullGreeks solve, for populating a per-(contract,date)
// settlement-mark memo. `mark` is the raw per-share American mark (`fair_value`,
// bit-identical to a Marks-mask solve of the same contract — pinned by
// BacktestExec.L2MarkMemoCruxFullGreeksMarkEqualsMarksMark). `T` is the contract's
// retained residual tenor at the last solve's valuation.
struct RetainedMark {
  std::uint32_t uid{0};
  double K{0.0};
  double T{0.0};
  Side side{Side::Call};
  double mark{0.0};
  PriceStatus status{PriceStatus::ModelUnavailable};
};

// ── The pricer ────────────────────────────────────────────────────────────

struct PriceOptions {
  // Worker threads for the per-contract Greeks fan-out. 0 => hardware
  // concurrency (>= 1); clamped to the unique-contract count. Output is
  // bit-identical regardless of this value.
  unsigned n_threads{1};
  // Route Greeks through the analytic Andersen-Lake path (american_greeks_al: five
  // boundary solves instead of the FD path's seven — delta/gamma exact, theta/charm
  // from the continuation PDE). price + delta/gamma/vega/rho/vanna/volga stay
  // bit-identical to the FD path; theta/charm become the exact PDE value. Off by
  // default so PortfolioPricer::price is unchanged; the backtest enables it.
  bool analytic_greeks{false};
  // WS-P P3 A/B: route the FullGreeks risk columns through the Christianson
  // through-iterations adjoint kernel (detail::american_greeks_adjoint) instead of
  // the finite-difference bundle. evaluate_batch computes IV + American mark only
  // (Marks fields), then delta/gamma/vega/theta/rho/vanna/volga/charm come from ONE
  // taped Andersen-Lake solve + a reverse boundary tangent per unique contract —
  // delta/gamma bit-identical to the FD path (spot-independent boundary), vega/rho
  // matched to the served mark on ~83% of a realistic grid, FD fallback elsewhere.
  // The mark (PriceFrame::price / fair_value) is the same andersen_lake value the FD
  // path serves. Off by default (every existing result bit-identical). This mode is
  // COMPUTE-ONLY: it does not stage FullGreek seeds and does not publish a reusable
  // base-risk stamp, so a later pnl_totals cannot reuse adjoint base risk under an FD
  // assumption (it recomputes, fail-safe). Supersedes analytic_greeks when both set.
  bool adjoint_greeks{false};
  // GR-F1: also compute the carry/borrow axis ∂P/∂q per contract (via
  // american_carry_greeks_al, the analytic-AL tier) and surface it as the
  // returning `price()` frame's `dP_dq` column + `PriceTotals::dP_dq`. Off by
  // default (every existing frame byte-unchanged; the column stays EMPTY / the
  // total stays NaN). Currently honored on the returning `price()` path only —
  // the caller-owned `price_into`/`price_totals` views carry no dP_dq span yet.
  // Requires the FullGreeks mask (ignored under prices_only). The per-share axis
  // is position-scaled (qty*multiplier) exactly like the other frame columns.
  bool carry_greeks{false};
  // Quote-refresh mode: compute IV + American mark only (one solve per unique
  // contract) and leave risk columns NaN. Full Greeks remain the default for
  // backward compatibility; market-making quote loops should enable this and
  // run risk on its own cadence.
  //
  // Both PriceFrame's per-lane Greek columns and PriceTotals' Greek sums are NaN
  // under this mode -- never 0.0. A zero would be indistinguishable from a book
  // that is genuinely delta/vega-flat.
  bool prices_only{false};
  // Skew-adjusted (SpiderRock) delta: delta + VegaSlope * vega, with
  // sticky.ref_uprc_weight omega in [0, 1] (0 = sticky-delta: the smile
  // slides bodily with the underlying; 1 = sticky-strike: VegaSlope forced
  // to 0, i.e. exactly the raw analytic delta). Off by default -- every
  // pre-I6 price()/price_into()/price_totals() result is bit-identical.
  // Applied ONLY to the FullGreeks delta column (PriceFrame::delta /
  // PriceTotals::delta); `pnl_explain`'s Taylor coefficients are always the
  // raw American Greeks (the decomposition's coefficients must stay raw, or
  // the P&L attribution no longer sums to the true price change) and never
  // read this flag. RunConfig::price (backtest.hpp) is exactly this struct,
  // so a backtest run that sets this flag has its delta-hedge overlay
  // (backtest.cpp, unmodified) trade on the adjusted delta automatically --
  // the hedger consumes PriceFrame::delta, not PriceOptions, directly.
  bool skew_adjusted_delta{false};
  StickyParams sticky{};
  // Call-local route for resolved Marks batches. Auto preserves the measured
  // scalar shipment gate; ForceAvx2 is capability-guarded and never reads or
  // mutates the process-global ISA override. Full-Greek evaluation remains on
  // its existing scalar/analytic routes.
  simd::SimdIsa resolved_price_isa{simd::SimdIsa::Auto};
  // Per-call economics route. Configured follows each prepared surface's query
  // tier; ColdReference bypasses transient correction accelerators for marks,
  // Greeks, totals, and every P&L leg without rebuilding the surface.
  QueryExecution query_execution{QueryExecution::Configured};
  // L4 first-order tier (K4 seam): which analytic-AL boundary solves the FullGreeks
  // bundle actually needs. DEFAULT `{}` (all true) is the full 5-solve bundle —
  // BIT-IDENTICAL to the pre-L4 maskless path — so every existing price_into /
  // price_totals / backtest frame is byte-unchanged. A reduced request (e.g. a
  // hedge frame that consumes only delta, or a risk frame that consumes delta+vega)
  // narrows the solves the analytic bundle spends (full=5, {delta,vega}=3, {delta}=1).
  //
  // CORRECTNESS COUPLING (base-risk stamp, L1): a bundle computed under a reduced
  // `greek_needs` is stamped with those needs; the base-risk REUSE guard in
  // pnl_totals/pnl_explain requires the stamped needs to be full() before a P&L
  // Taylor decomposition (which reads all eight base greeks) may reuse it. A narrowed
  // base therefore NEVER silently feeds a full P&L attribution — it forces a fresh
  // full solve. So narrowing a frame that a later P&L reuses (L1) is a NET LOSS; only
  // narrow frames whose bundle no P&L reuses (see loop-stage3.md §economy).
  PricedSurface::GreekNeeds greek_needs{};
};

class PortfolioPricer {
public:
  // Take ownership of the book (dedup already paid by Portfolio::create). Reuse
  // one pricer across many surface snapshots (fixed book, moving market).
  explicit PortfolioPricer(Portfolio pf) noexcept : pf_(std::move(pf)) {}

  [[nodiscard]] const Portfolio &portfolio() const noexcept { return pf_; }
  [[nodiscard]] Status retime(std::span<const double> position_T) { return pf_.retime(position_T); }

  // Price the book against one surface per underlying. Positions whose uid has no
  // registered surface are ModelUnavailable; degenerate contracts are
  // InvalidContract/NumericError; the rest are priced. @return the frame (the
  // call itself fails only never — an empty book gives an empty frame).
  [[nodiscard]] Result<PriceFrame> price(const SurfaceSet &surfaces,
                                         const PriceOptions &opts = {}) const;

  // In-place price: solve the book against one surface per underlying and scatter
  // the result into the caller-owned spans of `out`. With a reserved `ws` and a
  // view whose marks spans (and, under a Greeks mask, greek spans) are sized to
  // the position count, this allocates no *frame* memory on the hot path — the
  // retained PreparedPortfolio and the scratch SoA in `ws` are reused across
  // snapshots. (At `opts.n_threads > 1` the worker fan-out itself allocates a
  // thread vector; see PortfolioWorkspace.)
  //
  // `fields` selects which columns to materialize: `Marks` writes only
  // {id,uid,pv,price,iv,status} (leaving the greek spans untouched — they may be
  // empty); `FullGreeks` additionally writes the eight Greek columns. The marks
  // columns and the `pv` total are bit-identical to `price()`; the reduction is
  // the same fixed-input-order sum, so `out.total` is deterministic across
  // thread counts. `seeds` is an optional unordered set of immutable
  // PricedSurface-produced FullGreeks results. Exact per-contract provenance
  // matches skip only those unique solves; missing, stale, mismatched, or
  // conflicting candidates fail closed to an ordinary solve. At most
  // n_positions() candidates are accepted as input; a larger span is rejected
  // as a whole to preserve the workspace's warm-allocation bound. Seeds are
  // ignored under a marks-only mask. @return InvalidArgument on a view span-size
  // mismatch, or the propagated substrate-build error.
  [[nodiscard]] Status price_into(const SurfaceSet &surfaces, PriceFieldMask fields,
                                  PriceFrameView out, PortfolioWorkspace &ws,
                                  const PriceOptions &opts = {}) const;

  // Seed-aware overload kept separate so the established five-parameter ABI
  // and member-function type remain available to existing callers.
  [[nodiscard]] Status price_into(const SurfaceSet &surfaces, PriceFieldMask fields,
                                  PriceFrameView out, PortfolioWorkspace &ws,
                                  const PriceOptions &opts,
                                  std::span<const FullGreekSeed> seeds) const;

  // Totals only: solve the book and reduce weight*result over positions in fixed
  // input order, with NO per-row frame allocation and NO scatter. Bit-identical
  // to `price(...).total` for the matching mask (both masks). The win for a
  // totals-only caller is that no 101 B/pos (or 37 B/pos) frame is materialized
  // at all. Shares the retained PreparedPortfolio / scratch in `ws`.
  [[nodiscard]] Result<PriceTotals> price_totals(const SurfaceSet &surfaces, PriceFieldMask fields,
                                                 PortfolioWorkspace &ws,
                                                 const PriceOptions &opts = {}) const;

  // Taylor PnL-explain between a base and a shifted surface per underlying. The
  // time-roll dt is taken from the two surfaces' valuation timestamps; when they
  // match (dt=0) the theta/charm terms vanish (pure vol/spot/rate explain).
  [[nodiscard]] Result<PnlFrame> pnl_explain(const SurfaceSet &base, const SurfaceSet &shifted,
                                             const PriceOptions &opts = {}) const;

  // In-place Taylor PnL-explain: reprice the book against a base + shifted surface
  // per underlying and scatter the full 19-column decomposition into the
  // caller-owned spans of `out`. With a reserved `ws` and a view whose 19 spans
  // (and `total`) are all sized to the position count, this allocates no *frame*
  // memory on the hot path — the retained PreparedPortfolio and the P&L solve
  // scratch in `ws` are reused across snapshots. (At `opts.n_threads > 1` the
  // worker fan-out itself allocates a thread vector; see PortfolioWorkspace.)
  //
  // Every column and the `total` fields are bit-identical to `pnl_explain()`; the
  // reduction is the same fixed-input-order sum, so `out.total` is deterministic
  // across thread counts. @return InvalidArgument on a view span-size mismatch, or
  // the propagated substrate-build error.
  [[nodiscard]] Status pnl_explain_into(const SurfaceSet &base, const SurfaceSet &shifted,
                                        PnlFrameView out, PortfolioWorkspace &ws,
                                        const PriceOptions &opts = {}) const;

  // Totals only: solve the book against the base + shifted surfaces and reduce the
  // weighted per-row P&L decomposition over positions in fixed input order, with NO
  // per-row frame allocation and NO scatter. Bit-identical to `pnl_explain(...).total`.
  // Shares the retained PreparedPortfolio / scratch in `ws`.
  [[nodiscard]] Result<PnlTotals> pnl_totals(const SurfaceSet &base, const SurfaceSet &shifted,
                                             PortfolioWorkspace &ws,
                                             const PriceOptions &opts = {}) const;

  // Per-discrete-dividend portfolio sensitivities. This obtains this pricer's
  // exact position-scaled dP/dq column, then composes each event Jacobian from
  // the surface's served {F,r,now} and the supplied cash/proportional schedule.
  // The residual continuous borrow is implied per expiry from served F and held
  // fixed in the dividend bump. Rows/sums are deterministic across thread counts.
  //
  // Schedule uids must be unique; amounts and hybrid parameters must be finite,
  // with blend in [0,1]. Input errors return InvalidArgument. Per-lane model and
  // numeric failures are excluded and disclosed by row status/counts.
  [[nodiscard]] Result<DividendSensitivityFrame>
  dividend_sensitivities(const SurfaceSet &surfaces,
                         std::span<const UnderlierDividendScheduleView> schedules,
                         const PriceOptions &opts = {}) const;

  // Totals plus exact per-position shifted marks from the SAME unique-contract
  // solve. The established pnl_totals API and symbol remain unchanged; this
  // named caller-owned variant performs no second surface evaluation. Every
  // output span must equal n_positions() and all four mutable byte ranges must
  // be pairwise disjoint. A size mismatch or overlap returns InvalidArgument
  // before solving into the workspace or mutating any output element. Reserved
  // workspace + caller storage is allocation-free on the single-threaded hot
  // path.
  [[nodiscard]] Result<PnlTotals>
  pnl_totals_with_target_marks_into(const SurfaceSet &base, const SurfaceSet &shifted,
                                    TargetMarkView out, PortfolioWorkspace &ws,
                                    const PriceOptions &opts = {}) const;

  // Opt-in target-risk handoff. The P&L target leg is evaluated as FullGreeks,
  // and every successful unique-contract lane is exported as an immutable seed
  // whose T is the exact rolled tenor passed to that evaluation. A failed target
  // Greek lane is repriced through the established Marks route so P&L/marks stay
  // available, but it exports no seed. `target_seeds` is caller-owned, cleared
  // after validation, and filled in unique-contract order; reserve
  // n_contracts() entries alongside `ws.reserve(...)` for an allocation-stable
  // warmed path. Only the non-adjoint, full-GreekNeeds route is accepted.
  [[nodiscard]] Result<PnlTotals>
  pnl_totals_with_target_marks_and_full_greek_seeds_into(
      const SurfaceSet &base, const SurfaceSet &shifted, TargetMarkView out,
      std::vector<FullGreekSeed> &target_seeds, PortfolioWorkspace &ws,
      const PriceOptions &opts = {}) const;

  // L1 (AL-solve-wall sprint, fewer-solves): carry a retained base-risk bundle
  // across a book MEMBERSHIP SHRINK. When THIS pricer's book is a subset of `prev`'s
  // book (every unique (uid,K,T,side) of THIS book present in `prev`, at bit-exact
  // identity), each surviving unique's retained per-contract risk row (in `ws`) is
  // remapped into THIS book's contract order and the workspace's base-risk stamp is
  // re-homed to THIS book — so a following `pnl_totals`/`pnl_explain` for the SAME
  // base surface reuses the survivors' base bundle instead of re-solving it.
  //
  // Bit-identical BY CONSTRUCTION: the retained row is the SAME per-(uid,K,T,side)
  // solve the fresh path would produce (each unique's American solve is independent
  // of the book's composition — the exact per-lane invariance the no-churn-day reuse
  // and thread-count invariance already rely on), so removing an UNRELATED contract
  // cannot change a survivor's row. Precedent: QuantLib's `LazyObject` dirty-bit —
  // a cached result is invalidated by the delta that actually changed it, never by
  // wholesale recreation of the object.
  //
  // Fails CLOSED: returns false (and leaves the stamp NOT reusable for THIS book, so
  // the caller's next solve recomputes) on any identity gap — a superset/added
  // unique, a changed (uid,K,T,side), a stamp that does not correspond to `prev`, or
  // no live stamp. It NEVER weakens the `pnl_*` reuse guard: the base-surface
  // instance/identity checks there still independently re-validate every reuse, so a
  // stale carry can only ever fall back to a fresh solve, never serve wrong risk.
  [[nodiscard]] bool carry_base_risk_subset(const PortfolioPricer &prev,
                                            PortfolioWorkspace &ws) const;

  // L2 (AL-solve-wall sprint): export each unique contract's retained base mark
  // (from the most recent FullGreeks `price_into`/`price_totals` on `ws`) into
  // `out` — cleared then filled, one row per unique contract in contract order.
  // `out` is left EMPTY when `ws` holds no matching retained bundle (size gate),
  // so a caller memo fails closed. Reads only; no solve. Used by the backtest to
  // populate a per-(contract,date) settlement-mark memo without a second pass.
  void retained_marks(const PortfolioWorkspace &ws, std::vector<RetainedMark> &out) const;

private:
  [[nodiscard]] Result<PnlTotals>
  pnl_totals_impl(const SurfaceSet &base, const SurfaceSet &shifted, PortfolioWorkspace &ws,
                  const PriceOptions &opts, bool target_full_greeks) const;

  Portfolio pf_;
  // H4 (WS-H): retained workspace for the RETURNING convenience API (price() /
  // pnl_explain()). Without it each returning call built a one-shot local
  // PortfolioWorkspace, so ensure_prepared re-ran PreparedPortfolio::create (a
  // stable_sort + tile partition over the uniques) and re-resized the scratch SoA
  // EVERY call, silently losing the cross-snapshot reuse the _into variants keep.
  // Reused across calls, ensure_prepared rebuilds only on an actual book change.
  // NOT for concurrent returning-API calls on the SAME pricer (the shared scratch
  // would race) — the caller-owned-workspace _into variants remain the
  // concurrent-safe / allocation-transparent path. A unique_ptr (not a value) so
  // PortfolioPricer stays trivially/noexcept-movable and a moved-from pricer stays
  // callable (the returning API lazily re-creates it on next use). mutable: the
  // returning wrappers are const but legitimately reuse this private scratch.
  mutable std::unique_ptr<PortfolioWorkspace> returning_ws_;
};

} // namespace atx::vol
