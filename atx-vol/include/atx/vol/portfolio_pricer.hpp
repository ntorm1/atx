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
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/adjusted_greeks.hpp" // StickyParams
#include "atx/vol/priced_surface.hpp"  // PricedSurface, PricingContext
#include "atx/vol/query_pricing.hpp"   // QueryExecution
#include "atx/vol/types.hpp"           // Result, Status, Side

namespace atx::vol {

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
  // frames). @return InvalidArgument only on a structurally impossible input
  // (currently none — reserved for future validation).
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

  Portfolio() noexcept;

  std::vector<Position> positions_;            // input order preserved
  std::vector<OptionContract> contracts_;      // unique (uid,K,T,side)
  std::vector<std::uint32_t> pos_contract_ix_; // position -> unique-contract idx
  std::vector<std::size_t> first_position_ix_; // unique-contract idx -> first position
  std::vector<std::uint32_t> uids_;            // sorted unique uids
  std::uint64_t logical_id_{0};                // process-unique; moves with this logical book
  std::uint64_t revision_{0};                  // advances after each successful retime commit
};

// ── SurfaceSet (uid -> PricedSurface, non-owning) ────────────────────────

class SurfaceSet {
public:
  SurfaceSet(const SurfaceSet &) = default;
  SurfaceSet &operator=(const SurfaceSet &) = default;
  SurfaceSet(SurfaceSet &&other) noexcept;
  SurfaceSet &operator=(SurfaceSet &&other) noexcept;

  // Build from a plain vector of surfaces; each surface supplies its own uid.
  // @return InvalidArgument on a null pointer or a duplicate uid.
  [[nodiscard]] static Result<SurfaceSet> create(std::span<const PricedSurface *const> surfaces);

  // The surface for `uid`, or nullptr if none was registered.
  [[nodiscard]] const PricedSurface *find(std::uint32_t uid) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return by_uid_.size(); }

private:
  friend class PortfolioPricer;

  SurfaceSet() noexcept;
  std::vector<std::pair<std::uint32_t, const PricedSurface *>> by_uid_; // sorted by uid
  // Process-unique identity for exact retained-valuation provenance. Copies keep
  // the identity because they resolve the same immutable surface objects; moves
  // transfer it, empty the moved-from resolver, and refresh its identity to
  // close same-address ABA.
  std::uint64_t logical_id_{0};
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

// Portfolio-level column sums over the Ok lanes of a price frame.
struct PriceTotals {
  double pv{0.0};
  double delta{0.0};
  double gamma{0.0};
  double vega{0.0};
  double theta{0.0};
  double rho{0.0};
  double vanna{0.0};
  double volga{0.0};
  double charm{0.0};
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
  std::vector<PriceStatus> status;
  PriceTotals total{};

  [[nodiscard]] std::size_t size() const noexcept { return id.size(); }

  // True when the eight Greek columns are populated (the FullGreeks mask). Under
  // the Marks mask they are left EMPTY, so a marks-only caller must gate any
  // Greek-column read on this. (Vacuously true for an empty frame — no rows.)
  [[nodiscard]] bool greeks_materialized() const noexcept { return delta.size() == id.size(); }
};

// ── In-place price API: caller-owned output view + reusable workspace ──────

// A caller-owned output view for `price_into`: one span per PriceFrame column
// plus a totals sink. `price_into` writes into these spans and — given a
// reserved workspace and correctly-sized view — allocates no frame memory (see
// PortfolioWorkspace for the n_threads>1 worker-pool caveat). The eight
// Greek spans may be left EMPTY under the Marks mask; `price_into` never touches
// an empty Greek span. Every marks span (`id/uid/pv/price/iv/status`) and
// `total` must be present and sized to the position count.
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

  // Pre-size the internal scratch for a book of `n_unique` unique contracts
  // (the per-row output frame is caller-owned, so `n_positions` is advisory).
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
// thread vector; see PortfolioWorkspace.)
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
  // thread counts. @return InvalidArgument on a view span-size mismatch, or the
  // propagated substrate-build error.
  [[nodiscard]] Status price_into(const SurfaceSet &surfaces, PriceFieldMask fields,
                                  PriceFrameView out, PortfolioWorkspace &ws,
                                  const PriceOptions &opts = {}) const;

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

private:
  Portfolio pf_;
};

} // namespace atx::vol
