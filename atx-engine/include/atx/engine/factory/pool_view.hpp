#pragma once

// atx::engine::factory — PoolView: the corr-to-pool backing seam (S4b-2).
//
// ===========================================================================
//  What this unit is — the scale lever
// ===========================================================================
//  pool_aware_fitness scores a candidate's MARGINAL diversification against an
//  already-admitted pool. The ONE operation it needs from that pool is the
//  candidate's worst-case redundancy:
//
//      worst_corr(pnl) = MAX |corr| of the candidate vs. every pool member.
//
//  Today that runs against an EPHEMERAL combine::AlphaStore with an O(N) scan
//  (corr_to_pool, fitness.hpp). The persistent library::Library already owns an
//  O(neighbors) SimHash corr index (online_corr_to_pool) that nothing in the
//  factory used. PoolView is the thin seam that lets fitness score against EITHER
//  backing with NO second corr implementation — each backing forwards to its own
//  existing MAX-|corr| impl:
//    * AlphaStorePool -> corr_to_pool(..., Reduce::Max)  (O(N), exact, S3 fixtures)
//    * LibraryPool    -> Library::worst_corr_to_pool(...) (O(neighbors), the engine)
//  Both return MAX, so they AGREE EXACTLY whenever the SimHash recall is 1.0 (the
//  S4-3 / S4-5 orthogonal equal-norm-basis fixtures hit recall == 1.0). The new
//  pool_aware_fitness overload here computes redundancy = view.worst_corr(pnl) and
//  is otherwise byte-for-byte the §4.6 score (it reuses detail::fitness_core +
//  detail::finish_report from fitness.hpp, so wq/robust/dsr/raw are written once).
//
//  INCLUDE DIRECTION: fitness.hpp stays library-AGNOSTIC (it does not know about
//  library::Library); this header is the library-backed layer that #includes
//  fitness.hpp (for the helper) and library.hpp. The PoolView-taking overload
//  lives HERE, not in fitness.hpp, to avoid a fitness->library include cycle.
//
//  NOTE on the MAX-vs-MEAN intentional split: the LEGACY AlphaStore overload in
//  fitness.hpp uses Reduce::Mean for redundancy (a whole-pool diversification
//  discount). This PoolView overload is MAX-based — the worst single-neighbor
//  redundancy — which is exactly what the SimHash index can serve cheaply and what
//  the admission gate screens on. The two overloads therefore measure different
//  pool semantics BY DESIGN; this one is the scalable, gate-consistent path.

#include <span> // std::span

#include "atx/core/error.hpp" // Result, Ok
#include "atx/core/types.hpp" // atx::f64

#include "atx/engine/alpha/panel.hpp"          // alpha::Panel
#include "atx/engine/combine/store.hpp"        // combine::AlphaStore
#include "atx/engine/exec/execution_sim.hpp"   // exec::ExecutionSimulator
#include "atx/engine/factory/fitness.hpp"      // corr_to_pool, Reduce, detail::fitness_core/...
#include "atx/engine/factory/genome.hpp"       // factory::Genome
#include "atx/engine/library/library.hpp"      // library::Library
#include "atx/engine/loop/weight_policy.hpp"   // engine::WeightPolicy

// Forward declaration — a pointer parameter needs only a forward decl (also
// declared in fitness.hpp, included above; kept here so this header is
// self-documenting and does not rely on transitive include order).
namespace atx::engine::alpha { class Engine; }

namespace atx::engine::factory {

// ===========================================================================
//  PoolView — the single corr-to-pool operation, backing-agnostic.
//
//  One operation: MAX |corr| of a candidate pnl against the pool. Two backings,
//  one corr impl each, both return MAX so they agree (recall == 1.0 caveat — see
//  the header). The interface is intentionally minimal: fitness needs nothing else
//  from a pool than this redundancy scalar.
// ===========================================================================
struct PoolView {
  virtual ~PoolView() = default;
  [[nodiscard]] virtual atx::f64 worst_corr(std::span<const atx::f64> pnl) const = 0;
};

// AlphaStore backing: the O(N) EXACT MAX |corr| scan (corr_to_pool, Reduce::Max).
// The ground-truth backing the S3 factory fixtures build against. Borrows `pool`.
class AlphaStorePool final : public PoolView {
public:
  explicit AlphaStorePool(const combine::AlphaStore &pool) noexcept : pool_{pool} {}
  [[nodiscard]] atx::f64 worst_corr(std::span<const atx::f64> pnl) const override {
    return corr_to_pool(pnl, pool_, Reduce::Max);
  }

private:
  const combine::AlphaStore &pool_;
};

// Library backing: the O(neighbors) SimHash MAX |corr| scan (the real engine).
// Forwards to the persistent index nothing in the factory previously touched.
// Borrows `lib`.
class LibraryPool final : public PoolView {
public:
  explicit LibraryPool(const library::Library &lib) noexcept : lib_{lib} {}
  [[nodiscard]] atx::f64 worst_corr(std::span<const atx::f64> pnl) const override {
    return lib_.worst_corr_to_pool(pnl);
  }

private:
  const library::Library &lib_;
};

// ===========================================================================
//  pool_aware_fitness (PoolView overload) — the §4.6 score against EITHER backing.
//
//  Identical to the AlphaStore overload (fitness.hpp) in every term EXCEPT the
//  redundancy, which is the MAX |corr| the PoolView serves (vs. the legacy MEAN
//  scan). wq / robust / dsr / haircut / diversify / raw all come from the shared
//  detail::fitness_core + detail::finish_report helpers, so there is NO duplicated
//  wq/dsr math. Same other params as the legacy overload (full panel, policy, sim,
//  cfg, optional weak panel). Returns Err only if the candidate fails to
//  compile/evaluate/extract (full or weak panel).
//
//  RECALL CAVEAT: when `view` is a LibraryPool, worst_corr is the MAX |corr| over
//  the candidate's SimHash NEIGHBORS — it equals the AlphaStorePool's exhaustive
//  MAX only when recall == 1.0 (true on the orthogonal equal-norm-basis fixtures).
// ===========================================================================
[[nodiscard]] atx::core::Result<FitnessReport>
pool_aware_fitness(const Genome &cand, const PoolView &view, const alpha::Panel &panel,
                   const WeightPolicy &policy, const exec::ExecutionSimulator &sim,
                   const FitnessCfg &cfg, const alpha::Panel *weak_panel = nullptr,
                   alpha::Engine *engine = nullptr);

} // namespace atx::engine::factory
