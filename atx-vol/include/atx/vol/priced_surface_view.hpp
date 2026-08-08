#pragma once

// PricedSurfaceView — a ZERO-COPY, read-only view over one ATXVSA2 surface
// record mapped in place (WS-S / S2 of the backtest hot-path throughput sprint).
//
// Where `PricedSurface` OWNS its fitted curves (polymorphic `unique_ptr` slices)
// and is produced by a `reconstruct` (per-surface whole-blob
// CRC-32C + `make_unique`/vector-copy per slice), a `PricedSurfaceView` answers
// the SAME queries — resolve / fair_value / greeks / delta / vega / evaluate /
// evaluate_batch — directly over the mapped columnar bytes, with:
//
//   * NO per-open CRC (integrity is validate-on-demand — see SurfaceArchiveV2);
//   * ZERO per-surface allocation for parametric surfaces (Essvi/Svi/C8 params
//     and LinearVariance node arrays are read IN PLACE from the mapping);
//   * lazy, once-per-queried-slice materialization of the two array/derived-state
//     curve kinds (ConvexDense, SplineVol) whose evaluators need cached derived
//     state (wing anchors / natural-spline 2nd-derivatives) — reused verbatim so
//     the view is BIT-EXACT to the reconstruct path (§4 of docs/atxvsa2-format.md).
//
// The view reproduces `PricedSurface`'s COLD served path
// (`QueryPricingTier::LegacyCompatible` / `QueryExecution::ColdReference`): it
// carries no `QueryAccelerator`, which is the default and only bit-reproducible
// backtest route. The surface-level math (`interp_forward`, `bracket`, the
// linear-in-total-variance surface interpolation, `resolve`) is replicated
// bit-for-bit from `priced_surface.cpp` / `vol_curve.cpp`; price/greeks call the
// identical `american_*` free functions. The economic-correctness gate is
// std::bit_cast-equality vs a `PricedSurface` built from the same source.
//
// ## Lifetime / ownership
//
// A view is a BORROW of the archive's mapped bytes. It is valid only while the
// owning `SurfaceArchiveV2` (and its backing buffer / mmap) is alive; copying or
// outliving the mapping dangles. The view is move-only (it owns the materialized
// heavy curves and a never-reused `instance_id`). It is immutable after
// construction and concurrent-const-safe, exactly like `PricedSurface`.
//
// A MOVED-FROM view is left structurally EMPTY: it releases its record/column
// borrows, reports `n_slices() == 0`, and every query fails closed (`resolve`
// invalid, `iv`/`total_variance` NaN, the carry accessors 0, the `Result`
// queries InvalidArgument, `evaluate_batch` an error status per lane). It
// remains safe to destroy and to move-assign into.
//
// ## Reusing PricedSurface's query vocabulary
//
// The view intentionally reuses `PricedSurface`'s public nested contract types
// (EvalField / EvaluationSoA / FusedResult / ResolvedSurfacePoint) so the
// `PortfolioPricer` SoA plumbing (`evaluate_batch`) accepts a view with no change
// to those types when the pricer is re-pointed at views in wave 2 (B1/greeks).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "atx/vol/american.hpp"       // AmericanGreeks, AmericanMethod, AlOpts
#include "atx/vol/priced_surface.hpp" // PricedSurface (nested contract types), PricingContext
#include "atx/vol/simd/cpu.hpp"       // SimdIsa
#include "atx/vol/types.hpp"          // Result, Status, Side
#include "atx/vol/vol_curve.hpp"      // IVolCurve, VolCurveKind

namespace atx::vol {

// A fitted, zero-copy surface read view over mapped ATXVSA2 record bytes.
class PricedSurfaceView {
public:
  // Reuse PricedSurface's public query-contract vocabulary verbatim so the
  // pricer's SoA plumbing is unchanged when it accepts a view (wave 2).
  using EvalField = PricedSurface::EvalField;
  using EvaluationSoA = PricedSurface::EvaluationSoA;
  using FusedResult = PricedSurface::FusedResult;
  using ResolvedSurfacePoint = PricedSurface::ResolvedSurfacePoint;

  ~PricedSurfaceView();
  PricedSurfaceView(PricedSurfaceView &&) noexcept;
  PricedSurfaceView &operator=(PricedSurfaceView &&) noexcept;
  PricedSurfaceView(const PricedSurfaceView &) = delete;
  PricedSurfaceView &operator=(const PricedSurfaceView &) = delete;

  // Parse + validate one ATXVSA2 SurfaceRecord (`record` == the record's exact
  // byte extent) and build a view over it. `record` must remain mapped/alive for
  // the view's whole lifetime — the view borrows into it, copying out a heavy
  // slice's ConvexDense/SplineVol node arrays only when that slice is queried.
  // ParseError on any framing / bounds / alignment / kind failure. This performs
  // NO payload CRC.
  [[nodiscard]] static Result<PricedSurfaceView>
  create_over_record(std::span<const std::byte> record);

  // ── Queries (const; reproduce PricedSurface's COLD served path bit-for-bit) ──

  [[nodiscard]] ResolvedSurfacePoint resolve(double K, double T) const noexcept;

  [[nodiscard]] double iv(double K, double T) const noexcept;
  [[nodiscard]] double total_variance(double K, double T) const noexcept;

  [[nodiscard]] double forward_at(double T) const noexcept;
  [[nodiscard]] double q_eff_at(double T) const noexcept;
  [[nodiscard]] double rate_at(double T) const noexcept;

  // ── Strip-carry hoisting (Task P-1; see SurfaceStripCarry, priced_surface.hpp) ──
  // Mirrors PricedSurface::strip_carry_at / iv_with_carry exactly -- same
  // opaque-snapshot contract, same bit-identity claim (T-bracket/carry are
  // pure functions of T over this view's immutable mapped columns).
  [[nodiscard]] SurfaceStripCarry strip_carry_at(double T) const noexcept;
  [[nodiscard]] double iv_with_carry(double K, const SurfaceStripCarry &carry) const noexcept;

  [[nodiscard]] Result<double>
  fair_value(double K, double T, Side side,
             QueryExecution execution = QueryExecution::Configured) const;
  [[nodiscard]] Result<AmericanGreeks>
  greeks(double K, double T, Side side,
         QueryExecution execution = QueryExecution::Configured) const;
  // `needs` mirrors PricedSurface::greeks_analytic's K4 first-order tier so ONE
  // `SurfaceRef` call signature forwards to either type (WS-ZC1). The view is always
  // the cold analytic route, where a reduced bundle skips whole boundary solves and
  // leaves the unrequested Greeks 0 — bit-identical to PricedSurface's cold AL lane.
  [[nodiscard]] Result<AmericanGreeks>
  greeks_analytic(double K, double T, Side side,
                  QueryExecution execution = QueryExecution::Configured,
                  GreekNeeds needs = {}) const;
  [[nodiscard]] Result<double> delta(double K, double T, Side side,
                                     QueryExecution execution = QueryExecution::Configured) const;
  [[nodiscard]] Result<double> vega(double K, double T, Side side,
                                    QueryExecution execution = QueryExecution::Configured) const;

  [[nodiscard]] Result<FullGreekSeed>
  full_greek_seed(double K, double T, Side side, bool analytic,
                  QueryExecution execution = QueryExecution::Configured) const;

  [[nodiscard]] FusedResult evaluate(double K, double T, Side side, EvalField fields, bool analytic,
                                     QueryExecution execution = QueryExecution::Configured,
                                     GreekNeeds needs = {}) const;

  [[nodiscard]] Status evaluate_batch(std::span<const double> K, std::span<const double> T,
                                      std::span<const Side> side, EvalField fields, bool analytic,
                                      EvaluationSoA out,
                                      simd::SimdIsa resolved_price_isa = simd::SimdIsa::Auto,
                                      QueryExecution execution = QueryExecution::Configured,
                                      GreekNeeds needs = {}) const;

  // ── Introspection (mirrors PricedSurface) ────────────────────────────────────

  [[nodiscard]] const PricingContext &pricing() const noexcept { return pricing_; }
  [[nodiscard]] std::size_t n_slices() const noexcept { return n_slices_; }
  [[nodiscard]] std::uint32_t uid() const noexcept { return pricing_.uid; }
  [[nodiscard]] std::uint64_t instance_id() const noexcept { return instance_id_; }
  [[nodiscard]] VolCurveKind kind_at(std::size_t i) const noexcept;
  // A view carries no accelerator, so it can only ever BE a cold tier. Both cache-free
  // tiers (LegacyCompatible, ColdReference) are served identically; this reports WHICH
  // of the two the loader asked for so a borrowed snapshot is indistinguishable from an
  // owned one prepared at the same tier (WS-ZC1).
  [[nodiscard]] QueryPricingTier query_pricing_tier() const noexcept {
    return query_pricing_tier_;
  }
  // Record the cold tier this view is serving. InvalidArgument for a FAST tier: those
  // require a real QueryAccelerator, which a view cannot have — such a caller must
  // reconstruct an owned surface instead.
  [[nodiscard]] Status set_cold_query_pricing_tier(QueryPricingTier tier) noexcept;

private:
  PricedSurfaceView() = default;

  // Interpolated forward / effective carry / rate at T — bit-identical to
  // PricedSurface::interp_forward over the mapped columns.
  struct ForwardCarry {
    double forward{0.0};
    double q_eff{0.0};
    double rate{0.0};
  };
  [[nodiscard]] ForwardCarry interp_forward(double T) const noexcept;
  [[nodiscard]] double slice_rate(std::size_t index) const noexcept;

  // Local mirror of CurveSurface::Bracket (private there too). Task P-1: this
  // view's own T-bracket over col_T_, hoisted the same way SurfaceStripCarry
  // hoists PricedSurface's.
  struct Bracket {
    std::size_t lo{0};
    std::size_t hi{0};
    double upper_weight{0.0};
    [[nodiscard]] bool is_single_slice() const noexcept { return lo == hi; }
  };
  // Precondition: n_slices_ != 0 and T is finite/positive (callers guard this
  // the same way forward_at/rate_at/q_eff_at do before calling interp_forward).
  [[nodiscard]] Bracket surface_bracket(double T) const noexcept;

  // Surface-level total variance at (k_log, T) — replicates CurveSurface::w
  // (short-end scaling, single-slice long-end flat, linear-in-w bracket blend).
  [[nodiscard]] double surface_w(double k_log, double T) const noexcept;
  // Resolve a point given an already-resolved bracket (the strip-carry-hoist
  // ladder-reuse path). Bit-identical to surface_w(k_log, T) when
  // resolved == surface_bracket(T).
  [[nodiscard]] double surface_w(double k_log, double T, Bracket resolved) const noexcept;
  // Per-slice total variance w(k_log) — dispatch on the mapped kind byte.
  [[nodiscard]] double slice_w(std::size_t i, double k_log) const noexcept;
  [[nodiscard]] const IVolCurve *heavy_curve(std::size_t i) const noexcept;

  [[nodiscard]] ResolvedSurfacePoint resolve_with_carry(double K, double T,
                                                        ForwardCarry fc) const noexcept;
  [[nodiscard]] ResolvedSurfacePoint resolve_with_carry_and_bracket(double K, double T,
                                                                    ForwardCarry fc,
                                                                    Bracket bracket) const noexcept;

  [[nodiscard]] Result<double> price_resolved(const ResolvedSurfacePoint &p, Side side) const;
  [[nodiscard]] Result<AmericanGreeks> greeks_resolved(const ResolvedSurfacePoint &p, Side side,
                                                       bool analytic, GreekNeeds needs = {}) const;
  [[nodiscard]] Result<double> delta_resolved(const ResolvedSurfacePoint &p, Side side) const;
  [[nodiscard]] Result<double> vega_resolved(const ResolvedSurfacePoint &p, Side side) const;
  [[nodiscard]] FusedResult evaluate_resolved(const ResolvedSurfacePoint &p, Side side,
                                              EvalField fields, bool analytic,
                                              GreekNeeds needs = {}) const;

  // ── Borrowed columnar views into the mapped record (non-owning) ──────────────
  std::span<const std::byte> record_{};        // the whole surface record extent
  const std::uint8_t *col_kind_{nullptr};      // kind[n_slices]
  const double *col_T_{nullptr};               // T[n_slices]  (ascending maturities)
  const double *col_forward_{nullptr};         // forward[n_slices]
  const double *col_qeff_{nullptr};            // q_eff[n_slices]
  const double *col_df_{nullptr};              // df[n_slices]
  const double *col_borrow_{nullptr};          // borrow[n_slices] (fidelity; unused on hot path)
  const std::uint64_t *col_payload_off_{nullptr}; // record-relative payload offset[n_slices]
  const std::uint32_t *col_node_count_{nullptr};  // node_count[n_slices]

  std::size_t n_slices_{0};
  PricingContext pricing_{};
  bool term_rates_{false};
  QueryPricingTier query_pricing_tier_{QueryPricingTier::LegacyCompatible};

  // Lazy concrete curves for the two derived-state kinds (ConvexDense,
  // SplineVol). Empty (no heap) for parametric-only surfaces; otherwise one
  // concurrency-safe slot per slice. Each heavy curve is constructed at most
  // once, on that slice's first query.
  struct HeavyCurveSlot;
  mutable std::unique_ptr<HeavyCurveSlot[]> heavy_curve_slots_{};

  std::uint64_t instance_id_{0};
};

} // namespace atx::vol
