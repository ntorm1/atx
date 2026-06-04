# Phase 1 — Implementation Progress

**Worktree:** _(none — see Plan adjustments)_ in-place on the active feature line.
**Branch:** `feat/atx-core-stdlib` (the active monorepo feature line; atx-core lives here too).
**Base:** `feat/atx-core-stdlib` @ `e10a8c2` (`feat(core): domain — Price/Quantity/Notional/Symbol/Bar/Tick`).
**Started:** `2026-06-02` · **Closed:** `2026-06-03`
**Source plan:** [`phase-1-event-spine-implementation-plan.md`](phase-1-event-spine-implementation-plan.md)
**Prior progress:** none (first engine sprint)
**Execution:** subagent-driven development — fresh implementer subagent per unit, then a spec-compliance
review and a code-quality review before the unit is marked done. **Status: CLOSED — all 7 units green.**

---

## Plan adjustments vs. the source plan

**Kickoff posture (2026-06-02).**

1. **All atx-core upstreams have landed — Phase 1 ran fully GREEN, no `atx_engine_pending` staging.**
   atx-core shipped, on this same branch, the headers Phase 1 needs: **L8** `time`
   (`atx/core/datetime.hpp` — `time::Timestamp`) and `domain` (`domain/domain.hpp` — `Symbol`,
   `Bar{ts,o,h,l,c,vol}`, `Tick`, `Side`), **L4** `concurrent::disruptor`, **L3** `fixed_vector` +
   `hash_map`, and `hash`. **No atx-core stub was required**; every unit built against the real API.

2. **As-built API deltas (the plan is a fossil; the ledger records reality):**
   - `domain::Bar` carries a **single** `ts` (open/close-agnostic), **not** `close_ts`. The bitemporal
     axes live on the engine `Event`/`MarketPayload`, with `event_time := bar.ts` (release-at-close).
   - `domain::Symbol` is `{u32 id}` (POD) — rides the ring directly.
   - Price/Quantity wrap exact `Decimal` (i64 mantissa, Rule-of-Zero) → `Bar`/`Tick` trivially-copyable,
     so the `Event` POD assertion holds.

3. **Worktree deviation (deliberate; agent §"deviate explicitly with reason").** The human was actively
   working in this checkout on `feat/atx-core-stdlib`. A separate worktree would force a fresh `build/` +
   a multi-hundred-MB FetchContent re-download onto a OneDrive-synced path and split the ledger across
   trees. Phase 1 was built **in-place** on the current feature branch (NOT master), reusing the
   configured `build/`, per-unit `feat(p1-N)` commits, no push. **Consequence for close:** there is no
   worktree/branch to merge — the work is already on the active line. The plan's P1-6 "merge `--no-ff`"
   step is N/A.

4. **Shared-branch ref race (observed; recovery posture).** Multiple agents committed to the single
   `feat/atx-core-stdlib` ref in one shared working tree (atx-core L9 `series`, `io`/parquet, linalg).
   Twice an engine commit was momentarily **orphaned** when a parallel commit won the branch-pointer race
   (the P1-2 shared-index incident; the P1-3 cleanup `e942f08`/`f5bde7a`). Mitigation in force: stage only
   explicit engine pathspecs, and after each commit **verify `merge-base --is-ancestor` vs HEAD and
   re-attach any orphan** (the P1-3 cleanup was re-attached as `0c01b8a`). No engine work was lost.

Deferred (out of Phase 1 scope — see ROADMAP):

- Strategy / Portfolio / ExecutionSim / P&L / cost model → **Phase 2**.
- Alpha-research layer (columnar eval, formulaic vocab) → **Phase 3**.

---

## Per-unit ledger

| Unit | Status | Commit | Notes |
|------|--------|--------|-------|
| P1-0 | ✅ done | `cf6252d` | `fwd.hpp`; `tests/CMakeLists.txt` → `*_test.cpp` glob + `atx_warnings` + Threads; `bench/` (`*_bench.cpp` glob, `ATX_BUILD_BENCH`); engine lib links `atx_warnings`; `scaffold_test` green. |
| P1-1 | ✅ done | `554cceb`+`14b8c2e` | `event/event.hpp`: `EventType{Market,Signal,Order,Fill}` (`:u8`); cache-aligned trivially-copyable `Event{knowledge_ts,event_ts,type,payload}`; payload = `std::array<std::byte,kPayloadBytes=64>` + memcpy accessors (no union-active-member UB). Guards `sizeof==128`/`alignof==64`. **13 tests**. spec ✅ + quality APPROVED. |
| P1-2 | ✅ done | `1e00593`+`f83c273` | `data/market.hpp`: `MarketPayload{symbol, Kind{Bar,Tick}, delisted_final, union{Bar,Tick}}` (tagged union → **sizeof==56** ≤ 64; tag-guarded `as_bar`/`as_tick`). `make_market_bar`/`tick` set `event_ts=ts`, enforce `ATX_ASSERT(knowledge_ts ≥ ts)`. **15 tests** (2 EXPECT_DEATH). spec ✅ + quality APPROVED. |
| P1-3 | ✅ done | `8e6602b`+`0c01b8a` | `bus/event_bus.hpp`: thin `EventBus<Cap,ConsumerCount,Producer>` over `concurrent::Disruptor` (no ring reinvented). Zero-copy `claim_slot`; `drain_in_order` reads `published_sequence()` once, drains each registered consumer in registration order (deterministic), `noexcept` fail-loud. Zero-alloc proven. **10 tests** + 2-thread smoke; bench. spec ✅ + quality APPROVED. (`0c01b8a` = cleanup re-attached after the ref-race orphaned `e942f08`.) |
| P1-4 | ✅ done | `f62f465`+`5e3fd71` | `clock/sim_clock.hpp`: `SimClock` monotonic `advance_to` (`ATX_ASSERT(t ≥ now_)`), `constexpr` `now()`/`is_visible(knowledge_ts)` look-ahead gate (`knowledge_ts ≤ now_`); restatement-window semantics documented + tested. **13 tests** (EXPECT_DEATH on backward advance). spec ✅ + quality APPROVED (`5e3fd71` = constexpr per agent §2). |
| P1-5 | ✅ done | `90470da` | `data/data_handler.hpp`: `IDataHandler` + `InMemoryBarFeed` k-way frontier merge (min-heap on `(knowledge_ts, source_idx)`). Advances clock to frontier **before** publishing the coalesced slice (no look-ahead); monotonicity `ATX_ASSERT` on re-arm (fail-closed); survivorship (delisted-final passed through, never filtered); zero-alloc per `step()`; caller-drains seam. **11 tests** (incl. EXPECT_DEATH on unsorted source). spec ✅ + quality APPROVED. |
| P1-6 | ✅ done | `e4bac79` | `tests/replay_determinism_test.cpp`: `RecordingConsumer` folds an **order-sensitive** `hash_combine` digest over the semantic event stream; `replay(sources)` runs the full feed→bus→consumer path. Proof: identical input → identical digest; **3 mutations** (reordered tie / changed value / added late bar) → distinct digests (non-vacuous, reviewer-verified empirically). **6 tests**. spec ✅ + quality APPROVED. |

### P1-3 measured throughput

| Config | metric | ns | Host / build |
|--------|--------|-----|--------------|
| SP→SC publish (claim+fill+publish+lockstep drain) | ns/op | ~112–135 | 16-core 2.5 GHz, **Debug/clang-cl** (upper bound) |
| SP→SC drain dispatch (/256 batch) | ns/event | ~21–22 | same |
| SP→2-consumer drain (/256) | ns/event-delivery | ~22–33 | same |

> **Measured, Debug build** — not a release/calibrated figure. Satisfies the "measure, don't assume" gate
> only; the refuted "52 ns/hop" Disruptor figure is **not** used.

### Deferred residuals (lift to ROADMAP future-work backlog)

- **Linux/clang TSan pass** for the EventBus threaded path (clang-cl/Windows has no usable TSan; the
  2-thread test is race-clean by construction — join before assert).
- **`.clangd` config bug (pre-existing, `6455a22`):** `Diagnostics.ClangTidy: true` is invalid (clangd
  wants a dictionary), so the IDE clang-tidy diverges from the enforced CLI `clang-tidy -p build` gate
  (caused repeated IDE false-positives this sprint). One-line fix: drop the key (clang-tidy stays on via
  the `.clang-tidy` files).
- **`EventBus<>&` ctor rigidity:** `InMemoryBarFeed` binds the *default* bus template params
  (`ConsumerCount=1`), so a second gated consumer can't share the publishing bus and tests heap-allocate
  the ~8 MB ring. If Phase 2 needs smaller rings / multi-consumer feeds, template the feed on the bus type.
- **Fixed-capacity merge heap:** the merge uses `std::priority_queue` (baseline; ≤ N entries, no
  steady-state alloc). The 4-ary intrusive heap over `fixed_vector` is the noted cache-friendly upgrade.
- **clang-tidy MSVC→clang driver:** `macro.hpp: 'spdlog/spdlog.h' file not found` from untranslated
  `/external:I` flags — tooling note, not a defect (real build clean).
- **PIT universe intervals / corporate-action records / restatement versioning** — designed-for in the
  `MarketPayload` layout (8 B headroom), not built (Phase 3/5).

---

## Phase 1 sprint commits

| Commit | Unit | Test counts |
|--------|------|-------------|
| `cf6252d` | marker (P1-0) | `EngineScaffold` 1/1 |
| `554cceb`+`14b8c2e` | P1-1 | EventTaxonomy 13/13 |
| `1e00593`+`f83c273` | P1-2 | MarketData 15/15 |
| `8e6602b`+`0c01b8a` | P1-3 | EventBus 10/10 (+2-thread smoke) |
| `f62f465`+`5e3fd71` | P1-4 | SimClock 13/13 |
| `90470da` | P1-5 | DataHandler 11/11 |
| `e4bac79` | P1-6 | ReplayDeterminism 6/6 |

**Phase 1 adds 69 new tests (first engine sprint; total engine footprint: 69/0/0 in one binary
`atx-engine-tests`, plus the `atx-engine-bench` target).** All green under `/W4 /permissive- /WX`;
clang-tidy (CLI `-p build`) + clang-format clean across every unit.

---

## What Phase 1 proves / Next sprint priorities

**Phase 1 proves the engine has a deterministic, point-in-time, survivorship-safe event spine.** A fixed
multi-source feed replays to a **byte-identical event-hash** across independent runs, and three distinct
input mutations each perturb that hash (the proof is non-vacuous, empirically verified). A decision made
at the clock's frontier can never see data whose `knowledge_time` exceeds `now()` — the DataHandler
advances the clock to each frontier *before* publishing the coalesced slice, and the `SimClock` gate
admits only `knowledge_ts ≤ now`. Delisted symbols are carried with their final bar (no survivorship
filtering). The bus is a thin, zero-allocation, zero-copy facade over the lock-free LMAX Disruptor with a
deterministic registration-order drain. Every hot-path type is trivially-copyable and rides a
pre-allocated ring.

**Baton to Phase 2:** plug a Strategy → Portfolio → ExecutionSim (√-impact cost) loop onto this bus to
earn the first runnable, cost-honest backtest on one alpha. The strategy is reached through the
`ISignalSource` seam (VM-centric); the loop goes green on `ScriptedSignalSource` before Phase 3's alpha
VM lands. The `EventType` taxonomy already reserves `Signal`/`Order`/`Fill` for the Phase-2 payloads.

**Carry-forward decisions (from the final holistic review — design notes, not Phase-1 defects):**
1. **Wire or re-scope `SimClock::is_visible()`.** Today the no-look-ahead guarantee is enforced by *feed
   ordering* (`InMemoryBarFeed::step()` advances the clock to the frontier before publishing), so the
   gate the SimClock header advertises is correct but **not yet load-bearing** at the bus/consumer seam.
   A new producer (e.g. a Phase-2 strategy re-publishing Signals/Orders) that forgets advance-before-
   publish would leak look-ahead untyped. Phase 2 should either assert `clock.is_visible(e.knowledge_ts)`
   inside `drain_in_order` delivery, or document the gate as a caller-side PIT/restatement helper.
2. **Resolve `EventBus<>&` rigidity before the 2nd gated consumer.** `InMemoryBarFeed` hard-binds the
   default bus params (`ConsumerCount=1`); a strategy consumer + an exec-sim consumer both gated on the
   feed's bus need `ConsumerCount≥2` — a different type the ctor can't take. Template the feed on the bus
   type, or introduce a producer-facing bus handle. Biggest structural constraint on building Phase 2.
3. Untested seams (theoretical at Phase-1's fixed 64K ring): a coalesced slice larger than ring capacity
   (wrap-vs-slice), and `make_market_tick` through the feed (bar-only today). Add when ring sizing becomes
   configurable / a tick feed lands.
