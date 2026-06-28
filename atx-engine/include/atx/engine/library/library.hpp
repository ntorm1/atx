#pragma once

// atx::engine::library — Library: the unified S4 facade (S4-5).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  The single public entry point that COMPOSES the four S4 units behind one
//  admit path — it owns a LibraryStore (S4-1), a DedupIndex (S4-2), a
//  CorrNeighborIndex (S4-3), and a LifecycleJournal (S4-4), and orchestrates them
//  in the ONE load-bearing order. It adds NO new corr / dedup / journal logic:
//  every screen reuses the committed unit verbatim (no second implementation).
//
// ===========================================================================
//  admit() — the load-bearing order (CHEAPEST gate first; P4 semantics preserved)
// ===========================================================================
//   1. library-wide dedup (L3): dedup_.contains(canon_hash) => Duplicate (cheapest).
//   2. incremental corr-to-pool BEFORE staging: online_corr_to_pool over the
//      candidate's SimHash neighbors (the §0.3 dangling-span discipline — the
//      candidate pnl is the caller's own buffer, read before any store growth).
//   3. the P4 AlphaGate floors, in the EXACT AlphaGate::admit order + operators:
//         metrics.fitness      <  cfg.min_fitness      => RejectFitness  (WQ-aligned primary)
//         metrics.sharpe       <  cfg.min_sharpe       => RejectSharpe   (low sanity floor)
//         metrics.turnover     >  cfg.max_turnover     => RejectTurnover (ceiling)
//         metrics.holding_days <  cfg.min_holding_days => RejectTurnover (S4-2 floor; inert at 0)
//         worst_corr           >  cfg.max_pool_corr    => RejectCorrelated
//      (replicated verbatim so admit_verdict_only matches AlphaGate::admit over
//      the whole pool — the o(N) corr screen replaces the gate's O(N) scan).
//   4. admit: stage -> corr_.add -> dedup_.insert -> journal_.transition
//      (Candidate -> Admitted), ALL in AlphaId order (L7). A flush is triggered
//      when the memtable reaches flush_batch_.
//
//  The verdict semantics MATCH AlphaGate::admit(metrics, pnl, equivalent_pool)
//  EXACTLY, feeding the INCREMENTAL worst_corr in place of the gate's internal
//  O(N) scan. Check order matches §5.2 (extended by S4-2):
//    1. fitness      < min_fitness      => RejectFitness   (WQ-aligned primary gate)
//    2. sharpe       < min_sharpe       => RejectSharpe    (low sanity floor)
//    3. turnover     > max_turnover     => RejectTurnover  (ceiling)
//    3b.holding_days < min_holding_days => RejectTurnover  (S4-2 floor; inert at 0)
//    4. corr         > max_pool_corr    => RejectCorrelated
//  The only approximation is the corr accelerator's RECALL (which ids
//  it returns) — on the S4-3 fixtures recall is ~1.0, so the verdict matches; a
//  candidate sitting exactly on max_pool_corr where a missed neighbor flips the
//  verdict is the known recall caveat (the probe fixture avoids that knife-edge).
//
// ===========================================================================
//  Determinism / threading
// ===========================================================================
//  open() rebuilds the corr index from the store IN ALPHAID ORDER, seeded only by
//  the master seed (L7). All metadata Databases (catalog/dedup/lifecycle) are the
//  one-per-thread sqlite connections the underlying units own; Library is
//  thread-COMPATIBLE (the owning thread drives admit/flush).
//
//  SAFETY (§0.3): step 2 reads the CALLER's candidate buffer (c.pnl) — never a
//  store span across a flush. corr_.add(id, c.pnl) after stage still reads the
//  caller's buffer (add copies only the signature). No store.pnl(id) span is held
//  across a stage()/flush().

#include <optional> // lazily-sized corr index (T fixed at first admit)
#include <span>
#include <string>
#include <utility> // std::move
#include <vector>

#include "atx/core/error.hpp" // Result, Status, Ok, Err
#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // f64, u8, u32, u64, usize

#include "atx/engine/combine/cost_util.hpp" // combine::cost_adjusted_fitness (S4-1)
#include "atx/engine/combine/gate.hpp"    // AlphaGate, GateConfig, GateVerdict
#include "atx/engine/combine/metrics.hpp" // combine::AlphaMetrics
#include "atx/engine/combine/store.hpp"   // combine::AlphaId

#include "atx/engine/library/corr_index.hpp"  // CorrNeighborIndex, online_corr_to_pool
#include "atx/engine/library/dedup_index.hpp" // DedupIndex
#include "atx/engine/library/lifecycle.hpp"   // LifecycleState, LifecycleJournal
#include "atx/engine/library/manifest.hpp"    // LibraryManifest, ManifestEntry, finalize_version_id
#include "atx/engine/library/record.hpp"      // Provenance, SegmentReaderLite
#include "atx/engine/library/store.hpp"       // LibraryStore, AlphaRecordView

namespace atx::engine {
class ISignalSource; // non-owning re-eval handle (forward-declared)
} // namespace atx::engine

namespace atx::engine::library {

using combine::AlphaGate;
using combine::AlphaId;
using combine::GateConfig;

// ===========================================================================
//  AlphaCandidate — one alpha presented to admit().
//
//  SAFETY: pnl / pos_flat are NON-OWNING spans into the CALLER's buffers; they
//  must outlive the admit() call (admit reads them before staging, and stages a
//  COPY into the store). `source` may be nullptr in tests (no re-eval handle).
// ===========================================================================
struct AlphaCandidate {
  atx::u64 canon_hash;
  std::span<const atx::f64> pnl;      // realized pnl stream (length T)
  std::span<const atx::f64> pos_flat; // positions (T*N), period-major then inst-minor
  combine::AlphaMetrics metrics;
  Provenance prov;
  atx::usize as_of;                   // as-of period for the Candidate->Admitted transition
  ISignalSource *source = nullptr;    // may be null in tests
};

// ===========================================================================
//  AdmitKind / AdmitVerdict — the facade's admission outcome.
//
//  Mirrors GateVerdict's branches + a Duplicate branch (the L3 dedup gate, which
//  AlphaGate has no analog for). `id` is meaningful ONLY on Accept. Enumerator
//  order is FROZEN (stable reject-histogram index) and does NOT reflect the
//  runtime check order — see GateVerdict in combine/gate.hpp.
// ===========================================================================
enum class AdmitKind : atx::u8 {
  Accept,
  Duplicate,
  RejectSharpe,
  RejectFitness,
  RejectTurnover,
  RejectCorrelated,
  RejectPriceScale,    // R2: holdout book is a trivial 1/price (price-scale) tilt
  RejectDsrSubwindow, // R3: holdout fails intra-holdout DSR sub-window check
};

struct AdmitVerdict {
  AdmitKind kind;
  AlphaId id;
};

// ===========================================================================
//  Library — the unified S4 facade.
// ===========================================================================
class Library {
public:
  // Default flush batch: seal the memtable into a segment every kFlushBatch admits.
  static constexpr atx::usize kFlushBatch = 1024;

  /// Open (or create) the library rooted at `dir`: open the store / dedup /
  /// lifecycle in `dir`, then REBUILD the corr index from the store in AlphaId
  /// order, seeded only by master_seeds.front() (L7). `cfg` holds the gate floors;
  /// `master_seeds` are persisted into the manifest so the index rebuilds
  /// identically across runs. T (the corr-index vector length) is the store's
  /// n_periods once any alpha exists, else kDefaultT until the first admit fixes it.
  [[nodiscard]] static Library open(const std::string &dir, GateConfig cfg,
                                    std::vector<atx::u64> master_seeds) {
    return Library{dir, cfg, std::move(master_seeds)};
  }

  /// Geometry-checked admit (Task 8 cross-run accumulation guard). Returns a CLEAN
  /// error — never an abort/OOB — when `c`'s holdout pnl length does not match the
  /// period count this library already fixed on a PRIOR run (the `--library-dir`
  /// reopen footgun: same dir, different panel/OOS geometry). On a matching length
  /// (the intended same-panel-many-seeds use) it delegates to admit() VERBATIM, so
  /// the happy path is byte-for-byte identical (same staging, same digest). Callers
  /// that accumulate across runs should prefer this over admit() so a geometry
  /// mismatch surfaces as a propagated InvalidArgument instead of an ATX_ASSERT
  /// abort (debug) / out-of-bounds projection read (release).
  [[nodiscard]] atx::core::Result<AdmitVerdict> try_admit(const AlphaCandidate &c,
                                                          const AlphaGate &gate) {
    if (const atx::usize fixed = fixed_period_count(); fixed != 0U && c.pnl.size() != fixed) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "library holdout geometry mismatch: candidate pnl length " +
                                std::to_string(c.pnl.size()) +
                                " != library period count t_ " + std::to_string(fixed));
    }
    return atx::core::Ok(admit(c, gate));
  }

  /// admit `c` through the full pipeline (see the header order). On Accept the
  /// returned id is the new global AlphaId; on any reject/duplicate the id is unset.
  ///
  /// PRECONDITION (period geometry): `c.pnl.size()` MUST equal the library's fixed
  /// period count once any alpha exists (the §4 shared-T contract — see ensure_corr /
  /// CorrNeighborIndex::signature). Cross-run accumulation against a DIFFERENT panel/
  /// OOS geometry violates this; callers that accumulate should gate through
  /// try_admit() (which returns a clean error) rather than relying on the asserts.
  [[nodiscard]] AdmitVerdict admit(const AlphaCandidate &c, const AlphaGate &gate) {
    // 1. library-wide dedup (cheapest gate first).
    if (dedup_.contains(c.canon_hash)) {
      return AdmitVerdict{AdmitKind::Duplicate, AlphaId{0}};
    }
    // 2. + 3. the verdict (corr-to-pool BEFORE staging, then the P4 floors).
    const AdmitKind verdict = verdict_for(c, gate);
    if (verdict != AdmitKind::Accept) {
      return AdmitVerdict{verdict, AlphaId{0}};
    }
    // 4. admit, all in AlphaId order (L7).
    ensure_corr(c.pnl.size()); // size the corr index to T on the first admit
    const auto staged = store_.stage(c.source, c.pnl, c.pos_flat, c.metrics, c.prov, c.canon_hash);
    ATX_CHECK(staged.has_value()); // shape mismatch is a programmer error; *staged below
                                   // would be UB on the error state under NDEBUG (always-on)
    const AlphaId id = *staged;
    corr_->add(id, c.pnl);                      // reads the caller's buffer (copies signature only)
    const auto ins = dedup_.insert(c.canon_hash, id);
    ATX_ASSERT(ins.has_value() && *ins);        // we already proved it was new (step 1)
    (void)ins;
    const auto tr = journal_.transition(id, LifecycleState::Admitted, c.as_of);
    ATX_ASSERT(tr.has_value());                 // Candidate->Admitted is always legal
    (void)tr;
    maybe_flush();
    return AdmitVerdict{AdmitKind::Accept, id};
  }

  /// The verdict admit() WOULD return for `c`, WITHOUT staging / mutating any
  /// state. Used by the differential test against the exact AlphaGate. Computes
  /// the dedup check + the corr-to-pool + the P4 floors (same order as admit()).
  [[nodiscard]] AdmitKind admit_verdict_only(const AlphaCandidate &c, const AlphaGate &gate) {
    if (dedup_.contains(c.canon_hash)) {
      return AdmitKind::Duplicate;
    }
    return verdict_for(c, gate);
  }

  /// Seal the live memtable into a segment (no-op if nothing is staged). Call
  /// before snapshot()/reopen to make every staged alpha durable. Resets the
  /// auto-flush batch counter on success so a following auto-flush is not biased
  /// early by admits already sealed here.
  [[nodiscard]] atx::core::Status flush_all() {
    auto st = store_.flush();
    if (st.has_value()) {
      memtable_pending_ = 0;
    }
    return st;
  }

  /// Record a lifecycle transition of `id` to `to` as of `as_of` (delegates to the
  /// journal — append-only PIT semantics, illegal edges return Err).
  [[nodiscard]] atx::core::Status mark(AlphaId id, LifecycleState to, atx::usize as_of) {
    return journal_.transition(id, to, static_cast<atx::u64>(as_of));
  }

  /// Max |corr| of `pnl` against the current pool via the SimHash neighbor scan
  /// (O(neighbors)). Empty pool (corr index unconstructed — no admits yet) => 0.0,
  /// matching the empty-pool gate convention. This is the SAME corr the admit-time
  /// verdict consults (verdict_for routes through it), exposed for the factory's
  /// PoolView seam (S4b-2): pool-aware fitness scores marginal corr against either
  /// this incremental index or the O(N) AlphaStore scan, with ONE corr impl each.
  ///
  /// RECALL CAVEAT: online_corr_to_pool returns the MAX |corr| over the candidate's
  /// SimHash NEIGHBORS, not the whole pool — it is APPROXIMATE in which ids it scans
  /// (it equals the exhaustive max only when recall == 1.0, as on the S4-3 / S4-5
  /// orthogonal equal-norm-basis fixtures). The corr reported per recalled id is the
  /// exact pairwise_complete_corr value.
  ///
  /// SAFETY: reads the CALLER's `pnl` buffer + store spans within this call only (no
  /// store growth), so nothing dangles — same discipline as verdict_for.
  [[nodiscard]] atx::f64 worst_corr_to_pool(std::span<const atx::f64> pnl) const {
    if (!corr_.has_value()) {
      return 0.0;
    }
    CorrNeighborIndex &idx =
        const_cast<CorrNeighborIndex &>(*corr_); // NOLINT: logical-const scratch (as in verdict_for)
    return online_corr_to_pool(pnl, store_, idx);
  }

  // --- R1: cumulative cross-run trial counter --------------------------------
  /// Returns the cumulative number of trials across all mine runs that have
  /// admitted into this library (loaded from the sidecar on open; 0 for a fresh
  /// library). Used by factory::mine_into to deflate DSR against the FULL
  /// multi-run N rather than just the current run's N.
  [[nodiscard]] atx::u64 cumulative_trials() const noexcept { return cumulative_trials_; }

  /// Add `n` trials to the cumulative counter (called once per mine run, after
  /// the admission loop, by factory::mine_into). NOT per-alpha admitted.
  void add_trials(atx::u64 n) noexcept { cumulative_trials_ += n; }

  // --- read passthroughs ----------------------------------------------------
  [[nodiscard]] atx::u64 n_alphas() const noexcept { return store_.n_alphas(); }
  [[nodiscard]] atx::usize n_segments() const noexcept { return store_.n_segments(); }
  [[nodiscard]] const std::string &segment_path(atx::usize i) const noexcept {
    return store_.segment_path(i);
  }
  [[nodiscard]] AlphaRecordView get(AlphaId id) const { return store_.get(id); }
  [[nodiscard]] std::span<const atx::f64> pnl(AlphaId id) const noexcept { return store_.pnl(id); }
  /// Alpha `id`'s target-weight cross-section at `period` (length n_instruments()).
  /// SAFETY: same aliasing contract as pnl() — the span ALIASES a segment Mapping
  /// (dangles when the store dies) or the live memtable (dangles on the next
  /// stage()/flush()). Copy out before the store grows. Consumed by S7-3 dead-alpha
  /// factor extraction (risk::extract_dead_factors reads dead holdings at as_of).
  [[nodiscard]] std::span<const atx::f64> positions(AlphaId id, atx::usize period) const noexcept {
    return store_.positions(id, period);
  }
  /// Shared period count of the store (0 for a fully-empty store). Exposed so a
  /// caller can bounds-check a `period` BEFORE positions(id, period): the store's
  /// pos_row does NO bounds check under NDEBUG (S7-3 dead-factor extraction guards
  /// as_of_period against this before reading dead holdings).
  [[nodiscard]] atx::usize n_periods() const noexcept { return store_.n_periods(); }
  [[nodiscard]] atx::core::Result<LifecycleState> state_as_of(AlphaId id, atx::usize t) const {
    return journal_.state_as_of(id, static_cast<atx::u64>(t));
  }
  [[nodiscard]] const std::vector<atx::u64> &master_seeds() const noexcept { return master_seeds_; }

  /// Build the content-addressed manifest of the current library. Flushes first so
  /// EVERY alpha is sealed in a segment (each entry's segment_crc is its segment's
  /// integrity_crc). Entries are emitted in ascending alpha_id order (L7); the
  /// lifecycle_at_snapshot is the current_state of each alpha; version_id is the
  /// crc32 content-address over (entries ++ master_seeds ++ cumulative_trials).
  ///
  /// R1: also writes the cumulative-trial counter sidecar (_manifest.bin) so
  /// that any subsequent Library::open from the same dir restores the counter.
  /// Sidecar write failures are silently ignored — snapshot() is [[nodiscard]]
  /// pure-compute in its callers' mental model, and a failed sidecar write does
  /// not corrupt the in-memory state; the counter is preserved across calls
  /// (worst case: a cross-process reopen starts at 0, which is the pre-R1 baseline).
  [[nodiscard]] LibraryManifest snapshot() {
    const auto st = store_.flush();
    ATX_ASSERT(st.has_value());
    (void)st;
    memtable_pending_ = 0; // snapshot sealed the memtable: keep the auto-flush counter in sync
    LibraryManifest m;
    m.master_seeds = master_seeds_;
    const std::vector<atx::u32> per_alpha_crc = segment_crc_per_alpha();
    const atx::u64 n = store_.n_alphas();
    m.entries.reserve(static_cast<atx::usize>(n));
    for (atx::u64 a = 0; a < n; ++a) {
      const AlphaId id{static_cast<atx::u32>(a)};
      const AlphaRecordView rec = store_.get(id);
      const auto state = journal_.current_state(id);
      const auto life = state.has_value() ? static_cast<atx::u8>(*state) : atx::u8{0};
      m.entries.push_back(ManifestEntry{a, rec.canon_hash, life,
                                        per_alpha_crc[static_cast<atx::usize>(a)]});
    }
    // R1: fold the cumulative trial counter into the manifest before hashing.
    m.cumulative_trials = cumulative_trials_;
    finalize_version_id(m);
    // R1: persist the counter to the sidecar so a reopened library restores it.
    (void)write_manifest(m, dir_ + "/_manifest.bin"); // best-effort; errors ignored
    return m;
  }

private:
  static constexpr atx::u32 kCorrK = 64; // SimHash hyperplanes (matches S4-3 fixtures)

  Library(const std::string &dir, GateConfig cfg, std::vector<atx::u64> master_seeds)
      : dir_{dir}, cfg_{cfg}, master_seeds_{std::move(master_seeds)}, store_{dir}, dedup_{dir},
        journal_{dir} {
    // R1: load the cumulative trial counter from the sidecar manifest if one
    // exists in `dir`. A fresh/never-snapshotted library has no sidecar -> 0,
    // which is byte-identical to the pre-R1 single-run behavior (prior == 0 =>
    // admit_fit.trial_count = res.trial_count, unchanged). NotFound and IoError
    // are normal ("no sidecar yet") and silently ignored; bad-magic (ATXMANI1
    // old sidecar) is also ignored so a library migrated in-place starts at 0
    // rather than refusing to open.
    {
      const auto maybe = read_manifest(dir + "/_manifest.bin");
      if (maybe.has_value()) {
        cumulative_trials_ = maybe->cumulative_trials;
      }
      // else: no sidecar or unreadable/corrupt — leave cumulative_trials_ at 0.
    }

    // The corr index's vector length T is fixed at construction. A reopened store
    // already knows T (n_periods()), so size + rebuild now; a fresh store defers
    // construction to the first admit (ensure_corr), which learns T from the
    // candidate pnl. Either way the index is seeded ONLY by the master seed (L7).
    if (store_.n_periods() != 0U) {
      ensure_corr(store_.n_periods());
      rebuild_corr_index();
    }
  }

  // The corr-index master seed: the first master seed (0 if none supplied).
  [[nodiscard]] static atx::u64 seed0(const std::vector<atx::u64> &seeds) noexcept {
    return seeds.empty() ? 0ULL : seeds.front();
  }

  // The library's LOAD-BEARING fixed period count, or 0 if not yet fixed. This is
  // the T that admit() will assert the candidate against: once the corr index is
  // constructed it is corr_->t(); on a reopened-but-not-yet-admitted-into store it
  // is store_.n_periods() (which the constructor uses to size the index). 0 means a
  // fresh, empty library whose T is still free to be learned from the first admit.
  // try_admit() consults this to convert the geometry-mismatch abort/OOB into a
  // clean error WITHOUT perturbing the same-T happy path.
  [[nodiscard]] atx::usize fixed_period_count() const noexcept {
    if (corr_.has_value()) {
      return corr_->t();
    }
    return store_.n_periods();
  }

  // Lazily construct the corr index sized to `t` PnL periods. Idempotent: once
  // constructed, every later candidate MUST share that T (the §4 shared-period
  // contract — asserted). Seeded only by the master seed (deterministic, L7).
  void ensure_corr(atx::usize t) {
    if (corr_.has_value()) {
      ATX_ASSERT(corr_->t() == t); // all alphas share one period count
      return;
    }
    corr_.emplace(seed0(master_seeds_), t, kCorrK);
  }

  // Rebuild the corr index from every alpha currently in the store, in AlphaId
  // order (deterministic). PRECONDITION: ensure_corr already sized the index.
  void rebuild_corr_index() {
    ATX_ASSERT(corr_.has_value());
    const atx::u64 n = store_.n_alphas();
    for (atx::u64 a = 0; a < n; ++a) {
      const AlphaId id{static_cast<atx::u32>(a)};
      corr_->add(id, store_.pnl(id)); // store span valid for this call (no growth)
    }
  }

  // The verdict (steps 2 + 3): corr-to-pool over the candidate's neighbors, then
  // the P4 AlphaGate floors in the EXACT order + operators. Reads the CALLER's
  // candidate buffer only (no store span across growth).
  [[nodiscard]] AdmitKind verdict_for(const AlphaCandidate &c, const AlphaGate &gate) const {
    // The lazy-corr optimization (P4-3): the floors are checked FIRST, so a
    // floor-rejected candidate never pays the corr cost — observationally identical
    // to the eager fixed order (the verdict of a floor-failing candidate is decided
    // before the corr gate is consulted). Match AlphaGate::admit's operators exactly.
    // Order mirrors §5.2: fitness (WQ-aligned primary) first, sharpe sanity floor second.
    const GateConfig &cfg = gate.cfg;
    // S4-1: mirror AlphaGate::admit — use cost-adjusted fitness when rt_cost_bps > 0.
    // At rt_cost_bps == 0 (default) the branch is not taken and eff_fitness ==
    // c.metrics.fitness exactly — byte-identical to the pre-S4-1 path.
    const atx::f64 eff_fitness =
        (cfg.rt_cost_bps > 0.0)
            ? combine::cost_adjusted_fitness(c.metrics.fitness, c.metrics.turnover, cfg.rt_cost_bps)
            : c.metrics.fitness;
    if (eff_fitness < cfg.min_fitness) {
      return AdmitKind::RejectFitness;
    }
    if (c.metrics.sharpe < cfg.min_sharpe) {
      return AdmitKind::RejectSharpe;
    }
    if (c.metrics.turnover > cfg.max_turnover) {
      return AdmitKind::RejectTurnover;
    }
    // S4-2: holding-period floor — mirrors AlphaGate::admit exactly (same operator <,
    // same field c.metrics.holding_days). Inert at min_holding_days=0.0 (the > 0.0
    // condition is false) => byte-identical to pre-S4-2 on the default path.
    if (cfg.min_holding_days > 0.0 && c.metrics.holding_days < cfg.min_holding_days) {
      return AdmitKind::RejectTurnover;
    }
    // The o(N) corr screen replaces AlphaGate's O(N) scan; worst_corr = MAX |corr|
    // over the SimHash neighbors (exact corr per neighbor). An empty pool (no admits
    // yet => corr_ unconstructed) has worst_corr = 0, matching AlphaGate's empty-pool
    // convention. Routed through the public worst_corr_to_pool accessor so there is
    // exactly ONE incremental-corr code path (the PoolView seam shares it, S4b-2).
    const atx::f64 worst_corr = worst_corr_to_pool(c.pnl);
    if (worst_corr > cfg.max_pool_corr) {
      return AdmitKind::RejectCorrelated;
    }
    return AdmitKind::Accept;
  }

  // Seal a segment once the memtable reaches the flush batch size.
  void maybe_flush() {
    ++memtable_pending_;
    if (memtable_pending_ >= kFlushBatch) {
      const auto st = store_.flush();
      ATX_ASSERT(st.has_value());
      (void)st;
      memtable_pending_ = 0U;
    }
  }

  // For each global AlphaId, the integrity_crc of the segment that holds it. Built
  // by attaching each sealed segment once and reading its base_alpha_id / n_alphas /
  // integrity_crc. PRECONDITION: every alpha is sealed (snapshot() flushes first).
  [[nodiscard]] std::vector<atx::u32> segment_crc_per_alpha() const {
    std::vector<atx::u32> out(static_cast<atx::usize>(store_.n_alphas()), 0U);
    for (atx::usize s = 0; s < store_.n_segments(); ++s) {
      auto reader = SegmentReaderLite::attach(store_.segment_path(s));
      ATX_CHECK(reader.has_value()); // reader->... below would be UB on a failed attach
                                     // under NDEBUG (always-on guard, not elided)
      const atx::u64 base = reader->base_alpha_id();
      const atx::u32 cnt = reader->n_alphas();
      const atx::u32 crc = reader->integrity_crc();
      for (atx::u32 i = 0; i < cnt; ++i) {
        const atx::usize g = static_cast<atx::usize>(base) + i;
        ATX_CHECK(g < out.size()); // guards the out[g] HEAP WRITE below: a catalog row whose
                                   // base+n_alphas overruns must abort, not silently corrupt
        out[g] = crc;
      }
    }
    return out;
  }

  std::string dir_;             // R1: library directory (for sidecar writes)
  GateConfig cfg_;
  std::vector<atx::u64> master_seeds_;
  LibraryStore store_;          // S4-1 (segmented append-only store)
  DedupIndex dedup_;            // S4-2 (canonical-hash dedup)
  LifecycleJournal journal_;    // S4-4 (PIT lifecycle journal)
  std::optional<CorrNeighborIndex> corr_; // S4-3 (sized to T on the first admit)
  atx::usize memtable_pending_{0};        // staged-since-last-flush count (flush batching)
  atx::u64 cumulative_trials_{0};         // R1: cross-run cumulative trial counter
};

// ===========================================================================
//  rebuild_equals — the manifest L6/L7 byte-identity verifier.
//
//  Reopens the library at `dir` (same cfg + seeds), recomputes its snapshot, and
//  asserts every recomputed segment_crc == manifest.entries[*].segment_crc AND the
//  recomputed version_id == manifest.version_id. Lives here (not in manifest.hpp)
//  because it constructs a Library (which #includes manifest.hpp).
// ===========================================================================
[[nodiscard]] inline bool rebuild_equals(const LibraryManifest &manifest, const std::string &dir,
                                         GateConfig cfg, std::vector<atx::u64> seeds) {
  Library re = Library::open(dir, cfg, std::move(seeds));
  const LibraryManifest m2 = re.snapshot();
  if (m2.version_id != manifest.version_id) {
    return false;
  }
  if (m2.entries.size() != manifest.entries.size()) {
    return false;
  }
  for (atx::usize i = 0; i < m2.entries.size(); ++i) {
    if (m2.entries[i].segment_crc != manifest.entries[i].segment_crc) {
      return false;
    }
  }
  return true;
}

} // namespace atx::engine::library
