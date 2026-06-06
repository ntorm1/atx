#pragma once

// atx::engine::combine — AlphaStore + AlphaRecord: the append-only alpha pool (P4-1).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  The append-only, deterministically-keyed pool that owns each evaluated
//  alpha's realized PnL stream (and its target-weight position stream) plus a
//  non-owning, re-evaluable source handle. It is the data substrate the gate
//  (P4-3) reads to screen candidates and the combiner (P4-4) reads to fit
//  weights. The gate loop (§1) admits a SUBSET of candidates and inserts each
//  accepted one, so incremental insert() is genuinely needed and the store owns
//  its OWN dense storage for the admitted subset (it cannot borrow a whole
//  alpha::AlphaStreams).
//
// ===========================================================================
//  §0-A: WRAP the as-built streams — do NOT recompute PnL
// ===========================================================================
//  Phase 3 already produced the dense per-alpha streams (alpha::AlphaStreams via
//  extract_streams). P4-1 CONSUMES rows that extract_streams already computed —
//  it does not re-run the backtest loop. insert() COPIES a pre-computed PnL row
//  (and per-period position cross-sections) into the store's own id-ordered
//  dense matrix; that is ingesting, not "rebuilding the matrices" (§0-A). The
//  convenience ingestor `ingest_streams` is the "collapse to one extract_streams
//  call" path: one insert() per alpha row in id order.
//
// ===========================================================================
//  Identity / determinism
// ===========================================================================
//  AlphaId is insertion order (u32), NOT a pointer — it survives store growth
//  (the backing vectors may reallocate; the id does not change). pnl_matrix()
//  exposes the streams in fixed id order so any downstream computation
//  (the combiner's sample covariance) is byte-reproducible. All PnL streams MUST
//  share the same n_periods (enforced on insert — a mismatch is an expected
//  failure returned as Err, never an abort). An alpha's pre-first-valid periods
//  are NaN and stored verbatim (the pairwise-complete policy downstream, §3.3,
//  handles them — the store never coerces NaN to 0).
//
// ===========================================================================
//  pnl_matrix() LAYOUT — alpha-major, row-major [n_alphas x n_periods]
// ===========================================================================
//  Stored as one contiguous buffer; row a (the slice [a*n_periods, +n_periods))
//  is alpha a's PnL stream of length n_periods, in insertion (id) order. This is
//  the natural layout given the AlphaStreams source (AlphaStreams::pnl(a) is a
//  contiguous per-alpha span), so insert() is a simple append/copy. The combiner
//  (P4-4) indexes element (a, t) as pnl_matrix()[a * n_periods() + t].
//
// ===========================================================================
//  Source handle ownership
// ===========================================================================
//  AlphaRecord::source is a NON-OWNING `atx::engine::ISignalSource*` (the caller
//  owns the lifetime; documented non-null in production, but a unit test that
//  does not exercise re-evaluation may pass nullptr). It is what P4-5's
//  CombinedSignalSource consumes to re-evaluate each constituent point-in-time.
//  ISignalSource is forward-declared here (its full definition lives in
//  loop/signal_source.hpp, namespace atx::engine) to keep this header light: a
//  caller that only passes records around does not transitively pull the alpha
//  VM in.

#include <span>    // std::span (non-owning stream views)
#include <utility> // std::move (Result hand-off)
#include <vector>  // std::vector (owned dense stream storage)

#include "atx/core/error.hpp" // Result, Status, Ok, Err, ErrorCode
#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // f64, u32, usize

#include "atx/engine/alpha/streams.hpp"   // alpha::AlphaStreams (ingest_streams source)
#include "atx/engine/combine/metrics.hpp" // AlphaMetrics

namespace atx::engine {
// Forward declaration: the strategy seam (full def in loop/signal_source.hpp).
// AlphaRecord::source is a NON-OWNING handle to one of these.
class ISignalSource;
} // namespace atx::engine

namespace atx::engine::combine {

// ===========================================================================
//  AlphaId — stable, insertion-ordered identity.
//
//  NOT a pointer: the value is the alpha's insertion index and never changes as
//  the store grows. Comparable so callers can use it as a map key / dedupe it.
// ===========================================================================
struct AlphaId {
  atx::u32 value;

  [[nodiscard]] friend constexpr bool operator==(AlphaId a, AlphaId b) noexcept {
    return a.value == b.value;
  }
  [[nodiscard]] friend constexpr bool operator!=(AlphaId a, AlphaId b) noexcept {
    return !(a == b);
  }
};

// ===========================================================================
//  AlphaRecord — one registered alpha's bookkeeping row.
//
//  Holds the stable id, the realized-performance summary, and the non-owning
//  re-evaluable source handle. The PnL and position streams themselves are NOT
//  stored here — they live in the store's flat, id-ordered dense matrices (so a
//  record stays small and the streams stay contiguous for the combiner). A POD
//  aggregate, copied by value (Rule of Zero).
// ===========================================================================
struct AlphaRecord {
  AlphaId id;
  AlphaMetrics metrics;
  // NON-OWNING. The caller owns the lifetime; valid for as long as the source
  // outlives the store's use of it. nullptr means "no re-eval handle" (a unit
  // test that does not exercise re-evaluation may store nullptr).
  ISignalSource *source;
};

// ===========================================================================
//  AlphaStore — the append-only, id-ordered pool of evaluated alphas.
//
//  OWNS its dense storage by value (Rule of Zero): records_ (one AlphaRecord per
//  id), pnl_ (flat [n_alphas x n_periods], alpha-major), and pos_ (flat
//  [n_alphas x n_periods x n_instruments]). insert() is the COLD path (it may
//  allocate, §3.4) and appends one alpha; it returns a stable AlphaId or Err on a
//  period-count mismatch with the established n_periods.
// ===========================================================================
class AlphaStore {
public:
  AlphaStore() = default;

  /// Append one evaluated alpha. COLD path (allocates). `source` is a NON-OWNING
  /// handle stored verbatim (may be nullptr — see AlphaRecord). `pnl` is the
  /// alpha's realized-return stream (length == n_periods()); `positions_flat` is
  /// its target-weight stream, period-major then instrument-minor, length ==
  /// n_periods() * n_instruments(). The FIRST insert fixes n_periods() and
  /// n_instruments() for the store; every later insert MUST match n_periods()
  /// (the §4 shared-period contract) — a mismatch returns Err(InvalidArgument)
  /// and leaves the store UNCHANGED. NaN cells are stored verbatim (never
  /// coerced to 0). Returns the new alpha's stable AlphaId.
  [[nodiscard]] atx::core::Result<AlphaId> insert(ISignalSource *source,
                                                  std::span<const atx::f64> pnl,
                                                  std::span<const atx::f64> positions_flat,
                                                  AlphaMetrics metrics) {
    const atx::usize periods = pnl.size();
    if (records_.empty()) {
      n_periods_ = periods; // first insert fixes the shape
      n_instruments_ = (periods == 0) ? 0 : positions_flat.size() / max_usize(periods, 1);
    } else if (periods != n_periods_) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "AlphaStore::insert: PnL period count disagrees with the pool");
    }
    // positions_flat MUST be exactly n_periods * n_instruments (a coherent
    // per-period cross-section). A ragged stream is a programmer error.
    if (positions_flat.size() != n_periods_ * n_instruments_) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "AlphaStore::insert: positions_flat size != n_periods * n_instruments");
    }

    const auto id = AlphaId{static_cast<atx::u32>(records_.size())};
    pnl_.insert(pnl_.end(), pnl.begin(), pnl.end());
    pos_.insert(pos_.end(), positions_flat.begin(), positions_flat.end());
    records_.push_back(AlphaRecord{id, metrics, source});
    return atx::core::Ok(id);
  }

  /// Batch ingestor: copy every alpha row of `streams` into the pool in id order
  /// (the §0-A "one extract_streams call" path). `sources[a]` and `metrics[a]`
  /// pair with stream row a. COLD path (allocates). Returns Err(InvalidArgument)
  /// if `sources` / `metrics` do not match streams.n_alphas(), or propagates the
  /// first insert() Err (a period/shape mismatch). On error the store holds the
  /// rows ingested before the failure (insert is append-only — partial ingest is
  /// the caller's to discard by rebuilding).
  [[nodiscard]] atx::core::Status ingest_streams(const alpha::AlphaStreams &streams,
                                                 std::span<ISignalSource *const> sources,
                                                 std::span<const AlphaMetrics> metrics) {
    const atx::usize n = streams.n_alphas();
    if (sources.size() != n || metrics.size() != n) {
      return atx::core::Err(
          atx::core::ErrorCode::InvalidArgument,
          "AlphaStore::ingest_streams: sources/metrics size != streams.n_alphas()");
    }
    const atx::usize periods = streams.n_periods();
    const atx::usize insts = streams.n_instruments();
    // Per-alpha positions are packed period-major then instrument-minor — the
    // exact positions_flat layout insert() expects. AlphaStreams stores the same
    // packing per alpha, so a single contiguous copy of the alpha's slice works.
    for (atx::usize a = 0; a < n; ++a) {
      std::vector<atx::f64> pos_flat;
      pos_flat.reserve(periods * insts);
      for (atx::usize t = 0; t < periods; ++t) {
        const std::span<const atx::f64> cs = streams.positions(a, t);
        pos_flat.insert(pos_flat.end(), cs.begin(), cs.end());
      }
      ATX_TRY_VOID(insert(sources[a], streams.pnl(a), pos_flat, metrics[a]));
    }
    return atx::core::Ok();
  }

  /// Number of alphas in the pool (== next AlphaId value).
  [[nodiscard]] atx::usize size() const noexcept { return records_.size(); }

  /// Number of alphas in the pool (alias of size(), for matrix-shape clarity).
  [[nodiscard]] atx::usize n_alphas() const noexcept { return records_.size(); }

  /// The shared PnL period count (0 until the first insert).
  [[nodiscard]] atx::usize n_periods() const noexcept { return n_periods_; }

  /// The shared position cross-section width (0 until the first insert).
  [[nodiscard]] atx::usize n_instruments() const noexcept { return n_instruments_; }

  /// The bookkeeping row for `id`. PRECONDITION: id.value < size() (ABORTS in
  /// debug — an out-of-range id is a programmer error, not a runtime condition).
  [[nodiscard]] const AlphaRecord &get(AlphaId id) const noexcept {
    ATX_ASSERT(id.value < records_.size());
    return records_[id.value];
  }

  /// The flat, alpha-major PnL matrix [n_alphas * n_periods]: element (a, t) is
  /// at index a * n_periods() + t (see the header LAYOUT note). Empty for an
  /// empty store.
  [[nodiscard]] std::span<const atx::f64> pnl_matrix() const noexcept {
    return std::span<const atx::f64>{pnl_};
  }

  /// Alpha `id`'s PnL stream (length == n_periods()): the matrix row
  /// [id*n_periods, +n_periods). PRECONDITION: id.value < size() (ABORTS in
  /// debug).
  [[nodiscard]] std::span<const atx::f64> pnl(AlphaId id) const noexcept {
    ATX_ASSERT(id.value < records_.size());
    // SAFETY: id.value < n_alphas (asserted) and pnl_ holds exactly
    //         n_alphas * n_periods_ cells, so [id*n_periods_, +n_periods_) lies
    //         wholly inside the allocation.
    const atx::usize off = static_cast<atx::usize>(id.value) * n_periods_;
    return std::span<const atx::f64>{pnl_.data() + off, n_periods_};
  }

  /// Alpha `id`'s target-weight cross-section at `period` (length ==
  /// n_instruments()). PRECONDITION: id.value < size() and period < n_periods()
  /// (ABORTS in debug).
  [[nodiscard]] std::span<const atx::f64> positions(AlphaId id, atx::usize period) const noexcept {
    ATX_ASSERT(id.value < records_.size());
    ATX_ASSERT(period < n_periods_);
    // SAFETY: both indices asserted in range; pos_ holds exactly
    //         n_alphas * n_periods_ * n_instruments_ cells, so the computed
    //         offset + n_instruments_ stays inside the allocation.
    const atx::usize off =
        (static_cast<atx::usize>(id.value) * n_periods_ + period) * n_instruments_;
    return std::span<const atx::f64>{pos_.data() + off, n_instruments_};
  }

private:
  // max(a, b) for usize — a tiny constexpr helper to avoid a 0-period divide in
  // the first-insert n_instruments inference (periods==0 short-circuits before).
  [[nodiscard]] static constexpr atx::usize max_usize(atx::usize a, atx::usize b) noexcept {
    return a > b ? a : b;
  }

  std::vector<AlphaRecord> records_; // one row per AlphaId, insertion order
  std::vector<atx::f64> pnl_;        // flat [n_alphas * n_periods_], alpha-major
  std::vector<atx::f64> pos_;        // flat [n_alphas * n_periods_ * n_instruments_]
  atx::usize n_periods_{0};          // shared period count (fixed by first insert)
  atx::usize n_instruments_{0};      // shared cross-section width (fixed by first insert)
};

} // namespace atx::engine::combine
