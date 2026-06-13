# atx-engine: header-only → incremental-compilation refactor

Date: 2026-06-13
Status: approved (pilot phase)

## Goal

Convert the `atx-engine` header-only library into a header + source-file
library so changing one implementation no longer forces every translation
unit that includes its header to recompile. Use private implementation
(pImpl) where it cuts compile times without hurting hot paths. Functions
may stay in headers when inlining yields a real optimization.

Constraints (from the requester):
- Go one file at a time.
- Keep functions and comments exactly the same (verbatim moves).
- Do not touch files the Sprint-7 plan is working on.

## The split pattern (applied per file)

Every symbol falls into one of three buckets:

1. **Stays inline in the header.** Tiny/hot free functions and methods where
   inlining matters, all `struct`/`enum`/type-alias declarations, and every
   template (a template body cannot move to a `.cpp` without explicit
   instantiation, so templates stay).
2. **Body moves to a `.cpp`; declaration stays in the header.** Cold, heavy
   function/method bodies. Drop the `inline` keyword on the definition; the
   header keeps the declaration (including `[[nodiscard]]`/`noexcept`). This
   is the primary compile-time win: the body is parsed once, not in every
   includer.
3. **True pImpl (opaque `std::unique_ptr<Impl>`).** ONLY for cold classes
   whose *private members* drag heavy includes into the public header, and
   only where the per-object heap indirection cannot land on a hot path.
   Most numeric classes here will NOT qualify — that is expected and
   documented per file with a one-line rationale.

## Mechanics

- New sources mirror the include tree under `src/` (e.g.
  `include/atx/engine/factory/crossover.hpp` →
  `src/factory/crossover.cpp`).
- Each `.cpp` `#include`s its own header first (verifies the header is
  self-sufficient), then any body-only includes.
- Each new `.cpp` is added **explicitly** to the `atx-engine` STATIC library
  in `atx-engine/CMakeLists.txt`, matching the existing `src/engine.cpp`
  style (no glob — deterministic source list).
- Symbols land in `atx-engine.lib`. The test target already links
  `atx::engine`, so no test wiring changes.

## Policy calls

- **Comments move verbatim.** A handful of comments make a now-false
  *structural* claim (e.g. crossover.hpp: "Header-only; COLD path ..."). To
  honour "keep comments exactly the same," these are left verbatim and the
  slight staleness is accepted.
- **Include trimming is conservative.** Body-only includes move from the
  header to the `.cpp` only when the header provably no longer needs them.
  The build + tests catch any consumer that leaned on a transitive include.

## Verification loop (every file)

```
cmake --build build --target atx-engine        # library compiles
cmake --build build --target atx-engine-tests  # tests link the moved symbols
ctest / run the file's *_test                  # behaviour unchanged
```

Green before moving to the next file. One file = one commit.

## Scope

Off-limits (Sprint-7 active set):
`book/*`, `cost/cost_aware`, `risk/dead_factor`, `risk/multi_period`.

Sweep order: **factory/** first (pilot), then outward through the
self-contained non-template subsystems (alpha/, eval/, learn/, library/,
combine/, loop/, parallel/, exec/, data/), skipping the off-limits set.

## Pilot

`factory/crossover.hpp` (all free functions, no class — exercises bucket 1
and 2, not pImpl):
- Header keeps `CrossoverCfg`, the inline predicates `shape_broadcastable`
  and `compatible`, and a declaration of `subtree_crossover`.
- `src/factory/crossover.cpp` gets `subtree_crossover` plus its
  `detail::splice_visit` and `detail::uniform_cut_index` helpers (verified
  used nowhere else), with `<unordered_map>`, `<vector>`, and `parser.hpp`
  moved out of the header.
- CMake wired, `factory_crossover_test` green.

pImpl (bucket 3) is demonstrated on the first class-bearing factory file in
the sweep, not on the pilot.
