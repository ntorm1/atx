# Phase 1 — Implementation Progress

**Worktree:** _(none — see Plan adjustments)_ in-place on the active feature line.
**Branch:** `feat/atx-core-stdlib` (the active monorepo feature line; atx-core lives here too).
**Base:** `feat/atx-core-stdlib` @ `e10a8c2` (`feat(core): domain — Price/Quantity/Notional/Symbol/Bar/Tick`).
**Started:** `2026-06-02`
**Source plan:** [`phase-1-event-spine-implementation-plan.md`](phase-1-event-spine-implementation-plan.md)
**Prior progress:** none (first engine sprint)
**Execution:** subagent-driven development — fresh implementer subagent per unit, then a spec-compliance
review and a code-quality review before the unit is marked done.

> **Ledger state:** marker-stage skeleton. Rows are `⏳ pending` until each unit lands. Update each row
> in the same commit as its implementation (or follow up with `docs(p1-N): record SHA`). Never fudge
> test counts. Scope changes go in *Plan adjustments* below, **not** in the frozen plan file.

---

## Plan adjustments vs. the source plan

**Kickoff posture (2026-06-02).**

1. **All atx-core upstreams have landed — Phase 1 runs fully GREEN, no `atx_engine_pending` staging.**
   The plan was written Pattern-B (blocked-on L4/L8/L3). Since then atx-core shipped, on this same
   branch, the real headers Phase 1 needs: **L8** `time` (`atx/core/datetime.hpp` — `time::Timestamp`,
   trivially-copyable i64-nanos) and `domain` (`domain/domain.hpp` — `Symbol`, `Bar{ts,o,h,l,c,vol}`,
   `Tick`, `Side`; `domain/symbol.hpp` — `SymbolTable`), **L4** `concurrent::disruptor`, **L3**
   `container/fixed_vector` + `container/hash_map`, and `hash` (`hash_bytes`/`hash_combine`). So **no
   atx-core stub is required** and every unit builds against the real, committed API. The
   `atx_engine_pending` label is therefore **not introduced** this sprint.

2. **As-built API deltas the plan must yield to (the plan is a fossil; the ledger records reality):**
   - `domain::Bar` carries a **single** `ts` field (open/close-agnostic by design), **not** a
     `close_ts`. The bitemporal `event_time`/`knowledge_time` axes therefore live on the engine
     `Event`/`MarketPayload` (P1-1/P1-2), with `event_time := bar.ts` (treated as the bar close per the
     release-at-close rule). The Bar itself stays time-convention-agnostic.
   - `domain::Symbol` is `{u32 id}` (POD, trivially-copyable) — rides the ring directly.
   - Prices/quantities are exact `Decimal` (single i64 mantissa, Rule-of-Zero) → `Bar`/`Tick` are
     trivially-copyable, so the `Event` POD union assertion holds.

3. **Worktree deviation (deliberate; agent §"deviate explicitly with reason").** The plan/ledger named a
   git worktree. The human is **actively working in this checkout on `feat/atx-core-stdlib`** and the tree
   is clean (only untracked planning docs). A separate worktree would force a fresh `build/` + a
   multi-hundred-MB FetchContent re-download onto a OneDrive-synced path (slow) and split the untracked
   ledger across trees. Instead Phase 1 is built **in-place on the current feature branch** (NOT master),
   reusing the configured `build/`, with per-unit `feat(p1-N)` commits and no push. Isolation cost is nil
   (single-threaded subagent execution; no competing branch work).

Realistic scope for this sprint:

1. **P1-0** — Module scaffold + CMake test glob (marker). Open ledger, freeze scope.
2. **P1-1** — Event taxonomy (`EventType`, cache-aligned POD `Event`). *blocked-on atx-core L8.*
3. **P1-2** — Market data records (`MarketPayload` over Bar/Tick + PIT `knowledge_ts`). *blocked-on L8.*
4. **P1-3** — Event bus (wrap `concurrent::disruptor`; SP→MC; bench + TSan). *blocked-on atx-core L4.*
5. **P1-4** — Sim clock + point-in-time visibility gate (look-ahead defense). *blocked-on L8.*
6. **P1-5** — DataHandler (`IDataHandler` + `InMemoryBarFeed`; survivorship: delisted included).
   *blocked-on atx-core L3 + L8.*
7. **P1-6** — Determinism replay harness (identical input → identical event-hash) + sprint close.

Defer (out of Phase 1 scope — see ROADMAP):

- Strategy / Portfolio / ExecutionSim / P&L / cost model → **Phase 2**.
- Alpha-research layer (columnar eval, formulaic vocab) → **Phase 3**.

---

## Per-unit ledger

| Unit | Status | Commit | Notes |
|------|--------|--------|-------|
| P1-0 | ✅ done | `cf6252d` | `fwd.hpp` (spine fwd-decls); `tests/CMakeLists.txt` → `*_test.cpp` glob + `atx_warnings` + Threads; `bench/` (`bench_main.cpp` + `*_bench.cpp` glob, `ATX_BUILD_BENCH`); engine lib links `atx_warnings`; `scaffold_test` green. No `atx_engine_pending` (all upstreams landed). |
| P1-1 | ✅ done | `554cceb`+`14b8c2e` | `event/event.hpp`: `EventType{Market,Signal,Order,Fill}` (`:u8`, closed); cache-aligned trivially-copyable `Event{knowledge_ts,event_ts,type,payload}`; payload = `std::array<std::byte,kPayloadBytes=64>` + generic memcpy accessors `store_payload<P>`/`payload_as<P>` (NO union-active-member UB). Guards `sizeof==128`/`alignof==64`/trivially-copyable. Exhaustive `to_string` no-`default`. **13 tests**, clang-tidy/format clean. |
| P1-2 | ✅ done | `1e00593`+`<polish>` | `data/market.hpp`: `MarketPayload{symbol, Kind{Bar,Tick}, delisted_final, union{Bar,Tick}}` (tagged union — required to fit 64B budget; **sizeof==56**, trivially-copyable, tag-guarded `as_bar`/`as_tick` w/ justified union-access NOLINT). `make_market_bar`/`make_market_tick` set `event_ts=ts`, enforce bitemporal `ATX_ASSERT(knowledge_ts ≥ ts)`. **15 tests** (incl. 2 EXPECT_DEATH). clang-tidy/format clean; spec ✅ + quality APPROVED. |
| P1-3 | ⏳ pending | `—` | `EventBus<Cap>` over `concurrent::disruptor`; SP→MC fanout; zero-alloc; TSan; bench ns/op. *blocked-on L4.* |
| P1-4 | ⏳ pending | `—` | `SimClock` monotonic + `is_visible(knowledge_ts)` look-ahead gate; restatement semantics. *blocked-on L8 `time`.* |
| P1-5 | ⏳ pending | `—` | `IDataHandler` + `InMemoryBarFeed`; stable `(knowledge_ts,symbol)` order; delisted-final included. *blocked-on L3+L8.* |
| P1-6 | ⏳ pending | `—` | Replay harness: identical input → identical event-hash; mutation detected. Sprint close ceremony. |

### P1-3 measured throughput

_(Fill when bench runs.)_ Record events/s and ns/op for SP→SC and SP→MC, plus host/build context
(CPU, compiler, build type, ring capacity). **Do not** cite the refuted Disruptor "52 ns/hop" figure;
report measured numbers only.

| Config | events/s | ns/op | Host / build |
|--------|----------|-------|--------------|
| SP→SC  | —        | —     | —            |
| SP→MC (2 consumers) | — | — | —          |

### Deferred residuals

_(Lift to ROADMAP future-work backlog at close.)_ None recorded yet.

---

## Phase 1 sprint commits

| Commit | Unit | Test counts (suite/total/fail/skip) |
|--------|------|-------------------------------------|
| `cf6252d` | marker (P1-0) | scaffold-only: `EngineScaffold` 1/1 pass |
| `554cceb`+`14b8c2e` | P1-1 | EventTaxonomy 13/13 pass |
| `1e00593`+`<polish>` | P1-2 | MarketData 15/15 pass |
| `—`    | P1-3 | — |
| `—`    | P1-4 | — |
| `—`    | P1-5 | — |
| `—`    | P1-6 + close | — |

**Phase 1 adds `<N>` new tests (first engine sprint; total engine footprint: `<K>`/0/0 across `<J>`
binaries).** _(Fill at close.)_

---

## What Phase 1 proves / Next sprint priorities

_(Written at close — the baton handoff.)_ Expected statement: *Phase 1 proves the engine has a
deterministic, point-in-time, survivorship-safe event spine — the same feed replays to a byte-identical
event-hash, future-dated data is provably invisible, and delisted symbols are present. Phase 2 plugs a
Strategy → Portfolio → ExecutionSim (√-impact cost) loop onto this bus to earn the first runnable,
cost-honest backtest on one alpha.*
