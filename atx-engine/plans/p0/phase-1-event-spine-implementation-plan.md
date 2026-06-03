# Phase 1 — Event Spine + Data Model — Implementation Plan

**Module:** `atx-engine` (`p0`)
**Phase theme:** A deterministic, point-in-time, **concurrency-capable and high-performance-by-default**
event spine — a Disruptor-backed bus, a bitemporal event taxonomy, market records, an event-driven
sim clock + look-ahead gate, and a DataHandler that merges N time-sorted sources into one chronological
stream (delisted symbols included). The trustworthy, fast substrate every later phase plugs into.
**Status:** frozen at kickoff (fossil — scope changes go in the ledger's *Plan adjustments*, not here).
**Authority:** [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md). Quality bar:
[`../docs/implementation-quality.md`](../docs/implementation-quality.md).
**Read first:** [`engine-architecture-audit.md`](engine-architecture-audit.md) §2–§3, §6.
**Grounding:** §2/§3/§5 below cite real open-source engines (NautilusTrader, QuantConnect LEAN, Zipline,
backtrader, LMAX Disruptor/Aeron, ArcticDB, kdb+ tickerplant). See Appendix A for the reference index.
**Upstream now available:** atx-core **L4 `concurrent::disruptor` is implemented** (SP/MP, 1..N consumers,
gating sequences, wait strategies, lock-free, cache-line-padded). Phase 1 builds directly on its real
API (§3.2). atx-core **L8 `time`/`domain` and L3 containers remain pending** (blocked-on; §6).

---

## 1. What this sprint delivers

The trustworthy, fast substrate of the engine. After Phase 1:

1. An **event taxonomy** and a cache-aligned, trivially-copyable `Event` POD that rides the Disruptor
   ring with zero per-event allocation.
2. A **bitemporal market data model** (`event_time` + `knowledge_time`, delisting, PIT universe).
3. An **EventBus** — a thin, engine-typed wrapper over atx-core's lock-free `Disruptor`, supporting a
   single producer → N gated consumers, batch draining, and two execution modes (deterministic
   single-threaded backtest / concurrent live).
4. An **event-driven SimClock** that advances to `min(next_data_ts, next_timer_ts)` and a **look-ahead
   gate** (`knowledge_time ≤ clock`).
5. A **DataHandler** that k-way-merges N time-sorted sources into one chronological, coalesced stream of
   `MarketEvent`s, releasing bars **at close (knowledge time)**, with delisted symbols present.
6. A **determinism replay harness** proving byte-identical event-hash across runs.

No strategy, portfolio, P&L, or cost model yet — those are Phase 2. This phase is pure spine + data
model, but built to production performance and concurrency standards from unit 1.

**Maps to research:** [`../../research/renaissance-worldquant-deep-dive.md`](../../research/renaissance-worldquant-deep-dive.md)
§IV (event-driven architecture, the three biases), §VIII (Disruptor), §4.4 (point-in-time data).

---

## 2. Reference architecture (open-source-grounded)

The spine is the SEDA / single-writer event-loop pattern proven by NautilusTrader (which explicitly
compares its in-process design to the LMAX Disruptor) and LEAN. The canonical shape:

```
  sources (N, each sorted by knowledge_time)          ATX EVENT SPINE (Phase 1)
  ┌───────────────┐                          ┌───────────────────────────────────────────┐
  │ InMemoryBar   │ cursor ─┐                │                                             │
  │ Feed (AAPL…)  │         │   k-way merge   │   ┌──────────┐  publish   ┌─────────────┐  │
  ├───────────────┤         ├──▶ frontier ───▶│   │ SimClock │◀──advance──│ DataHandler │  │
  │ InMemoryBar   │ cursor ─┤   min-heap by   │   │ (PIT gate)│           │ (producer)  │  │
  │ Feed (delist) │         │   knowledge_ts  │   └────┬─────┘            └──────┬──────┘  │
  ├───────────────┤         │   (LEAN frontierUtc)     │ now()                   │ Event   │
  │ … source K    │ cursor ─┘                          ▼                         ▼         │
  └───────────────┘                          gate: knowledge_ts ≤ now   ┌──────────────┐   │
                                                                        │  EventBus    │   │
                                                                        │ (Disruptor   │   │
                                                                        │  SP→N gated) │   │
                                                                        └──┬───┬───┬───┘   │
                                                       barrier-ordered fan-out│   │   │     │
                                                          ┌──────────────────▼┐ ┌▼─┐ ┌▼───┐ │
                                                          │ Strategy (P2)     │ │..│ │P/L │ │
                                                          └───────────────────┘ └──┘ └────┘ │
                                                            (Phase 1: RecordingConsumer only)│
                                                          └───────────────────────────────────┘
```

Each subsystem maps to a proven reference:

| ATX component | Reference (open-source) | What we borrow |
|---|---|---|
| `EventBus` over Disruptor | LMAX Disruptor; Aeron log buffers; kdb+ `.u` pub/sub | pre-alloc pow-2 ring, padded int64 cursor, store-release publish, gating sequences, batch drain, write-then-publish |
| `MessageBus` ergonomics | NautilusTrader `MessageBus` (`crates/common/src/msgbus`) | typed fast-path routing, deterministic handler order by priority, cache-before-publish |
| `SimClock` event-driven advance | NautilusTrader `TestClock::advance_time` (sorts `Vec<TimeEvent>` by `ts_event`); LEAN `frontierUtc`; Zipline `MinuteSimulationClock` | clock = next event's knowledge time; timers in ordered map |
| Look-ahead gate | NautilusTrader `ts_event`/`ts_init`; LEAN `BaseData.Time`/`EndTime`; Zipline `simulation_dt` closure | bar released at **close**, visibility gated on knowledge time |
| DataHandler k-way merge | LEAN `SubscriptionSynchronizer.Sync()`; backtrader `_next` min-timestamp merge | min-heap frontier merge, coalesce equal timestamps |
| Bitemporal data model | ArcticDB `as_of`/snapshots; Snodgrass bitemporal; kdb+ tickerplant log | `event_time` + `knowledge_time`, delisting dates, PIT universe intervals |

---

## 3. Concurrency & performance model (build this in from unit 1)

> This is a **performance-by-default** sprint. Every hot-path type is lock-free or single-writer,
> cache-line-aware, and zero-allocation in steady state. Concurrency-capable primitives are used even
> where the backtest runs single-threaded, so the *same* spine runs live multi-threaded later.

### 3.1 Two execution modes, one code path

The Disruptor is concurrency-capable; the backtest drives it deterministically. Both modes use the
identical `EventBus` + consumers — the difference is only who turns the crank:

- **Backtest mode (Phase 1 default): single logical thread, deterministic.** The DataHandler publishes
  the events for one timestamp slice, then the driver drains each consumer **in registration (barrier)
  order** before advancing the clock. This gives a total order on simultaneous events and byte-identical
  replays (research determinism rules; NautilusTrader single-threaded kernel). Even single-threaded, the
  Disruptor ring beats a naive `std::function` callback list: zero alloc, contiguous cache-friendly
  slots, and **batch drain** (a consumer that wakes to find the cursor N ahead processes all N in a
  tight loop without re-touching atomics).
- **Live mode (future, Phase 6+): multi-threaded.** Producer thread (DataHandler/feed) + consumer
  threads, each with its own gating sequence; the producer is gated by the slowest consumer so it never
  overwrites an unconsumed slot. Background I/O (sockets) on other threads re-inject onto the ring
  (NautilusTrader pattern).

**Determinism rules (enforced by the spine, from the research synthesis):**
1. Single logical thread owns ordering decisions in backtest mode.
2. Total order on simultaneous events: tie-break by a stable key `(knowledge_ts, source_idx, seq)` —
   never hash-map/insertion order.
3. Knowledge-time == clock-time invariant: a consumer may only see data with `knowledge_ts ≤ clock`;
   advancing the clock is the *only* way new data becomes visible.
4. Intra-timestamp settlement: after delivering a slice, drain consumer-generated commands within the
   same timestamp until quiescent (relevant Phase 2; the bus must support re-entrant publish within a
   tick — design for it now).

### 3.2 EventBus over the real atx-core Disruptor API

atx-core L4 ships this (as-built signatures — use them verbatim):

```cpp
namespace atx::core::concurrent {
enum class ProducerKind : u8 { Single, Multi };
struct SpinYieldWait; struct BusySpinWait; struct YieldWait;   // wait policies
inline constexpr i64 kInitialSequence = -1;

template <class Event, usize Capacity,
          ProducerKind Producer = ProducerKind::Single,
          usize ConsumerCount   = 1,
          class WaitStrategy     = SpinYieldWait>
class Disruptor {
  static constexpr usize capacity() noexcept;            // Capacity rounded up to pow2
  static constexpr usize consumer_count() noexcept;
  [[nodiscard]] i64 claim() noexcept;                    // SP: CAS-free; MP: fetch_add
  Event& at(i64 seq) noexcept; const Event& at(i64) const noexcept;
  void publish(i64 seq) noexcept;                        // store-release
  [[nodiscard]] i64 published_sequence() const noexcept; // non-blocking highest published
  i64 wait_for(i64 seq) noexcept;  i64 wait_for(i64 seq, usize c) noexcept;   // returns HIGHEST avail
  [[nodiscard]] i64 try_wait_for(i64 seq) const noexcept;
  void consumed(i64 seq) noexcept;  void consumed(i64 seq, usize c) noexcept;
  [[nodiscard]] i64 consumer_sequence(usize c) const noexcept;
  class ConsumerBarrier { i64 wait_for(i64); void consumed(i64); i64 sequence() const; usize index() const; };
  ConsumerBarrier consumer(usize c) noexcept;
};
}
```

The engine `EventBus` (P1-3) is a **thin, engine-typed facade** — it does NOT reinvent the ring:

```cpp
namespace atx::engine {
// Capacity, consumer count, producer mode are engine-config compile-time params.
template <usize Capacity = 1u << 16, usize ConsumerCount = 1,
          atx::core::concurrent::ProducerKind Producer = atx::core::concurrent::ProducerKind::Single>
class EventBus {
 public:
  // Producer (DataHandler) — claim a slot, fill in place (zero-copy), publish.
  [[nodiscard]] event::Event& claim_slot(i64& out_seq) noexcept;   // = at(claim())
  void publish(i64 seq) noexcept;
  // Consumer registration -> a typed handle around ConsumerBarrier.
  [[nodiscard]] Consumer add_consumer(usize index) noexcept;       // index < ConsumerCount
  // Backtest driver: drain all consumers up to published_sequence() in barrier order.
  template <class Fn> void drain_in_order(Fn&& dispatch) noexcept;  // single-threaded mode
 private:
  atx::core::concurrent::Disruptor<event::Event, Capacity, Producer, ConsumerCount,
                                   atx::core::concurrent::SpinYieldWait> ring_{};
};
}
```

Design notes:
- **Default capacity `2^16` slots** of `Event` — sized so a full trading day of daily bars for a few
  thousand symbols never wraps within one timestamp slice; tune per the perf budget (§3.4). Power-of-two
  is enforced by the Disruptor.
- **Wait strategy:** `SpinYieldWait` default (good for live); backtest single-threaded mode never blocks
  (it only drains up to `published_sequence()`), so the wait strategy is irrelevant on that path —
  document it.
- **Consumer dependency ordering** (Strategy → Portfolio → Execution) in backtest mode is achieved
  trivially by `drain_in_order` calling consumers in registration order; the Disruptor's cross-consumer
  gating DAG is reserved for live mode (note as future, do not build the DAG in Phase 1).

### 3.3 Performance-by-default principles (every unit obeys)

- **Zero allocation on the hot path.** Events are pre-allocated ring slots, filled in place (Aeron
  `tryClaim` zero-copy model). No `std::function`, no `std::any`, no `std::variant` heap, no `shared_ptr`
  in the per-event path. Use atx-core `fixed_vector`/`object_pool` for any per-tick scratch.
- **`Event` is a trivially-copyable POD**, `≤ 1` cache line where achievable, `ATX_CACHE_ALIGNED`,
  `static_assert(std::is_trivially_copyable_v<Event>)`. A tagged POD union beats `std::variant` for
  ring storage (no indirect dispatch, no non-trivial special members).
- **int64-nanos time everywhere** (`atx::core::time::Timestamp`), two semantic fields per market event:
  `event_time` and `knowledge_time`. No `double` seconds, no wall-clock objects on the hot path.
- **Cache-line discipline.** Producer cursor, each consumer sequence, and the gate cache are already
  padded inside the Disruptor; the engine adds no falsely-shared shared state. Keep per-consumer mutable
  state on its own line.
- **Branch-light, batch-friendly.** Consumers drain batches (`wait_for` returns the highest available);
  the merge coalesces equal-timestamp events so the dispatch loop is tight.
- **`ATX_RESTRICT` / `prefetch`** on the columnar source-cursor scans (the merge reads the next row of
  each source; prefetch the next).
- **Measure, don't assume.** P1-3 and P1-5 ship Google Benchmarks with ns/op + host/build recorded
  (agent.md perf gate). Do not cite vendor figures; report measured numbers.

### 3.4 Performance budget (targets; validate in P1-3/P1-6 bench)

| Path | Target (daily-bar backtest, single thread) | Why |
|---|---|---|
| EventBus publish (claim+fill+publish) | ≤ ~20 ns/event | as-built Disruptor SP→SC ≈ 15.6 ns/op measured; engine wrapper adds ~nothing |
| Single-consumer drain dispatch | ≤ ~25 ns/event amortized | batch drain over contiguous slots |
| k-way merge per emitted event | ≤ ~log₂(N) heap ops + 1 cmp | N = source count; heap holds only N entries |
| Steady-state allocations per event | **0** | pre-allocated ring + source cursors |
| Replay of 1 symbol-decade of daily bars (~2,520 bars) | < a few ms | sanity ceiling; refine when real data lands |

These are ceilings to design against, not acceptance gates for Phase 1 (real-data benches come in
Phase 2+). Record actuals in the ledger.

---

## 4. Bitemporal data model (kills look-ahead + survivorship structurally)

Two orthogonal time axes (Snodgrass bitemporal; ArcticDB `as_of`; Zipline `dt < adj_dt ≤ perspective_dt`;
LEAN `EmitTimeUtc`):

- **`event_time`** (int64 nanos) — when the fact was true in the market. For a bar this is its
  **close/`EndTime`**, not its open (LEAN `BaseData.EndTime`; NautilusTrader bar `ts_event`). Sort/merge
  axis.
- **`knowledge_time`** (int64 nanos) — when the system *could have known* it (ingest/publish; NautilusTrader
  `ts_init`). The **clock-gating axis**: a record is visible iff `knowledge_time ≤ clock`. For a normal
  historical bar, `knowledge_time = event_time` (known at close). The field exists so late/restated data
  is modelled honestly.

**The #1 anti-look-ahead rule (from LEAN + NautilusTrader): release a bar at its close, gated on
`knowledge_time`, never at its open.** This single convention prevents the classic "bar stamped at open
but containing the close" leak.

**Per-record fields the data model carries (P1-2), even if Phase 1 only populates a subset:**

| Field | Type | Purpose | Reference |
|---|---|---|---|
| `event_time` | `time::Timestamp` | true-at time (bar close) | LEAN `EndTime` |
| `knowledge_time` | `time::Timestamp` | knowable-at; clock gate | NautilusTrader `ts_init` |
| `symbol` | `domain::Symbol` | interned id | LEAN map files / CRSP PERMNO |
| `bar` / `tick` | `domain::Bar`/`Tick` | OHLCV / trade | — |
| `delisted_final` | `bool` | last bar before delist (kept in feed) | survivorship fix |

**Deferred to Phase 3/5 (designed-for, not built now):** point-in-time universe-membership intervals
`{symbol, member_from_ts, member_to_ts}`; corporate-action records with `announce_ts`/`ex_date` applied
only when `action_knowledge_ts ≤ perspective_dt` (keep raw prices + factor series, adjust on read);
symbol↔permanent-id mapping over time; restatement/correction versioning (ArcticDB version chain). The
P1-2 record layout must leave room for these without a breaking change — note it in the header.

---

## 5. The time-slice merge algorithm (DataHandler core, P1-5)

Merge N sources, each sorted ascending by `knowledge_time`, into one chronological, coalesced stream —
the no-look-ahead k-way merge (LEAN `SubscriptionSynchronizer.Sync()`; backtrader `_next`). The heap
holds only one entry per source (N entries), so it is tiny and hot.

```text
# Each source = a cursor over a knowledge_time-sorted column of rows.
# heap entries: (knowledge_ts: i64, source_idx: u32)  -- min-heap by knowledge_ts, tie-break source_idx
min_heap heap
for s in sources:
    if s.move_next():
        heap.push( {s.current.knowledge_time, s.index} )

while not heap.empty():
    frontier := heap.top().knowledge_ts           # earliest pending knowledge time
    clock.advance_to(frontier)                     # event-driven clock: jump to next knowledge time
    # drain EVERY source whose head == frontier  -> coalesce simultaneous events into one slice
    while not heap.empty() and heap.top().knowledge_ts == frontier:
        idx := heap.pop().source_idx
        s   := sources[idx]
        seq := bus.claim()                         # reserve ring slot
        bus.at(seq) = make_market_event(s.current) # fill in place (zero-copy)
        bus.publish(seq)                           # store-release
        if s.move_next():                          # re-arm with this source's next head
            ATX_ASSERT(s.current.knowledge_time >= frontier)   # monotonicity (no look-ahead)
            heap.push( {s.current.knowledge_time, s.index} )
    # backtest mode: drain consumers in barrier order for THIS slice before next frontier
    bus.drain_in_order(dispatch)
```

Invariants (assert them):
- Never touch a row whose `knowledge_time > frontier` (the heap guarantees you only see heads).
- Each source is pre-sorted by `knowledge_time`; assert monotonicity on `move_next` (NautilusTrader
  raises on unsorted — we `ATX_ASSERT` / fail-closed).
- Equal-timestamp events are coalesced into one slice and published before any consumer reacts (LEAN
  `frontierUtc`; Zipline single `dt`).
- C++: `std::priority_queue` of `{i64 ts, u32 idx}` is the baseline; a 4-ary or intrusive heap over an
  atx-core `fixed_vector` is the cache-friendly upgrade (note as an optional optimization; baseline first).

---

## 6. Dependency posture (Pattern B — blocked-on atx-core)

Written against the full atx-core design contracts (stdlib spec §6). **L4 `disruptor` is now built**
(unblocks P1-3's core dependency). Still pending: **L8** (`time`, `domain`), **L3** (`fixed_vector`,
`hash_map`). Workflow per unit:
1. Write the engine header + **failing TDD tests** against the atx-core contract now (red).
2. If a depended-on layer is unbuilt, build the test target but exclude it from the green-gate behind
   the CMake label `atx_engine_pending`; ledger row marks `blocked-on Lx`.
3. When atx-core Lx merges, flip to the gated build, watch red→green, commit `feat(p1-N)`.

A residual is filed against the atx-core build so its agent sees engine Phase 1 waits on L8 + L3.
(ROADMAP cross-module table is canonical.) **Updated blocked-on map:**

| Unit | Blocked-on | Today |
|---|---|---|
| P1-1, P1-2, P1-4, P1-5 | L8 `time`, `domain` | ⏳ |
| P1-3 | ~~L4 `disruptor`~~ | ✅ **built** |
| P1-5 | L3 `fixed_vector` (+ `hash_map`), L8 | ⏳ |

P1-3 can now go green as soon as the engine `Event` type (P1-1) exists — its only hard upstream
(`disruptor`) is shipped.

---

## 7. Cross-cutting gates (agent.md — every unit)

- **TDD:** failing GoogleTest first; one behavior per `TEST`; name `Subject_Condition_ExpectedResult`;
  cover happy path, **boundaries** (0/1/max/empty), error paths, invariant violations (`EXPECT_DEATH`).
- **Warnings = errors:** `/W4 /permissive- /WX` (MSVC/clang-cl) / `-Wall -Wextra -Wpedantic -Wconversion
  -Wshadow -Werror` (Clang). clang-tidy scope `cppcoreguidelines-*,bugprone-*,cert-*,misc-*,performance-*`
  + clang-format clean. (The `atx_warnings` interface target already carries the flags.)
- **Sanitizers:** ASan + UBSan on every test run. **TSan on the bus unit (P1-3) where a TSan toolchain
  is available** — note: clang-cl on Windows ships no usable TSan (same caveat as the atx-core disruptor);
  the multi-threaded tests are written race-clean (join before assert) and a Linux/clang TSan pass is a
  tracked residual.
- **API discipline:** `const`/`constexpr`/`noexcept`/`[[nodiscard]]` where they hold; `Result<T>`/`Status`
  for expected failures, no throw for control flow; weakest sufficient type at interfaces
  (`span`/`string_view`/concepts); no owning raw pointers; Rule of Zero; exhaustive `enum class` switches;
  functions ≤ ~60 lines; `// SAFETY:` on every deviation (union access, memory ordering, reinterpret).
- **Zero hot-path allocation** in the bus/merge/replay path (§3.3).

---

## 8. Unit decomposition (P1-0 … P1-6)

Dispatch **sequential** (each consumes the prior's headers). TDD red→green→refactor within each.

---

### P1-0 — Module scaffold + CMake test glob  *(marker commit)*

**Scope.** Create the `atx::engine` tree and wire the build so new `*_test.cpp` (and `*_bench.cpp`) are
auto-discovered. Open the Phase 1 ledger and freeze scope. No engine logic.

**Deliverables.**
- `include/atx/engine/` with `fwd.hpp` (namespace doc comment + forward decls for `event::Event`,
  `data::MarketPayload`, `SimClock`, `EventBus`, `IDataHandler`).
- `tests/CMakeLists.txt` glob/list for `tests/*_test.cpp` (mirror atx-core); a trivial `scaffold_test.cpp`
  proves GTest link.
- `bench/` with `bench_main.cpp` (`BENCHMARK_MAIN();`) + `CMakeLists.txt`, gated on `ATX_BUILD_BENCH`
  (mirror atx-core/bench). Engine bus/merge benches land here.
- CMake label `atx_engine_pending` for blocked targets.

**Acceptance.** `cmake --build` green; `scaffold_test` passes; ledger marker commit
`docs(p1-0): open phase-1 sprint ledger` with `Base: master @ <SHA>`.

---

### P1-1 — Event taxonomy  *(blocked-on atx-core L8 `time`)*

**Scope.** The event vocabulary every bus message uses: a closed, exhaustive `EventType` and a compact,
cache-aligned, trivially-copyable `Event` POD carrying a timestamp and a tagged payload. Designed so
Phases 2+ add payload variants, not new mechanisms (NautilusTrader's typed messages; LMAX in-place ring
entries).

**Data structure (the hot-path object — get this right).**
```cpp
namespace atx::engine::event {
enum class EventType : u8 { Market, Signal, Order, Fill };   // closed; exhaustive switches, no default

struct ATX_CACHE_ALIGNED Event {                 // trivially copyable; rides the Disruptor ring
  atx::core::time::Timestamp knowledge_ts{};     // clock-gating axis (sort/visibility)
  atx::core::time::Timestamp event_ts{};         // true-at axis (bar close)
  EventType                  type{};
  // payload: a tagged POD union, filled by typed makers. Phase 1 populates Market (P1-2);
  // Signal/Order/Fill payloads land in Phase 2. Sized to the largest Phase-2 payload, documented.
  alignas(8) std::byte       payload[kPayloadBytes]{};   // SAFETY: access only via tag==type
  [[nodiscard]] constexpr EventType kind() const noexcept { return type; }
};
static_assert(std::is_trivially_copyable_v<Event>);
// static_assert(sizeof(Event) <= 64) if achievable; else document the chosen size + alignment.
}
```

**Why POD union over `std::variant`:** trivial-copyability (ring slot memcpy), no indirect dispatch, no
non-trivial special members — the LMAX "mutate pre-allocated entry in place" model. Every union access
carries `// SAFETY: tag == type`.

**Tests.** type-tag round-trip; exhaustive-switch compiles (no `default`); `is_trivially_copyable`;
size/alignment `static_assert`s; `knowledge_ts`/`event_ts` ordering comparators; default-constructed
event well-formed. Boundary: `knowledge_ts == event_ts`; epoch-zero.

**Perf note.** No methods allocate; `Event` is memcpy-able. Confirm `alignof(Event) == 64`.

**Acceptance.** `/W4 /WX` clean; tests green under ASan/UBSan (or `blocked-on L8` staged red).
Commit `feat(p1-1): event taxonomy`.

---

### P1-2 — Market data records (bitemporal)  *(blocked-on atx-core L8 `domain`)*

**Scope.** The market payload: wrap atx-core `domain::Bar`/`Tick` into a `MarketPayload` carrying
`symbol`, the bitemporal `event_time`/`knowledge_time`, and the `delisted_final` survivorship flag.
Layout leaves room for the Phase-3/5 PIT fields (§4) without a breaking change.

**Data structure.**
```cpp
namespace atx::engine::data {
struct MarketPayload {                                  // stored inside Event::payload (POD)
  atx::core::domain::Symbol  symbol{};
  enum class Kind : u8 { Bar, Tick } kind{};
  bool                       delisted_final{false};     // survivorship: last bar before delist
  union { atx::core::domain::Bar bar; atx::core::domain::Tick tick; };   // SAFETY: tag = kind
};
// Typed maker: knowledge_time defaults to the bar close (event_time); override for late/restated data.
[[nodiscard]] event::Event make_market_bar(atx::core::domain::Symbol,
                                           const atx::core::domain::Bar&,
                                           atx::core::time::Timestamp knowledge_ts /* = bar close */,
                                           bool delisted_final = false) noexcept;
}
```

**Contract (document in header).** `knowledge_time ≥ event_time` (a fact cannot be known before it is
true) — `ATX_ASSERT` in debug. Default `knowledge_time = event_time = bar.close_ts` (the release-at-close
rule, §4). Strong types only across the boundary (no raw `double`).

**Tests.** bar/tick construction; `knowledge_ts ≥ event_ts` invariant (`EXPECT_DEATH` on violation);
delisted-final flag round-trips; payload tag matches accessor; price/qty are strong types. Boundary:
zero-volume bar; `knowledge_ts == event_ts`; a late bar (`knowledge_ts > event_ts`).

**Acceptance.** As P1-1. Commit `feat(p1-2): bitemporal market data records`.

---

### P1-3 — EventBus over the Disruptor  *(upstream L4 BUILT; bench-gated; TSan where available)*

**Scope.** A thin engine facade over atx-core `concurrent::Disruptor` (§3.2). Single producer
(DataHandler) → N gated consumers; zero-copy claim-fill-publish; batch drain; the deterministic
`drain_in_order` backtest driver. **No reinvention** of the ring.

**Implementation shape.** `claim_slot` = `at(claim())`; `publish` forwards; `add_consumer(i)` returns a
`Consumer` wrapping `Disruptor::consumer(i)`; `drain_in_order` reads `published_sequence()` once and, for
each registered consumer in order, drains from its sequence+1 to the published cursor, invoking the
dispatch callback per event, then `consumed(highest)`. Write-then-publish discipline (kdb+ `.u.upd`):
fill the slot fully before `publish` (the store-release makes it visible).

**Tests (multi-thread where applicable).**
- SP→SC FIFO ordering of large N (seed-style).
- SP→multi-consumer: every consumer observes every event in order; `drain_in_order` invokes consumers in
  registration order (deterministic).
- Zero-allocation on the publish/drain path (instrument with a counting allocator or a no-alloc guard).
- Backpressure/wrap: small capacity; in single-threaded drive, `drain_in_order` between slices prevents
  overrun — assert no slot is overwritten before consumed (checksum/per-slot tag).
- Batch drain: a multi-event slice drains in one `drain_in_order` pass.
- Boundary: capacity rounding (pow2), single-consumer, capacity-1 ring.

**Bench.** `disruptor`-style: EventBus publish ns/op (SP→SC), drain dispatch ns/op, SP→2-consumer; record
host/build/capacity. Target ≤ ~20 ns publish (§3.4). Report measured only.

**Acceptance.** `/W4 /WX` clean; tests green (ASan/UBSan; TSan where toolchain supports); bench recorded.
Commit `feat(p1-3): event bus over disruptor`.

---

### P1-4 — SimClock + point-in-time gate  *(blocked-on atx-core L8 `time`)*

**Scope.** The look-ahead defense + the event-driven clock. A monotonic sim clock that **advances to the
next event's knowledge time** (NautilusTrader `advance_time`; LEAN `frontierUtc`; Zipline pre-grid as the
alternative model) and a gate making a record visible iff `knowledge_time ≤ clock`.

**Data structure.**
```cpp
namespace atx::engine {
class SimClock {
 public:
  [[nodiscard]] atx::core::time::Timestamp now() const noexcept { return now_; }
  void advance_to(atx::core::time::Timestamp t) noexcept;          // ATX_ASSERT(t >= now_) monotonic
  [[nodiscard]] bool is_visible(atx::core::time::Timestamp knowledge_ts) const noexcept {
    return knowledge_ts <= now_;                                   // the gate
  }
  // Event-driven advance helper: next clock = min(next_data_ts, next_timer_ts). Timers (bar closes,
  // timeouts) added in Phase 2; Phase 1 advances purely on data knowledge_ts (the merge drives it).
 private:
  atx::core::time::Timestamp now_{};
};
}
```

**Contract.** Advance is **monotonic** (assert non-decreasing; the DataHandler is the sole advancer in
Phase 1). Restatement semantics: when two records share `event_time` but differ in `knowledge_time`, the
gate admits only the one knowable as of `now()` — original value during the window before the restatement's
knowledge time (ArcticDB `as_of`; Zipline `dt < adj_dt ≤ perspective_dt`). Document this.

**Tests.** monotonic advance (`EXPECT_DEATH` on backward advance); future-dated record invisible; visible
exactly at `knowledge_ts == now`; restatement window returns original then restated value after its
knowledge time. Boundary: equal timestamps; epoch-zero.

**Acceptance.** As above. Commit `feat(p1-4): sim clock + point-in-time gate`.

---

### P1-5 — DataHandler (k-way merge feed)  *(blocked-on atx-core L3 + L8)*

**Scope.** The feed. An abstract `IDataHandler` and an in-memory implementation that **k-way-merges N
knowledge_time-sorted sources** into one chronological, coalesced stream (§5), advances the `SimClock`,
and publishes `MarketEvent`s onto the bus. **Survivorship defense:** delisted symbols included with their
final bar (`delisted_final`). This is the LEAN `SubscriptionSynchronizer` / backtrader `_next` pattern.

**Data structures.**
```cpp
namespace atx::engine::data {
class IDataHandler {
 public:
  virtual ~IDataHandler() = default;
  [[nodiscard]] virtual bool step() = 0;   // advance one coalesced timestamp slice; false at EOF
};

// One source = a cursor over a knowledge_time-sorted span of rows.
struct BarRow { atx::core::domain::Symbol symbol; atx::core::domain::Bar bar;
                atx::core::time::Timestamp knowledge_ts; bool delisted_final{false}; };

class InMemoryBarFeed final : public IDataHandler {
 public:
  // sources: each a span pre-sorted ascending by knowledge_ts. Heap holds one head per source.
  InMemoryBarFeed(std::span<const std::span<const BarRow>> sources,
                  SimClock& clock, EventBus<>& bus) noexcept;
  [[nodiscard]] bool step() override;      // §5 frontier merge: drain all heads == frontier, publish
 private:
  // min-heap of {i64 knowledge_ts, u32 source_idx}; tie-break source_idx for determinism.
  // baseline std::priority_queue; optional 4-ary heap over fixed_vector for cache locality.
};
}
```

**Contract.** Emits in stable `(knowledge_ts, source_idx)` order; coalesces equal timestamps into one
slice; advances the clock to each frontier *before* publishing that slice (so consumers see
`knowledge_ts ≤ now`). Asserts source monotonicity on advance (fail-closed on unsorted, NautilusTrader
behavior). Does **not** filter symbols by present-day existence — delisted symbols carry a final bar.

**Tests.** chronological emission (knowledge_ts non-decreasing); coalescing (two sources, same ts → one
slice, both events before any consumer reaction); no look-ahead (consumer never sees `knowledge_ts >
clock.now()`); delisted symbol present with final bar; tie-break order stable; monotonicity assert fires
on unsorted source; empty feed → `step()` false immediately; deterministic across two constructions.
Boundary: single source; single row; all rows same timestamp; N sources interleaved.

**Perf.** Heap ops ≤ ~log₂(N) per event; prefetch next row per source; zero alloc after construction.
Optional bench: merge throughput (events/s) over synthetic N-source data.

**Acceptance.** As above. Commit `feat(p1-5): k-way-merge in-memory DataHandler`.

---

### P1-6 — Determinism replay harness + sprint close

**Scope.** The proof. An integration test replaying a fixed multi-source feed through the merge → bus →
a `RecordingConsumer` **twice**, asserting a **byte-identical event-hash** (research §8.2; determinism
rules §3.1). Then the sprint-close ceremony.

**Design.** `RecordingConsumer` folds a rolling hash (atx-core `hash::hash_combine`/wyhash) over the
ordered `(seq, type, knowledge_ts, event_ts, symbol, payload-bytes)` stream. The harness runs the full
`InMemoryBarFeed(N sources) → EventBus → RecordingConsumer` path twice on identical input and asserts
equal final hashes; then asserts a **mutated** input (reordered tie, changed value, added late bar)
yields a **different** hash (the test is not vacuously passing).

**Tests.** identical-input → identical-hash; mutated-input → different-hash; multi-consumer hashes agree
on order; coalesced-slice order is stable across runs. This is the Phase-1 **exit-criteria evidence**.

**Sprint close (sprint.md ceremony).**
1. Lift residuals into ROADMAP future-work backlog (TSan-on-Linux pass; PIT universe/corporate-actions
   designed-not-built; 4-ary heap optimization).
2. Update ROADMAP Phase 1 status table (`⏳ → ✅ <sha>` / `⚠️ partial`).
3. Bump ROADMAP `Last reviewed`.
4. Write "What Phase 1 proves / Next sprint priorities" in the ledger (baton to Phase 2).
5. Write `phase-1.md` user reference (public bus/clock/DataHandler API + the determinism + no-look-ahead
   + survivorship guarantees).
6. Merge worktree → master (`--no-ff`, `merge: phase-1 — event spine`). **Do not push unless asked.**

**Acceptance.** Replay harness green; close items 1–6 done; ledger + ROADMAP reconciled.
Commit `docs(p1-close): close phase-1 — 6 units, <M> tests`.

---

## 9. Sub-agent dispatch checklist (per unit)

Each brief includes (sprint.md): worktree path + branch (`worktree-phase-1-event-spine`); the unit's
scope **quoted** from this plan; acceptance criteria; the verbatim *"Marker-commit pattern is mandatory:
commit before stopping or work is lost."*; commit format `feat(p1-N): <summary>`; predecessor SHAs; the
ledger-row instructions (status, test counts, blocked-on note); and the handoff block below with the
atx-core headers (`error.hpp`, `types.hpp`, `platform.hpp`, **the as-built `concurrent/disruptor.hpp`**)
as the positive style + API reference.

### Handoff block (paste into every coding sub-agent brief)
```text
Implementation quality standard (atx):
Governed by .agents/cpp/agent.md (safety-critical-grade C++20). Use atx-core public headers
(error.hpp, types.hpp, platform.hpp, concurrent/disruptor.hpp) as the positive style + API reference:
module-level intent comment, grouped types/APIs, explicit ownership/lifetime/error contracts, concise
comments that explain invariants and non-obvious control flow — never narrate code.

Performance is a first-class requirement. Zero allocation on the hot path; trivially-copyable POD events
in pre-allocated ring slots (fill in place, no std::function/variant/shared_ptr per event); int64-nanos
time; cache-line alignment (ATX_CACHE_ALIGNED) for shared/hot state; batch-friendly, branch-light loops.

No UB, no narrowing, no uninitialized vars, no owning raw pointers. const/constexpr/noexcept/[[nodiscard]]
where they hold. Expected failures -> Result<T>/Status, never throw for control flow. Weakest sufficient
type at interfaces (span/string_view/concepts). Every enum-class switch exhaustive; loops bounded;
functions <= ~60 lines. // SAFETY: on every deviation (union access, memory ordering, reinterpret_cast).

TDD: failing GoogleTest first, watch it fail for the right reason, then implement. Cover happy path,
boundaries (0/1/max/empty), error paths, invariant violations (EXPECT_DEATH for documented preconditions).
Build /W4 /WX clean; clang-tidy(cppcoreguidelines/bugprone/cert/misc/performance)/format clean; tests pass
under ASan+UBSan (TSan for the bus where the toolchain supports it). Zero allocation on the bus/merge/
replay hot path.

A unit is done only when header + impl + tests + ledger row + build/test gate are all present. No TODO
stubs, no fake success paths, no untested skeletons. Prefer a smaller complete slice over a larger
partial one.
```

---

## 10. Risks / watch-items

- **LSP false positives** without `compile_commands.json` — verify against the real `cmake --build`,
  ignore phantom missing-include/clang-tidy-out-of-policy noise (sprint.md lesson; the atx-core disruptor
  saw exactly this).
- **atx-core L8/L3 contract drift** — if `time`/`domain`/containers ship with signatures differing from
  stdlib-spec §6, update engine headers to the *as-built* API and note the delta in the ledger row (the
  plan is a fossil; the ledger records reality). The disruptor already proved the spec maps cleanly.
- **POD union vs `std::variant` for `Event`** — defaulting to POD union for trivial-copyability and the
  ring's in-place memcpy model; if the `// SAFETY:` burden grows, revisit in P1-1 and record in the ledger.
- **TSan unavailable on clang-cl/Windows** — bus concurrency tests are race-clean by construction; a
  Linux/clang TSan pass is a tracked residual (same as the atx-core disruptor).
- **`Event` payload sizing** — fix `kPayloadBytes` to the largest Phase-2 payload now (document the
  budget) so the ring slot size is stable across phases; growing it later reshapes the ring.
- **Concurrency by default vs determinism** — the spine is concurrency-capable (Disruptor) but the
  backtest drives it single-threaded for byte-identical replays. Do not introduce real consumer threads in
  Phase 1; that's live-mode (Phase 6+).

## 11. Definition of done (Phase 1)

- P1-0..P1-6 implemented; tests pass under ASan+UBSan (TSan for P1-3 where supported) — or blocked-on
  units staged red behind `atx_engine_pending` with the ROADMAP cross-module table current.
- `/W4 /permissive- /WX` clean; clang-tidy(scoped)/format clean.
- EventBus + merge benches recorded (ns/op + host/build); publish within the §3.4 budget or the variance
  explained.
- Determinism replay harness (P1-6) green: identical input → identical event-hash; mutation detected.
- Look-ahead (P1-4 gate + release-at-close), survivorship (P1-5 delisted feed), and coalesced-slice
  ordering proven by test.
- Zero steady-state allocation on the bus/merge/replay path (instrumented).
- Ledger closed; ROADMAP status + `Last reviewed` updated; `phase-1.md` written; worktree merged
  `--no-ff` (not pushed unless asked).

---

## Appendix A — Open-source reference index

| System | What we took | Primary source |
|---|---|---|
| LMAX Disruptor | pre-alloc pow-2 ring, padded int64 cursor, store-release publish, gating sequences, batch drain, wait strategies | lmax-exchange.github.io/disruptor/disruptor.html |
| Aeron | zero-copy `tryClaim` (fill-in-place), pre-allocated log buffers, lock-free SPSC/MPSC | aeron.io/docs |
| NautilusTrader | `MessageBus` (typed routing, deterministic handler order), `TestClock::advance_time` (sort by `ts_event`), `ts_event`/`ts_init`, single-threaded kernel, cache-before-publish | github.com/nautechsystems/nautilus_trader `crates/common/src/{msgbus,clock}` |
| QuantConnect LEAN | `SubscriptionSynchronizer.Sync()` frontier k-way merge, `TimeSlice`, `BaseData.Time`/`EndTime` release-at-close | github.com/QuantConnect/Lean `Engine/DataFeeds/*` |
| Zipline | `MinuteSimulationClock` event grid, `BarData` `simulation_dt` closure, `dt < adj_dt ≤ perspective_dt` PIT adjustments | github.com/quantopian/zipline `zipline/gens/*`, `data/data_portal.py` |
| backtrader | min-timestamp k-way feed merge (`_next`), runonce vs runnext | github.com/mementum/backtrader `cerebro.py` |
| ArcticDB | bitemporal `as_of` read, snapshots for PIT universe, immutable version chain | arcticdb.io, docs.arcticdb.io |
| kdb+ tickerplant | `.u.upd` write-then-publish, `.u.sub`/`.u.pub` fan-out, batch vs zero-latency, log-replay decoupled | code.kx.com/q/architecture/tickq |
| Snodgrass bitemporal | event-time vs knowledge-time formal model | Developing Time-Oriented Database Applications in SQL |
