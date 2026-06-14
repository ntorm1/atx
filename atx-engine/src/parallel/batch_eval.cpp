#include "atx/engine/parallel/batch_eval.hpp"

#include <cstddef>    // std::size_t, std::byte
#include <cstring>    // std::memcpy (eval slot gather)
#include <functional> // std::function (dispatch wrapper signature)
#include <memory>     // std::unique_ptr, std::make_unique
#include <optional>   // std::optional
#include <span>       // std::span (serialized input view)
#include <utility>    // std::move
#include <vector>     // std::vector

#include "atx/core/macro.hpp" // ATX_ASSERT, ATX_CHECK

#include "atx/engine/alpha/vm.hpp" // alpha::Engine

#include "atx/engine/parallel/executor.hpp"      // SlotView, WorkloadId, slot_view_bytes
#include "atx/engine/parallel/workload_eval.hpp" // serialize_eval_input (serialized process path)

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

namespace {

// THE ONE batch-eval map (S7.5a). Both the DetPool& and the IExecutor& overloads
// forward here, so there is a SINGLE implementation of the map — no copy-paste
// divergence. The only thing that differs between substrates is HOW shards are
// dispatched, which is injected as `dispatch`:
//
//   dispatch(n_items, body) runs body(item, worker_id) for every item in
//   [0, n_items), worker_id in [0, n_workers), blocking until all complete and
//   rethrowing the lowest-index body exception (the DetPool::parallel_for /
//   IExecutor::parallel_for contract). It returns Ok() in the steady state; a
//   non-Ok return means the SUBSTRATE itself rejected the in-process map (e.g.
//   ProcessExecutor::parallel_for), which we surface as the function's error.
//
// `n_workers` is the substrate's resolved worker count (pool.n_workers() /
// exec.workers()) — it sizes the per-worker Engine vector and the warmup fan.
//
// WHY BYTE-IDENTICAL ACROSS SUBSTRATES: the map MATH is untouched (it is exactly
// the as-built strategy-A body). Each program k writes ONLY its disjoint output
// slots and errs[k]; each worker wid touches ONLY engines[wid]; the panel is
// const; there is no cross-worker FP accumulation. So which substrate dispatches
// the shards, and which worker grabs which shard, cannot move a single result bit
// — the digest is identical to the single-thread batch path and invariant across
// worker counts, by construction.
template <class Dispatch>
[[nodiscard]] atx::core::Result<atx::engine::alpha::SignalSet>
parallel_evaluate_impl(std::span<const atx::engine::alpha::Program> progs,
                       const atx::engine::alpha::Panel &panel, atx::usize n_workers,
                       Dispatch &&dispatch) {
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
  engines.reserve(n_workers);
  for (atx::usize w = 0; w < n_workers; ++w) {
    engines.push_back(std::make_unique<alpha::Engine>(panel));
  }

  // (4) Warm each engine once so the timed dispatch loop allocates nothing (the
  // slot pool has already grown). The DetPool& path warmed via for_each_worker
  // (engine wid on worker wid); the substrate-agnostic seam has no per-worker
  // primitive, so we warm via the SAME dispatch over [0, n_workers): shard w warms
  // engines[w], so EVERY engine [0, n_workers) is warmed exactly once. The barrier
  // inside dispatch completes warmup fully before any eval shard runs, and warming
  // is a pure local mutation of one engine's slot pool — no engine is ever touched
  // concurrently, and warmup touches NO result bit, so the output is unchanged
  // whether engine w warms on worker w (DetPool for_each_worker) or on whichever
  // worker grabs shard w here. Ignore the per-warm Result (a real error resurfaces
  // in the main loop below); a non-Ok DISPATCH status means the substrate rejected
  // the in-process map -> surface it.
  if (!progs.empty()) {
    ATX_TRY_VOID(dispatch(n_workers, [&](std::size_t w, std::size_t /*wid*/) {
      (void)engines[w]->evaluate(progs[0]);
    }));
  }

  // (5) Per-item error slots (one per program; written only by the owning index).
  std::vector<std::optional<atx::core::Error>> errs(progs.size());

  // (6) Fan the programs across the workers. SAFETY: each `wid` touches ONLY
  // engines[wid]; each `k` writes ONLY out.alphas[offsets[k] .. +nroots) and
  // errs[k] — disjoint slots, no shared mutable state, panel is const. No cross-
  // worker FP accumulation ⇒ determinism by construction.
  ATX_TRY_VOID(dispatch(progs.size(), [&](std::size_t k, std::size_t wid) {
    detail::evaluate_into(*engines[wid], progs[k], offsets[k], out, errs[k]);
  }));

  // (7) After the barrier, scan ascending: the lowest-index error is the
  // deterministic failure.
  for (std::size_t k = 0; k < errs.size(); ++k) {
    if (errs[k].has_value()) {
      return atx::core::Err(*errs[k]);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
//  Serialized SUBMIT path (MultiProcess substrate, S7.5c): the SAME Engine::evaluate
//  map runs in a worker process via the registered WorkloadId::Eval + serialized
//  (Programs + Panel) InputView seam. Shard k (== program k) writes its nroots_k
//  alpha value columns (cells f64 each) into its pre-indexed slot; the PARENT already
//  holds the root NAMES from `progs`, so names never cross back. The parent gathers
//  the slots in (program order, then root order) into the returned SignalSet —
//  byte-identical to the in-process path and the single-thread batch, invariant
//  across worker counts (R1/§0.5).
//
//  Slot sizing: eval's per-shard column count VARIES (nroots_k differs per program),
//  unlike the streams workloads' fixed FoldResult slot. We size EVERY slot to the
//  worst case max_nroots * cells * sizeof(f64) so a single uniform-stride SlotView
//  covers all shards; shard k writes only its first nroots_k columns and the parent
//  reads back exactly nroots_k columns. (A fixed-stride slot region is what the seam
//  provides; padding the short shards costs only zeroed tail bytes, never a result
//  bit.)
[[nodiscard]] atx::core::Result<atx::engine::alpha::SignalSet>
parallel_evaluate_multiprocess(std::span<const atx::engine::alpha::Program> progs,
                               const atx::engine::alpha::Panel &panel, IExecutor &exec) {
  namespace alpha = atx::engine::alpha;

  // Prefix-sum output offsets (program order, then root order) + the max per-program
  // root count (the worst-case slot column width).
  std::vector<std::size_t> offsets(progs.size());
  std::size_t total_roots = 0;
  std::size_t max_nroots = 0;
  for (std::size_t k = 0; k < progs.size(); ++k) {
    offsets[k] = total_roots;
    const std::size_t nr = progs[k].roots.size();
    total_roots += nr;
    if (nr > max_nroots) {
      max_nroots = nr;
    }
  }

  alpha::SignalSet out;
  out.dates = panel.dates();
  out.instruments = panel.instruments();
  out.alphas.resize(total_roots);

  // Degenerate input: no roots to produce (empty progs, or every program rootless).
  // Match the in-process result for that input — an empty-shaped SignalSet, NO submit
  // (no segment, no spawn). max_nroots == 0 implies total_roots == 0, so this also
  // covers the rootless case (and avoids a zero-stride SlotView).
  if (progs.empty() || total_roots == 0 || max_nroots == 0) {
    return out;
  }

  const std::size_t cells = panel.dates() * panel.instruments();
  // A zero-cell panel (dates or instruments == 0) produces empty value columns; the
  // in-process path returns alphas with empty `values`. Honour that without a submit
  // (a zero-byte slot has nothing to transport).
  if (cells == 0) {
    for (std::size_t k = 0; k < progs.size(); ++k) {
      for (std::size_t j = 0; j < progs[k].roots.size(); ++j) {
        out.alphas[offsets[k] + j].name = progs[k].roots[j].name;
      }
    }
    return out;
  }

  const std::vector<std::byte> input = serialize_eval_input(progs, panel);
  const std::size_t slot_size = max_nroots * cells * sizeof(atx::f64);
  const std::size_t n = progs.size();

  std::vector<std::byte> buf(slot_view_bytes(n, slot_size), std::byte{0});
  SlotView slots = make_slot_view(buf, n, slot_size);
  const atx::core::Status s =
      exec.submit(WorkloadId::Eval, InputView{std::span<const std::byte>{input}}, n, slots);
  if (!s) {
    return atx::core::Err(s.error()); // a shard evaluate Err (lowest id) or infra fault
  }

  // Gather: for program k, root j -> out.alphas[offsets[k]+j] = { name, copy of slot k
  // column j (cells f64) }. Assembly order is (program order, then root order) — bit-
  // identical to the in-process parallel_evaluate_impl.
  const std::size_t col_bytes = cells * sizeof(atx::f64);
  for (std::size_t k = 0; k < n; ++k) {
    const std::span<const std::byte> cslot = slots.cslot(k);
    const std::size_t nr = progs[k].roots.size();
    ATX_ASSERT(cslot.size() >= nr * col_bytes);
    for (std::size_t j = 0; j < nr; ++j) {
      alpha::SignalSet::Alpha &a = out.alphas[offsets[k] + j];
      a.name = progs[k].roots[j].name;
      a.values.resize(cells);
      std::memcpy(a.values.data(), cslot.data() + j * col_bytes, col_bytes);
    }
  }
  return out;
}

} // namespace

} // namespace detail

// Evaluate `progs` across `pool`'s workers — one program per work item, each on a
// per-worker stateful Engine — and assemble the roots into one SignalSet in
// (program order, then root order within a program). Byte-identical to the
// single-thread Engine::evaluate of the equivalent batch program, and invariant
// across worker counts. Returns the LOWEST-index program's evaluate error, if any.
atx::core::Result<atx::engine::alpha::SignalSet>
parallel_evaluate(std::span<const atx::engine::alpha::Program> progs,
                  const atx::engine::alpha::Panel &panel, DetPool &pool) {
  // Dispatch wrapper: DetPool::parallel_for runs the body over [0, n) and rethrows
  // the lowest-index exception. It returns void and cannot reject the map, so this
  // wrapper always reports Ok() (a body failure is a thrown exception, not an Err).
  return detail::parallel_evaluate_impl(
      progs, panel, pool.n_workers(),
      [&pool](std::size_t n, const std::function<void(std::size_t, std::size_t)> &body) {
        pool.parallel_for(n, body);
        return atx::core::Ok();
      });
}

// S7.5a/S7.5c — the SAME batch eval over the substrate-agnostic IExecutor seam,
// SUBSTRATE-AWARE. The map MATH (Engine::evaluate of each Program) is UNCHANGED
// across both substrates — only the TRANSPORT differs:
//   * InProcess (ThreadExecutor): the closure-bodied parallel_for path (S7.5a),
//     shared with the DetPool& overload via detail::parallel_evaluate_impl.
//   * MultiProcess (ProcessExecutor): serialize (Programs + Panel), submit under
//     WorkloadId::Eval (n = progs.size()), gather the per-program alpha value columns
//     from the output slots in (program order, then root order).
// Output is byte-identical across both substrates, worker counts, and the single-
// thread batch path (the §0.5 capstone). An unknown substrate aborts defensively
// (ATX_CHECK) — never a silent zero-filled result.
atx::core::Result<atx::engine::alpha::SignalSet>
parallel_evaluate(std::span<const atx::engine::alpha::Program> progs,
                  const atx::engine::alpha::Panel &panel, IExecutor &exec) {
  switch (exec.substrate()) {
  case Substrate::InProcess:
    // Dispatch wrapper: IExecutor::parallel_for runs the body over [0, n) on the
    // executor's substrate (rethrowing the lowest-index exception in-process) and
    // returns a Status — Ok() in the steady state. That Err propagates out of the
    // impl. (A MultiProcess executor never reaches here; it takes the branch below.)
    return detail::parallel_evaluate_impl(
        progs, panel, exec.workers(),
        [&exec](std::size_t n, const std::function<void(std::size_t, std::size_t)> &body) {
          return exec.parallel_for(n, body);
        });
  case Substrate::MultiProcess:
    return detail::parallel_evaluate_multiprocess(progs, panel, exec);
  }
  // Unknown substrate (a future IExecutor that returns neither): fail loud rather
  // than silently return an empty SignalSet (the same fail-safe discipline as the
  // streams workloads' parallel_backtests / parallel_cpcv).
  ATX_CHECK(false && "parallel_evaluate: unknown executor substrate");
  return atx::engine::alpha::SignalSet{};
}

} // namespace atx::engine::parallel
