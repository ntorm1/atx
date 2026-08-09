#pragma once

// SweepDriver -- cache-first, variant-parallel backtest sweeps over the track
// lakehouse (Task C3, backtest-production-lakehouse sprint).
//
// ## What this is
//
// `run_sweep` turns a GRID of `BacktestStrategyTemplate` variants (one
// underlier's corpus, replayed under every variant) into a set of tracks:
//
//   1. ENUMERATE + DEDUPE -- every variant is canonicalized to a `TrackKey`
//      (D1, track_key.hpp) over (variant, `SweepSpec::base_config`,
//      `SweepSpec::data_snapshot_id`). Two variants that hash to the SAME key
//      have IDENTICAL economics -- one of them is redundant, so only the
//      FIRST occurrence (scan order over `SweepSpec::variants`) is kept for
//      execution. `SweepResult::variants` is therefore one entry per UNIQUE
//      key, in first-occurrence order -- deterministic because it is the
//      SCAN order, never the iteration order of an unordered container (the
//      dedupe membership check below uses `std::unordered_map` for O(1)
//      lookup only; its iteration order never reaches the output).
//   2. CACHE-FIRST -- each unique key is probed against the catalog (D3).
//      `TrackKey` already folds `engine_id` (which folds
//      `kBacktestEconomicsRev`, track_key.hpp) into the hash, so a probe HIT
//      can only ever be a track computed under the CURRENT economics
//      revision -- there is no separate freshness check to get wrong, and no
//      stale-revision row can ever be served as a hit. A hit skips the run
//      entirely; a miss is scheduled.
//   3. VARIANT-PARALLEL EXECUTION -- every miss runs `run_backtest` with
//      `RunConfig::price.n_threads` forced to 1 (the brief's "opts.n_threads=1
//      inner runs"): the OUTER fan-out (`SweepConfig::n_threads`, via
//      `atx::vol::parallel_for`, detail/parallel_for.hpp) is what runs
//      variants concurrently, not the inner pricer. Every run shares
//      `SweepConfig::snapshot_pool` (C2), so N variants over the SAME corpus
//      open each archive once between them -- and per invariant I1-I8, a
//      pooled run is bit-identical to a solo private-cache run of the same
//      variant regardless of how many OTHER variants raced it (pins:
//      `SweepDriverTest.SweepResultNavsMatchIndividualBaselinesUnder-
//      VariantParallelism`).
//   4. PUBLISH -- back on the calling thread (never inside the parallel
//      fan-out -- see "Threading" below), each successful miss is
//      `TrackStore::write_staging`'d then `Catalog::register_staging`'d, in
//      deterministic (first-occurrence) order.
//   5. TRIAL REGISTRATION -- `Catalog::record_trial` is called once per
//      ORIGINAL variant (every index in `SweepSpec::variants`, including
//      duplicates and cache hits), because the `trials` table counts
//      ATTEMPTS, not unique configs (B4's Deflated Sharpe N needs the real
//      multiple-testing count) -- so a sweep with 2 identical variants runs
//      the backtest ONCE but records TWO trial rows, both referencing the
//      same track_key.
//
// ## What this is NOT
//
// `run_sweep` is fail-closed on anything STRUCTURAL: `Catalog::open`
// failing, a variant that does not `validate_backtest_template`, a probe/
// write/register/record_trial error. Any of those aborts the WHOLE sweep
// (`Err`), matching the rest of this sprint's "a cache that might silently
// serve a wrong or partial answer is worse than one that refuses" posture
// (`reconcile_nav`'s fail-closed abort is the same shape, track_key.hpp).
// There is no per-variant partial-failure mode; that is a possible future
// widening, not something this task's brief asks for.
//
// This task does NOT implement economics-rev SUPERSESSION bookkeeping
// (retiring an old-revision row so both generations stay queryable) or D4
// compact+reload verification -- both are Task D5's job, which extends this
// same file. `run_sweep`'s own cache-first correctness does not need
// supersession machinery: a revision bump changes `engine_id`, which changes
// every `TrackKey` in the sweep, so a probe under the new revision can never
// hit an old-revision row in the first place (see point 2 above) --
// supersession is about GC/bookkeeping of the now-unreachable old rows, not
// about correctness of what a fresh sweep computes.
//
// ## Threading
//
// Only `run_backtest` itself runs inside the parallel fan-out (each miss
// writes its OWN disjoint result slot, mirroring the `out[t] = run_backtest
// (...)` pattern `BacktestExec.SnapshotPoolConcurrentRunsMatchSerial` already
// proves race-free). `TrackStore`/`Catalog` calls happen strictly AFTER the
// fan-out's join barrier, on the calling thread alone -- deliberately, because
// `atx::core::db::Database` (the handle `Catalog` wraps) "must NOT be shared
// across threads" (atx/core/db/sqlite.hpp) and `Catalog` itself is one
// `Database`. Serializing the publish phase sidesteps that constraint
// entirely rather than opening one `Catalog` per worker.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/backtest.hpp"             // Clock, RunConfig, BacktestResult
#include "atx/vol/backtest_template.hpp"    // BacktestStrategyTemplate
#include "atx/vol/research/catalog.hpp"     // Catalog (D3)
#include "atx/vol/research/snapshot_pool.hpp" // SnapshotPool (C2)
#include "atx/vol/research/track_key.hpp"   // TrackKey (D1)
#include "atx/vol/research/track_store.hpp" // TrackStore, TrackMeta (D2)
#include "atx/vol/types.hpp"                // Result, Status

namespace atx::vol {

// The config grid plus everything every variant in it shares: one corpus
// (`clock` over one underlier `uid`), one economics baseline (`base_config`
// -- the driver overrides only EXECUTION fields per run, see the file doc
// comment), one hive placement (`meta`), and the data identity every
// resulting `TrackKey` folds in (`data_snapshot_id`).
struct SweepSpec {
  // The config grid, in caller order. This IS the enumeration order dedupe,
  // execution and trial-recording index against; nothing here reorders it.
  std::vector<BacktestStrategyTemplate> variants;

  // The corpus every variant replays, and the underlier `uid` its projected
  // legs resolve against (`ProjectedTemplateStrategy::create`'s `uid`
  // argument). One sweep == one (clock, uid) pair; a sweep spanning multiple
  // underliers is multiple `run_sweep` calls, one per underlier -- matching
  // D2/D3's own per-track underlier/family placement below.
  Clock clock;
  std::uint32_t uid{0};

  // Shared RunConfig baseline every variant runs under. `run_sweep` copies
  // this once per run and overrides exactly `price.n_threads` (forced to 1)
  // and `snapshot_pool` (forced to `SweepConfig::snapshot_pool`) -- both
  // EXECUTION fields, excluded from `canonical_config_bytes` (track_key.hpp)
  // -- so every other field here, and every field of `variants[i]`, is what
  // actually differentiates two variants' `TrackKey`s.
  RunConfig base_config{};

  // Hive placement (D2/D3) for every track this sweep produces -- shared
  // across all variants, per the one-(clock,uid)-per-sweep scoping above.
  TrackMeta meta{};

  // `TrackKey`'s `data_snapshot_id`: SHA-256 over the sorted per-date content
  // identities of the archives `clock` actually reads (track_key.hpp).
  // Computed by the CALLER -- this header stays one-directional and does not
  // depend on BacktestDb/SurfaceDb, exactly like track_key.hpp itself.
  std::array<std::uint8_t, 32> data_snapshot_id{};
};

// Execution/lakehouse wiring for one `run_sweep` call -- as opposed to
// `SweepSpec`, which is the economics grid itself.
struct SweepConfig {
  // Non-owning; must outlive the call. Shared across every variant this
  // sweep runs, so N variants over one corpus open each archive ONCE between
  // them (SnapshotPool, C2). `nullptr` => every variant gets a private
  // per-run cache (today's default topology) -- still bit-identical, just
  // without the cross-variant archive reuse.
  SnapshotPool *snapshot_pool{nullptr};

  // TrackStore/Catalog root (D2/D3). `run_sweep` needs a working lakehouse --
  // cache-first IS the catalog probe/register cycle -- so this must name a
  // usable (creatable) directory.
  std::string lake_root;

  // `trials.sweep_id` -- identifies this sweep's attempts in the trial
  // registry (D3/B4). Must be non-empty; two `run_sweep` calls that want
  // independent trial accounting (e.g. two different sweeps that happen to
  // reuse a track) must pass different ids.
  std::string sweep_id;

  // Variant-level fan-out width for the MISSES (`atx::vol::parallel_for`,
  // detail/parallel_for.hpp). 0 => `atx_auto_worker_count()`. Every run
  // inside is forced to `price.n_threads = 1` regardless of this value (see
  // `SweepSpec::base_config` above) -- this knob controls how many variants
  // run CONCURRENTLY, not how many pricer threads each one gets.
  unsigned n_threads{0};
};

// One UNIQUE variant's outcome -- `SweepResult::variants` has exactly one of
// these per distinct `TrackKey` the sweep enumerated (see the file doc
// comment's dedupe rule).
struct SweepVariantOutcome {
  TrackKey key;
  // Index into `SweepSpec::variants` of the FIRST variant that hashed to
  // this key -- documents which original variant's economics this outcome
  // (and every duplicate collapsed into it) actually ran under.
  std::size_t first_variant_index{0};
  // The catalog already had this key registered -- no backtest ran this call.
  bool cache_hit{false};
  // This call executed a fresh backtest for this key (mutually exclusive
  // with `cache_hit`).
  bool ran{false};
  // Populated iff `ran`; a cache hit's result is not reloaded from the
  // lakehouse here (D4/D5's job) -- `run_sweep`'s own job is cache-first
  // SCHEDULING, not read-back.
  std::optional<BacktestResult> result;
};

// Outcome of one `run_sweep` call.
struct SweepResult {
  // One entry per UNIQUE `TrackKey`, in first-occurrence order over
  // `SweepSpec::variants` (see the file doc comment's dedupe rule).
  std::vector<SweepVariantOutcome> variants;
  // `spec.variants.size()` -- the pre-dedupe submission count.
  std::uint64_t n_variants_submitted{0};
  // Fresh backtests this call actually executed (== the number of misses).
  std::uint64_t engine_runs{0};
  // Unique keys that were already registered in the catalog (== the number
  // of hits).
  std::uint64_t cache_hits{0};
};

// Enumerate `spec.variants`, dedupe to unique `TrackKey`s, probe the catalog
// cache-first (hit => skip, miss => run), run every miss variant-parallel
// over `spec.clock`/`spec.uid` (each run forced to `price.n_threads = 1`,
// sharing `config.snapshot_pool`), publish each fresh track to the lakehouse
// (`TrackStore::write_staging` + `Catalog::register_staging`), and record one
// `Catalog::record_trial` row per ORIGINAL variant (attempts, not unique
// configs -- see the file doc comment).
//
// Err(InvalidArgument) if `spec.variants` is empty, any variant fails
// `validate_backtest_template`, or `config.lake_root`/`config.sweep_id` is
// empty. Fail-closed on any catalog/store operation error (aborts the whole
// sweep -- see the file doc comment's "What this is NOT").
[[nodiscard]] Result<SweepResult> run_sweep(const SweepSpec &spec, const SweepConfig &config);

} // namespace atx::vol
