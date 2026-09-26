# Bounded parallel research IC

Production `1485804d`; implementation-following fixtures `6e450581`. Prerequisite imports `6a30b68b`/`e577670c` are local equivalents of root `3048951c`/`9b65981a` and must not be re-imported as new work.

Only `ic_research.hpp`, private `ic_screen.cpp`, and the existing focused `ic_research_test.cpp` change. The legacy `ic_screen.hpp` API/configuration remains unchanged. This is a runtime implementation change, not a new signal, statistical threshold, training selection or validation fit.

Research options now declare explicit `workers` in 1..4. `evaluate_research_ic(signal, cache, scratch, pool)` borrows the caller's already-owned DetPool; workers1 requires null, workers>1 requires exactly that count. The caller stays outside pool jobs and uses the same VM pool sequentially. No nested pool or additional thread stacks are created by this kernel.

Contiguous date bands run independently. Within each date the existing horizon loop, finite-pair scan, strict endpoint/guard support, signal-rank reuse check and SIMD correlation arithmetic remain in the same order. Workers write disjoint calendar-series entries. Each worker owns four N-double vectors, two N-index vectors and four integer paired counters. Paired counts reduce after the barrier; floating means, quarter checks, HAC/IID estimates and final classification retain their original serial chronological order. Signal, cached labels/ranks and per-horizon output series are not duplicated. No candidate-sized D*N worker allocation is introduced.

Scratch reports and admits all worker row buffers before allocating them. Parallel preparation additionally requires retained immutable cache plus scratch to fit `max_cache_bytes`. One-worker legacy retains its prior separate scratch-cap semantics; private bookkeeping bytes grow slightly due the explicit options/row wrapper. Runner admission must still cover panel, VM, incoming signal, composition and its existing threads. Pool4 owns the two actual runner hooks and recipe/runtime receipt binding.

G0 independently source-approved production `1485804d`: disjoint complete row coverage, worker-owned rank scratch, exact inner-horizon reuse and ordered reductions, exact pool admission and aggregate memory refusal. The existing three research/legacy cases remain and two postimplementation cases are added. New 413x96 parity coverage includes ties, NaN/infinite signals, finite-but-absent prices, membership joins, guarded labels, different mature horizons, 1/2/4 workers, and repeated scratch use with a changed candidate. Every Pearson/rank series bit, estimate field and coverage counter is compared. Refusal checks cover workers0/5/max, absent/wrong pool, cache-plus-worker budget, and short support.

`git diff --check` passed. No C++ compilation, native execution, market-data run or timing was performed in this lane. Native parity and actual speed remain parent qualification gates; no speedup is claimed from source inspection.
