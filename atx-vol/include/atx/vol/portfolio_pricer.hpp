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
// A `PortfolioPricer` is immutable after construction; `price` / `pnl_explain`
// are const, and the `PricedSurface` inputs are concurrent-const-safe, so one
// pricer may be queried from many threads. Each call's internal fan-out owns its
// own worker locals.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/priced_surface.hpp" // PricedSurface, PricingContext
#include "atx/vol/types.hpp"          // Result, Status, Side

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
// weight (qty*multiplier) and an index into the unique-contract table. Move-only
// is unnecessary (Rule of Zero, all-value); copyable.
class Portfolio {
public:
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

  // The unique-contract index that position `i` references (i < n_positions()).
  [[nodiscard]] std::uint32_t contract_ix(std::size_t i) const noexcept {
    return pos_contract_ix_[i];
  }

private:
  Portfolio() = default;

  std::vector<Position> positions_;            // input order preserved
  std::vector<OptionContract> contracts_;      // unique (uid,K,T,side)
  std::vector<std::uint32_t> pos_contract_ix_; // position -> unique-contract idx
  std::vector<std::uint32_t> uids_;            // sorted unique uids
};

// ── SurfaceSet (uid -> PricedSurface, non-owning) ────────────────────────

class SurfaceSet {
public:
  // Build from a plain vector of surfaces; each surface supplies its own uid.
  // @return InvalidArgument on a null pointer or a duplicate uid.
  [[nodiscard]] static Result<SurfaceSet> create(std::span<const PricedSurface *const> surfaces);

  // The surface for `uid`, or nullptr if none was registered.
  [[nodiscard]] const PricedSurface *find(std::uint32_t uid) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return by_uid_.size(); }

private:
  SurfaceSet() = default;
  std::vector<std::pair<std::uint32_t, const PricedSurface *>> by_uid_; // sorted by uid
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
// *frame* allocation on the hot path. (At `PriceOptions::n_threads > 1` the
// worker fan-out allocates a `std::vector<std::jthread>` plus thread stacks —
// a threading-layer cost, not a frame allocation; only `n_threads == 1` is
// fully allocation-free end to end.)
// Move-only (owns scratch); the implementation is pimpl'd so the header stays
// free of the aligned-substrate and per-contract-result types.
//
// ## CONTRACT: one workspace per LIVE book
//
// The retained-substrate cache invalidates on (a) the ADDRESS of the owning
// `PortfolioPricer::pf_`, (b) the current unique-contract count
// (`pf.n_contracts()`), and (c) a cheap O(1) fingerprint of the book's first
// unique contract's exact bits — all allocation-free, pointer/integer-only
// checks, but NOT a full content hash. A `Portfolio`/`PortfolioPricer`
// destroyed and reconstructed at the SAME address (e.g. a loop that rebuilds a
// local `PortfolioPricer` per iteration) can still slip past this guard if the
// new book happens to match the old one's unique-contract count AND
// first-contract fingerprint while differing only in the middle of the book —
// a residual ABA hazard. Given that, treat a `PortfolioWorkspace` as bound to
// ONE logical book for its lifetime: build a fresh workspace per book (or keep
// a keyed pool) rather than looping a single reused workspace across a
// sequence of distinct books built at a recurring address.
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
};

class PortfolioPricer {
public:
  // Take ownership of the book (dedup already paid by Portfolio::create). Reuse
  // one pricer across many surface snapshots (fixed book, moving market).
  explicit PortfolioPricer(Portfolio pf) noexcept : pf_(std::move(pf)) {}

  [[nodiscard]] const Portfolio &portfolio() const noexcept { return pf_; }

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

private:
  Portfolio pf_;
};

} // namespace atx::vol
