# atx-engine — Agent Onboarding

Quant backtesting engine. Consumer of `atx-core` (the C++20 stdlib in the same monorepo). Read this, not the whole tree, to pick up fast.

**Authority:** [`../cpp/agent.md`](../cpp/agent.md) governs all C++ here (safety-critical C++20: no UB, `const`/`constexpr`/`noexcept`/`[[nodiscard]]`, `Result<T>` not exceptions, exhaustive enum switches, functions ≤60 lines, TDD, zero hot-path alloc). This file is engine-specific deltas + how to build without pain.

---

## Build & test (don't reinvent this)

Toolchain: **Ninja + clang-cl + vcpkg**, build dir `build/`, type Debug. Run from a **VS Developer shell** (the MSVC env must be present) with `VCPKG_ROOT` set. A `build/` is usually already configured — reuse it.

```bash
# configure (only if build/ is missing or CMakeLists changed):
cmake --preset ninja -DATX_BUILD_TESTS=ON -DATX_BUILD_BENCH=ON

# build the engine tests / benches:
cmake --build build --target atx-engine-tests
cmake --build build --target atx-engine-bench        # ATX_BUILD_BENCH=ON

# run (use --test-dir, NOT cd):
ctest --test-dir build -R EventTaxonomy --output-on-failure          # one suite
ctest --test-dir build -R "EventTaxonomy|MarketData|EventBus|SimClock|DataHandler|ReplayDeterminism"   # whole engine
build/bin/atx-engine-bench.exe --benchmark_filter=EventBus           # a bench
```

- Tests/benches are **auto-globbed** (`tests/*_test.cpp`, `bench/*_bench.cpp`, CONFIGURE_DEPENDS) — drop a file, rebuild, done. Do **not** hand-edit `CMakeLists.txt`.
- The warnings gate `atx_warnings` (`/W4 /permissive- /WX`) is linked — any warning fails the build.
- **Bash tool CWD can drift** (a stray `cd build` persists). Use absolute paths, or `git -C <root>`, or `ctest --test-dir build`.

---

## What exists — Phase 1 event spine (CLOSED, 69 tests, all green)

Header-only spine under `include/atx/engine/`, namespace `atx::engine`. Compose these; don't rebuild them:

| Header | Type | Role |
|--------|------|------|
| `event/event.hpp` | `event::Event` | cache-aligned trivially-copyable POD (128 B); 2 bitemporal ts + `EventType{Market,Signal,Order,Fill}` + 64 B memcpy payload (`store_payload<P>`/`payload_as<P>`). |
| `data/market.hpp` | `data::MarketPayload` + `make_market_bar`/`make_market_tick` | tagged `union{Bar,Tick}` (56 B); enforces `knowledge_ts ≥ event_ts`. |
| `bus/event_bus.hpp` | `EventBus<Cap,ConsumerCount,Producer>` | thin facade over atx-core `concurrent::Disruptor`; `claim_slot`/`publish`/`drain_in_order` (registration-order, deterministic, zero-alloc). |
| `clock/sim_clock.hpp` | `SimClock` | monotonic `advance_to`; `is_visible(knowledge_ts)` look-ahead gate. |
| `data/data_handler.hpp` | `IDataHandler` + `InMemoryBarFeed` | k-way merge feed; `step()` advances clock to frontier then publishes the coalesced slice; caller drains between steps. |

Canonical loop + full API: [`../../atx-engine/plans/p0/phase-1.md`](../../atx-engine/plans/p0/phase-1.md). Old placeholder `engine.hpp`/`engine.cpp` (`int step(int)`) is dead — ignore/replace.

**Three invariants are tested — do not break them:** (1) **determinism** (same feed → byte-identical hash; `replay_determinism_test.cpp` is the proof), (2) **no look-ahead** (clock advances *before* publish; consumer only sees `knowledge_ts ≤ now`), (3) **no survivorship** (delisted symbols carried with their final bar).

---

## Landmines (this is the stuff that wastes hours)

- **Shared-branch ref race.** Multiple agents commit to `feat/atx-core-stdlib` in one working tree. Commits get **orphaned** when another wins the branch-pointer race. After every commit: `git -C <root> merge-base --is-ancestor <sha> HEAD` — if orphaned, re-attach (`git checkout <sha> -- <files>` then re-commit). Always stage **explicit pathspecs**, never `git add -A` (shared index).
- **clang-tidy is disabled.** Do not run CLI clang-tidy or use it as an agent gate. It is intentionally disabled repo-wide because the broad profile is prohibitively slow on umbrella-header translation units and third-party-heavy compile databases.
- **Heap-allocate `EventBus<>`.** Default ring is ~8 MB (65536 × 128 B); a stack instance overflows the 1 MB Windows stack. Use `std::make_unique` (see existing tests).
- **`EventBus<>&` is type-rigid.** `InMemoryBarFeed` binds the *default* bus params (`ConsumerCount=1`). A 2nd gated consumer needs a different bus type the ctor won't take — template the feed before adding one (Phase-2 carry-forward).
- **`SimClock::is_visible()` isn't load-bearing yet** — no-look-ahead is currently enforced by feed *ordering*, not the gate. Wire it into `drain_in_order`, or document it as a caller PIT helper, when adding new producers.
- Bench numbers in docs are **Debug** upper bounds — not calibrated; never cite as release figures.

---

## Workflow

- **TDD always** (failing GoogleTest first; cover boundaries + `EXPECT_DEATH` for `ATX_ASSERT` preconditions).
- A unit is done only when header + tests + **clang-format clean + `/W4 /WX` build + tests green**. Do not run clang-tidy unless the user explicitly re-enables it. `clang-format -i <file>` to fix format.
- Commit per unit `feat(pN-M): …` with the `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` trailer. **Do not push** unless asked.
- This is `feat/atx-core-stdlib` (the active line) — no worktree; atx-core lives here too.

---

## Status & what's next

- **Phase 1 (event spine): ✅ CLOSED.** Plan + ledger + user-ref in [`../../atx-engine/plans/p0/`](../../atx-engine/plans/p0/) (`ROADMAP.md` is canonical).
- **Phase 2 (backtest loop + portfolio + exec-sim): scoped, plan frozen** — `phase-2-backtest-loop-implementation-plan.md`. VM-centric: strategy reached via an `ISignalSource` seam; loop goes green on `ScriptedSignalSource` before Phase 3. `Signal`/`Order`/`Fill` payload slots already reserved in `EventType`.
- **Phase 3 (alpha-expression DSL → vectorized bytecode VM): scoped, plan frozen** — `phase-3-alpha-expression-dsl-implementation-plan.md`. Front-end (lexer/parser/typecheck/DAG) unblocked; VM blocked on atx-core L5/L6/L9.
- Research grounding: `../../atx-engine/research/*.md`.
