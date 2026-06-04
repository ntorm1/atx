# atx-engine Phase 1 — Event Spine (User Reference)

**Status:** ✅ shipped 2026-06-03 · `feat/atx-core-stdlib` · 69 tests, 0 fail.
**Plan:** [`phase-1-event-spine-implementation-plan.md`](phase-1-event-spine-implementation-plan.md) ·
**Ledger:** [`phase-1-progress.md`](phase-1-progress.md).

Phase 1 is the deterministic, point-in-time, survivorship-safe substrate every later phase plugs into.
It has **no** strategy, portfolio, P&L, or cost model yet (those are Phase 2). What it gives you: a typed
event taxonomy, a bitemporal market record, a lock-free bus, a look-ahead-proof clock, and a k-way-merge
feed — wired so the same feed always replays to a byte-identical event-hash.

---

## The three guarantees (each is a test, not a hope)

1. **Determinism.** Replaying a fixed feed twice yields a **byte-identical event-hash**
   (`RecordingConsumer` folds an order-sensitive digest). Three input mutations (reordered tie, changed
   value, added late bar) each change the hash — the proof is non-vacuous.
2. **No look-ahead.** A consumer can only ever see a record whose `knowledge_time ≤ clock.now()`. The
   feed advances the clock to each frontier **before** publishing that slice; the `SimClock` gate admits
   nothing from the future. Decision-at-`t` can never read data knowable only at `t+ε`.
3. **No survivorship bias.** Delisted symbols are carried through with their final bar
   (`delisted_final = true`); the feed never filters by present-day existence.

---

## Public API

All under namespace `atx::engine` (sub-namespaces `event`, `data`).

### `event::Event` — the hot-path ring message  (`event/event.hpp`)
```cpp
enum class EventType : u8 { Market, Signal, Order, Fill };   // closed; Signal/Order/Fill are Phase 2
struct Event {                                  // cache-aligned, trivially-copyable POD (sizeof 128)
  time::Timestamp knowledge_ts;                 // clock-gating axis (when it became knowable)
  time::Timestamp event_ts;                     // true-at axis (the bar's ts / close)
  EventType       type;
  std::array<std::byte, kPayloadBytes> payload; // kPayloadBytes == 64; tagged by `type`
  EventType kind() const;
  template <class P> void store_payload(const P&);   // P trivially-copyable, sizeof(P) ≤ kPayloadBytes
  template <class P> P    payload_as() const;        // memcpy out — no union-active-member UB
};
```

### `data::MarketPayload` + makers  (`data/market.hpp`)
```cpp
struct MarketPayload {                           // rides inside Event::payload (sizeof 56)
  domain::Symbol symbol;
  enum class Kind : u8 { Bar, Tick } kind;
  bool delisted_final;
  /* tagged union { domain::Bar; domain::Tick } */
  const domain::Bar&  as_bar()  const;           // PRECONDITION kind == Bar  (ATX_ASSERT)
  const domain::Tick& as_tick() const;           // PRECONDITION kind == Tick
};
// knowledge_ts ≥ bar.ts is enforced (a fact can't be known before it is true).
Event make_market_bar (Symbol, const Bar&,  Timestamp knowledge_ts, bool delisted_final = false);
Event make_market_tick(Symbol, const Tick&, Timestamp knowledge_ts, bool delisted_final = false);
```

### `EventBus<Capacity, ConsumerCount, Producer>` — thin Disruptor facade  (`bus/event_bus.hpp`)
```cpp
template <usize Capacity = 1u<<16, usize ConsumerCount = 1,
          core::concurrent::ProducerKind Producer = Single>
class EventBus {
  Event& claim_slot(i64& out_seq);               // zero-copy: fill the returned slot in place
  void   publish(i64 seq);                        // store-release; write the slot fully BEFORE publish
  Consumer add_consumer(usize index);             // index < ConsumerCount
  template <class Fn> void drain_in_order(const Fn& dispatch);  // dispatch: void(usize consumer, const Event&)
};
```
- **Backtest driver:** `drain_in_order` reads the published cursor once and drains each registered
  consumer in **registration order** — a total order on simultaneous events, byte-identical replays.
- Zero allocation and zero copy on the publish/drain path. The default ring is ~8 MB
  (65536 × 128 B) — **heap-allocate the bus** (`std::make_unique`) rather than stack it.

### `SimClock` — event-driven clock + look-ahead gate  (`clock/sim_clock.hpp`)
```cpp
class SimClock {
  constexpr Timestamp now() const;
  void      advance_to(Timestamp t);              // monotonic: ATX_ASSERT(t ≥ now())
  constexpr bool is_visible(Timestamp knowledge_ts) const;  // knowledge_ts ≤ now()
};
```

### `data::IDataHandler` / `InMemoryBarFeed` — the k-way merge feed  (`data/data_handler.hpp`)
```cpp
struct BarRow { domain::Symbol symbol; domain::Bar bar; time::Timestamp knowledge_ts; bool delisted_final = false; };

class IDataHandler { public: virtual bool step() = 0; };  // one coalesced slice; false at EOF

class InMemoryBarFeed final : public IDataHandler {
  InMemoryBarFeed(std::span<const std::span<const BarRow>> sources, SimClock&, EventBus<>&);
  bool step() override;   // advance clock to frontier → publish all heads == frontier (coalesced)
};
```
- Each source is a span of `BarRow` **pre-sorted ascending by `knowledge_ts`** (a backward row fails the
  monotonicity `ATX_ASSERT`, debug). Equal timestamps across sources coalesce into one slice, emitted in
  stable `(knowledge_ts, source_idx)` order.
- The spans, `SimClock`, and `EventBus` are **non-owning** — the caller owns them and must outlive the feed.

---

## The canonical replay loop

```cpp
auto bus = std::make_unique<EventBus<>>();        // ~8 MB ring — heap, not stack
SimClock clock;
InMemoryBarFeed feed(sources, clock, *bus);
auto consumer = bus->add_consumer(0);

while (feed.step()) {                              // step(): advance clock + publish the slice
  bus->drain_in_order([&](usize, const event::Event& e) {
    // e.knowledge_ts ≤ clock.now() is guaranteed here (no look-ahead)
    if (e.kind() == event::EventType::Market) {
      auto mp = e.payload_as<data::MarketPayload>();
      // ... react to mp.as_bar() ...
    }
  });
}
```
`step()` publishes; the **caller drains** between steps (keeps the ring bounded and preserves the
clock-before-publish ordering). In Phase 2 the dispatch lambda becomes Strategy → Portfolio → Execution.

---

## Known limits / residuals (carried to the backlog)

- **TSan** for the threaded bus path runs on Linux/clang only (clang-cl/Windows has none); the 2-thread
  test is race-clean by construction.
- **`EventBus<>&` ctor rigidity:** the feed binds the *default* bus params (`ConsumerCount = 1`). A
  multi-consumer or small-ring feed needs the feed templated on the bus type (Phase 2 if required).
- The merge heap is `std::priority_queue` (baseline; ≤ N entries, no steady-state alloc). A 4-ary
  intrusive heap over `fixed_vector` is the noted cache-friendly upgrade.
- PIT universe intervals, corporate-action records, and restatement versioning are *designed-for* in the
  `MarketPayload` layout (8 B headroom) but not built (Phase 3/5).
- Benchmarks are **Debug** upper bounds, not calibrated release figures.
