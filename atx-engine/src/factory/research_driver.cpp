#include "atx/engine/factory/research_driver.hpp"

#include <bit>     // std::bit_cast (f64 verdict sharpe -> u64 for the digest fold)
#include <cstddef> // std::size_t (hash_combine seed type)
#include <cstdint> // std::uint64_t (the folded verdict-sharpe bit pattern)
#include <span>    // std::span (the per-survivor OOS PnL slice)
#include <vector>  // std::vector (the cached regime labels)

#include "atx/core/hash.hpp" // atx::core::hash_combine (engine digest fold)

#include "atx/engine/eval/regime_slice.hpp" // eval::regime_labels, robustness_verdict (S4.5 gate)

#include "atx/engine/library/manifest.hpp" // library::ManifestEntry

namespace atx::engine::factory {

[[nodiscard]] ResearchReport ResearchDriver::run(const ResearchConfig &cfg) {
  ResearchReport rep;
  rep.seed = cfg.master_seed;
  // S4.4b robustness-gate SEAM (default-OFF). Echo the config flag into the report
  // so S4.5 (and a caller) can see the gate state; this is report-only and is NOT
  // folded into digest_acc below, so toggling the seam never shifts the engine
  // fingerprint. When false (the default), the per-run branch below is skipped
  // entirely — no compute, no RNG, no ordering change — so run() is byte-identical
  // to the pre-S4.4b path. S4.5 flips robustness_gate on and fills the live gating.
  rep.robustness_gate_active = cfg.robustness_gate;

  // Engine digest accumulator seeded from the engine seed (the SAME hash_combine
  // the Factory digest uses, cast to u64). Each run's FactoryReport.digest is
  // folded in below in run order (F1).
  atx::u64 digest_acc =
      static_cast<atx::u64>(atx::core::hash_combine(std::size_t{0}, cfg.master_seed));

  // One Factory over the FIXED panel — reused across every run (it carries no
  // per-run state; a fresh seeded SearchDriver is built inside each mine_into).
  Factory factory{dsl_, panel_, sim_, policy_};

  // S4.5 robustness gate: the vol-tercile partition of the (visible) panel — a pure
  // function of panel_ + vol_window, so it is computed ONCE for the whole engine run
  // and reused per-survivor. Empty (all-sentinel) when the gate is OFF (never read).
  const std::vector<atx::u8> regime_labels =
      cfg.robustness_gate
          ? eval::regime_labels(panel_, cfg.robustness_cfg.vol_window, eval::kNumRegimes)
          : std::vector<atx::u8>{};

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

    // S4.4b robustness-gate SEAM (default-OFF). When the gate is enabled, S4.5
    // screens THIS run's admitted survivors with a per-survivor RobustnessVerdict
    // (eval/regime_slice.hpp robustness_verdict, over the SealedPanel.visible()
    // OOS PnL) against cfg.robustness_cfg, and either report-only-records or
    // rejects the non-robust ones. S4.4b ships ONLY the guarded seam: when
    // cfg.robustness_gate is false (the default), this branch is NOT entered — no
    // compute, no RNG, no ordering change, no digest fold — so run() stays byte-
    // identical to the pre-S4.4b path. The branch body is UNREACHED by every
    // current test (none sets robustness_gate); S4.5 fills the per-survivor verdict
    // loop and exercises it. The lone reference to cfg.robustness_cfg lives inside
    // this branch, so the unused config slot never trips the OFF-path /W4 build.
    if (cfg.robustness_gate) {
      digest_acc = screen_run_robustness(fr, regime_labels, cfg.robustness_cfg, digest_acc, rep);
    }

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
                      : static_cast<atx::f64>(rep.total_duplicates) / static_cast<atx::f64>(denom);

  rep.digest = digest_acc;
  return rep;
}

[[nodiscard]] atx::u64
ResearchDriver::screen_run_robustness(const FactoryReport &fr, const std::vector<atx::u8> &labels,
                                      const eval::RobustnessConfig &robustness_cfg,
                                      atx::u64 digest_acc, ResearchReport &rep) const {
  // The survivors THIS run admitted are the half-open AlphaId range
  // [n_before, n_after) (mine_into admits in id order, so the range is exactly the
  // new admits). Screen each, in ascending id order, with a RobustnessVerdict over
  // its stored OOS PnL — which was realized over panel_ (the visible region the
  // engine mines on), so `labels` (the vol-tercile partition of that same panel)
  // lines up index-for-index with the PnL stream.
  const atx::u64 first = fr.library_n_alphas_before;
  const atx::u64 last = fr.library_n_alphas_after;
  for (atx::u64 a = first; a < last; ++a) {
    const library::AlphaId id{static_cast<atx::u32>(a)};
    // SAFETY: lib_.pnl(id) ALIASES a segment Mapping / the live memtable and dangles
    // on the next stage()/flush(); we only READ it within this iteration (no store
    // growth here), and the span length == panel_.dates() == labels.size() because
    // the PnL was extracted over panel_. A label/PnL length disagreement would make
    // robustness_verdict's debug precondition fire — guard it so a degenerate
    // (e.g. label-less) panel screens as not-robust instead of tripping the assert.
    const std::span<const atx::f64> pnl = lib_.pnl(id);
    ++rep.robust_screened;
    bool is_robust = false;
    if (pnl.size() == labels.size() && !labels.empty()) {
      const eval::RobustnessVerdict v =
          eval::robustness_verdict(pnl, std::span<const atx::u8>{labels}, robustness_cfg);
      is_robust = v.is_robust;
      if (is_robust) {
        ++rep.robust_passed;
      }
      // Fold the verdict's binding constraints into the engine fingerprint (ascending
      // id order). This is what makes the gate-ON digest S4.5's own; the gate-OFF path
      // never reaches here, so its digest is byte-identical to the pre-S4.5 engine.
      digest_acc = static_cast<atx::u64>(atx::core::hash_combine(
          static_cast<std::size_t>(digest_acc), std::bit_cast<std::uint64_t>(v.worst_regime_sharpe),
          std::bit_cast<std::uint64_t>(v.worst_window_sharpe), static_cast<atx::u64>(is_robust)));
    } else {
      // No usable regime partition for this survivor: fold a fixed not-robust marker
      // so the screen is still deterministic and order-sensitive.
      digest_acc = static_cast<atx::u64>(
          atx::core::hash_combine(static_cast<std::size_t>(digest_acc), static_cast<atx::u64>(a),
                                  static_cast<atx::u64>(0)));
    }
  }
  return digest_acc;
}

} // namespace atx::engine::factory
