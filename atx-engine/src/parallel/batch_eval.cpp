#include "atx/engine/parallel/batch_eval.hpp"

#include <cstddef>  // std::size_t
#include <memory>   // std::unique_ptr, std::make_unique
#include <optional> // std::optional
#include <utility>  // std::move
#include <vector>   // std::vector

#include "atx/engine/alpha/vm.hpp" // alpha::Engine

namespace atx::engine::parallel {

namespace detail {

// Evaluate one program on `engine` and move its roots into `out.alphas` at the
// pre-computed base offset. On evaluate error, records it in `slot` (left
// untouched on success). No shared mutable state: the caller guarantees `engine`
// is this worker's own and the [offset, offset+nroots) output range is disjoint.
void evaluate_into(atx::engine::alpha::Engine &engine,
                   const atx::engine::alpha::Program &prog, std::size_t offset,
                   atx::engine::alpha::SignalSet &out,
                   std::optional<atx::core::Error> &slot) {
  auto r = engine.evaluate(prog);
  if (!r.has_value()) {
    slot = r.error();
    return;
  }
  atx::engine::alpha::SignalSet ss = std::move(r).value();
  for (std::size_t j = 0; j < ss.alphas.size(); ++j) {
    out.alphas[offset + j] = std::move(ss.alphas[j]);
  }
}

} // namespace detail

// Evaluate `progs` across `pool`'s workers — one program per work item, each on a
// per-worker stateful Engine — and assemble the roots into one SignalSet in
// (program order, then root order within a program). Byte-identical to the
// single-thread Engine::evaluate of the equivalent batch program, and invariant
// across worker counts. Returns the LOWEST-index program's evaluate error, if any.
atx::core::Result<atx::engine::alpha::SignalSet>
parallel_evaluate(std::span<const atx::engine::alpha::Program> progs,
                  const atx::engine::alpha::Panel &panel, DetPool &pool) {
  namespace alpha = atx::engine::alpha;

  // (2) Prefix-sum output offsets so each program's roots land in disjoint slots.
  std::vector<std::size_t> offsets(progs.size());
  std::size_t total_roots = 0;
  for (std::size_t k = 0; k < progs.size(); ++k) {
    offsets[k] = total_roots;
    total_roots += progs[k].roots.size();
  }

  // Size the output BEFORE the parallel region — no concurrent vector growth.
  alpha::SignalSet out;
  out.dates = panel.dates();
  out.instruments = panel.instruments();
  out.alphas.resize(total_roots);

  // (3) One stateful Engine per worker. Engine borrows a `const Panel&` ⇒ not
  // move-assignable ⇒ hold via unique_ptr so the vector never assigns on growth.
  std::vector<std::unique_ptr<alpha::Engine>> engines;
  engines.reserve(pool.n_workers());
  for (atx::usize w = 0; w < pool.n_workers(); ++w) {
    engines.push_back(std::make_unique<alpha::Engine>(panel));
  }

  // (4) Warm each engine once ON ITS OWN WORKER THREAD so the timed dispatch loop
  // allocates nothing (the slot pool has already grown). Ignore the Result here —
  // a real error resurfaces in the main loop below.
  if (!progs.empty()) {
    pool.for_each_worker([&](std::size_t wid) { (void)engines[wid]->evaluate(progs[0]); });
  }

  // (5) Per-item error slots (one per program; written only by the owning index).
  std::vector<std::optional<atx::core::Error>> errs(progs.size());

  // (6) Fan the programs across the workers. SAFETY: each `wid` touches ONLY
  // engines[wid]; each `k` writes ONLY out.alphas[offsets[k] .. +nroots) and
  // errs[k] — disjoint slots, no shared mutable state, panel is const. No cross-
  // worker FP accumulation ⇒ determinism by construction.
  pool.parallel_for(progs.size(), [&](std::size_t k, std::size_t wid) {
    detail::evaluate_into(*engines[wid], progs[k], offsets[k], out, errs[k]);
  });

  // (7) After the barrier, scan ascending: the lowest-index error is the
  // deterministic failure.
  for (std::size_t k = 0; k < errs.size(); ++k) {
    if (errs[k].has_value()) {
      return atx::core::Err(*errs[k]);
    }
  }
  return out;
}

} // namespace atx::engine::parallel
