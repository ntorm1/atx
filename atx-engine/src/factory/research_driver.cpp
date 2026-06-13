#include "atx/engine/factory/research_driver.hpp"

#include <cstddef> // std::size_t (hash_combine seed type)

#include "atx/core/hash.hpp" // atx::core::hash_combine (engine digest fold)

#include "atx/engine/library/manifest.hpp" // library::ManifestEntry

namespace atx::engine::factory {

[[nodiscard]] ResearchReport ResearchDriver::run(const ResearchConfig &cfg) {
  ResearchReport rep;
  rep.seed = cfg.master_seed;

  // Engine digest accumulator seeded from the engine seed (the SAME hash_combine
  // the Factory digest uses, cast to u64). Each run's FactoryReport.digest is
  // folded in below in run order (F1).
  atx::u64 digest_acc = static_cast<atx::u64>(
      atx::core::hash_combine(std::size_t{0}, cfg.master_seed));

  // One Factory over the FIXED panel — reused across every run (it carries no
  // per-run state; a fresh seeded SearchDriver is built inside each mine_into).
  Factory factory{dsl_, panel_, sim_, policy_};

  atx::usize dry = 0; // consecutive zero-admit runs (the patience counter)
  for (atx::usize run = 0; run < cfg.max_runs && (cfg.patience == 0U || dry < cfg.patience);
       ++run) {
    // Overwrite the per-run search seed with the (master_seed, run) derivation.
    FactoryConfig run_cfg = cfg.per_run;
    run_cfg.search.master_seed = detail::seed_for_run(cfg.master_seed, run);

    const FactoryReport fr = factory.mine_into(run_cfg, lib_, gate_);

    rep.runs = run + 1U;
    rep.total_mined += fr.evaluated;
    rep.total_admitted += fr.admitted;
    rep.total_duplicates += fr.duplicates;

    // Fold this run's deterministic mine+admit fingerprint into the engine digest
    // (in run order, so a reordered or differing run sequence shifts the digest).
    digest_acc = static_cast<atx::u64>(
        atx::core::hash_combine(static_cast<std::size_t>(digest_acc), fr.digest));

    // Novelty-exhaustion counter: a zero-admit run is "dry"; `patience` consecutive
    // dry runs trip the early stop. A run that admits ANYTHING resets the counter.
    dry = (fr.admitted == 0U) ? (dry + 1U) : 0U;
  }

  // Snapshot the FINAL library: flushes + seals every staged alpha and computes the
  // content-addressed version_id (the manifest entries carry the per-alpha lifecycle).
  const library::LibraryManifest m = lib_.snapshot();
  rep.manifest_version_id = m.version_id;
  rep.library_size = lib_.n_alphas();

  // Lifecycle histogram from the FINAL manifest entries (NOT a journal query). Each
  // entry's lifecycle_at_snapshot is a LifecycleState 0..5; bounds-guard the cast so
  // an out-of-range value (impossible for valid enumerators) can never index OOB.
  for (const library::ManifestEntry &e : m.entries) {
    const atx::usize bucket = static_cast<atx::usize>(e.lifecycle_at_snapshot);
    if (bucket < rep.lifecycle_histogram.size()) {
      ++rep.lifecycle_histogram[bucket];
    }
  }

  // dedup_pct: the share of gate-passing "would-be-admits" the library recognized as
  // already-known (Duplicate). Denominator is (admitted + duplicates) — gate/deflation
  // rejects are excluded (they are not dedup events). max(1, .) guards an all-zero run.
  const atx::usize denom = rep.total_admitted + rep.total_duplicates;
  rep.dedup_pct = (denom == 0U)
                      ? 0.0
                      : static_cast<atx::f64>(rep.total_duplicates) /
                            static_cast<atx::f64>(denom);

  rep.digest = digest_acc;
  return rep;
}

} // namespace atx::engine::factory
