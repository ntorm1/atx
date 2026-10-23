# Live query: the fit's hand-off, the serve book and the hot query path (H1, r6)

Status: lane H1 (vol-sota r6, 2026-10-23), phase B of the serving record
(`docs/serve-bundle.md`). New: `include/atx/vol/api/fitting/serve_book.hpp` /
`src/fitting/serve_book.cpp` (the live book), the fitter's hand-off at the end of
`certify_and_serve`, the `evaluate_quote_slice` fast path, the quote-bench
`graded.serve_records` key and B0's `serve_build` stage. Nothing on a default path builds a
record; with every key off the predictions are byte-identical (9/9 parity hashes).

## 1. The pipeline

```
fit_quote_slice / fit_quote_surface
  certify_and_serve (C2's validation, C0's align_with_query)
    -> detail::stamp_serve_handoff(out, families)      QuoteSliceFit::serve_handoff
                                                         (fit clock: two family prices / side)
serve_build stage (after the fit clock closes; quote-bench: graded.serve_records)
  phase 1: build_serve_record(fit, ladders = false)     tiers 1-2: the first usable bundle
           -> ServeBook::publish_expiry                 version N
  phase 2: detail::build_serve_record_on(fit, policy, phase-1 record)
                                                         the same families, + ladders (tier 0)
           -> ServeBook::publish_expiry                 version N + 1
readers (one ServeBookReader per thread)
  serve / implied_vol / serve_chain                     tiers 0-1 (the primary tier cap)
  misses -> typed route; Stale / Missing / Invalidated -> async rebuild queue
```

### The hand-off (`QuoteSliceFit::serve_handoff`)

`VolSpaceCache::families()` hands `certify_and_serve` a raw pointer to the per-thread cache's
holder, so the hook cannot share ownership of it. It hands over the **recipe** instead: per
side the geometry (T, r, q), the anchor spot, the (σ, q) box and node counts, lane K3's
validated envelope (`ValidatedErrorEnvelope`, its family pointer cleared) with the audit and
the grid bound `q_bound`, and two of the family's own prices at points that are no
construction node. The record builder re-prepares the families from the recipe and accepts
them only when both probe prices are reproduced to the bit and the envelope re-derived from
the rebuilt nodes equals the handed one (`same_envelope`); then it applies the fitter's audit
and grid bound. A mismatch refuses that side (`ServeUnsupportedReason::HandoffMismatch`); the
record never serves a family the fitter did not certify. The hook writes nothing a fitted or
served number reads; it is set only when the fit ran with `deam_family_q` families.

Origins are typed: `ServeFamilyOrigin::Handed` (the fitter's), `Built` (the record's own
recipe, certified through the same `deam_family_certify`), `Supplied`
(`build_serve_record_with`). Self-built and handed records read the same fields
(`ServeSideSupport::family_bound_rel`, `domain_bound_usd`, the envelope; review-W2ci m1).

### The fast path (`QuoteSliceFit::serve_record`)

Opt-in by the record's presence. `evaluate_quote_slice(fit, K, side)` first asks
`detail::serve_record_value`: when the record was built from THIS fit (spot, variance clock,
rate and the bracketing node prices bit-equal), the strike is inside the certified domain,
and the query is a between-node or moved-node one, the price is the same certified slice
price and the volatility is the record's Family-tier root of that price at the fit spot,
returned only when lane K3's enclosure accepts it (`Skipped` / `RefinementValidated`, bound
≤ `iv_tolerance` = 1e-7). An unmoved node, a price-only node, a withheld node side and every
refused root fall through to the record-free path unchanged.

## 2. The serve book (`serve_book.hpp`)

### Publication protocol

- A `ServeBookVersion` is immutable: entries sorted by `expiry_id`, each the record
  (`shared_ptr<const SliceServeRecord>`), `expiry_ns`, `next_event_ns` and a caller `tag`,
  plus the version number.
- `ServeBookTransaction` stages puts and retirements; `commit()` takes the publisher mutex,
  builds the whole next version off to the side from the current one, then
  `current.store(next)` (an `std::atomic<std::shared_ptr>`) and `number.store(n + 1,
  release)`. Everything that can throw (the copy, `make_shared`, the graveyard reserve)
  happens before the store: a publisher that dies before or inside `commit()` publishes
  nothing and the old version stays live (`APublisherThatDiesMidPublishLeavesTheOldVersionLive`).
- Version numbers only grow. A reader compares numbers, never addresses, so the reuse of a
  freed version's memory cannot fool it (no ABA).

### Readers

- `ServeBook::reader()` returns a per-thread handle. Each query does one acquire load of the
  counter; only when it moved does the reader re-pin (`current.load()`). A chain
  (`serve_chain`) pins once: every contract of a chain is answered by one version.
- `primary_tier_max` (default Family) caps every request: a Certified Fast solve or a Cold
  route above the cap is a typed `TierUnavailable / ColdPath`, never a synchronous solve on
  the quoting thread.
- Normal queries allocate nothing, including the re-pin after a publication and a miss that
  queues a rebuild (`LivePublicationAlloc.TheQueryPathAllocatesNothing`).

### Reclamation (deferred, publisher-side)

Readers never free. Each commit moves the replaced version into a graveyard; every commit (or
`reclaim()`) frees, on the publisher's thread and outside the lock, the retired versions whose
only reference is the graveyard's. A version a reader still pins stays alive with its records
(`RetiredVersionsAreFreedOnlyAfterTheLastReaderLeaves`). A reader that outlives its book keeps
the book's shared state alive.

### Typed responses

| Condition | status | route | queued |
|---|---|---|---|
| no entry for the expiry | `TierUnavailable` | `ColdPath` | `Missing` (with a rebuild function) |
| entry has instants and the request no clock (`now_ns ≤ 0`) | `Stale` | `ColdPath` | no |
| `now_ns ≥ expiry_ns` or `≥ next_event_ns` (expiry, ex-dividend: geometry no longer applies) | `Invalidated` | `Refit` | `Invalidated` |
| request tier above `primary_tier_max` | `TierUnavailable` | `ColdPath` | no |
| record `Stale` (age > `max_age_ns`, route `Rebuild`) | `Stale` | `Rebuild` | `Stale` |
| ... and the queue is full | `BudgetExhausted` | `Rebuild` | nothing queued |
| every other record status (`OutOfDomain`, `PriceOnly`, `SigmaOutOfBox`, `NonMonotoneBand`, `Unconfirmed`, ...) | unchanged | unchanged | no |

Unsupported risk stays NaN with a typed reason, never zero
(`TheRecordsTypedRefusalsPassThroughTheBook`).

### The async rebuild queue

A ring of `queue_capacity` requests allocated at construction. A request for an expiry that
is queued or being rebuilt is deduplicated; a request from a reader whose pinned version is
already older than the book's is not queued at all (the reader re-answers from the newer
version). `build_workers` `std::jthread`s (never the pricing pool) pop requests, call the
owner's `ServeRebuildFn`, and publish what it returns; `build_workers = 0` leaves the queue to
`run_pending()`. A rebuild that throws or returns nothing publishes nothing (counted). The
counters report queue depth, queue age and rebuild completion (max and sum).

## 3. Clocks (B0)

`stages_json` gains the `serve_build` stage: `not_built` unless records were built
(quote-bench `graded.serve_records`); built, it is one more exclusive stage and
`fit_clock_s = sum − serve_build`. `clocks.ready_to_quote_s` is timed under the key (the
stage's seconds, the first-usable and final publication latencies per record) and stays
`not_built` otherwise. The fast-path audit the key runs (record-free and record answers per
row and per node midpoint) belongs to no stage: it shows as `stages_unattributed_s` inside
`cold_calibration_s` (2.7-3.8 s on SPY, section 6.3), so under the key compare fit clocks,
not `cold_calibration_s`.

## 4. Error statements and the tier table

See `docs/serve-bundle.md` ("Error statements"). In short: a node's bytes are `Exact`; the
family tier's statement is K3's per-point bound (`RefinementValidated`) where the envelope
covers the point; a ladder is `RefinementValidated` when its Chebyshev plane tail verifies and
its check points sit under it, plus K3's largest per-point bound over its samples, else it is
served `Sampled` (labelled, `ServeAnswer::sampled`) under `ServeRecordPolicy::sampled_ladders`,
and a ladder IV root is `Ok` only on a validated statement within `iv_tolerance`.

| Tier | Serves | Statement | Allocation | Cold solve |
|---|---|---|---|---|
| Ladder (0) | node sides with a kept ladder, spot in its window | `RefinementValidated` or `Sampled` | none | never |
| Family (1) | any strike of the domain, any spot, σ / q in the box | K3 per-point (`RefinementValidated`), or none outside the envelope | none | never |
| Certified (2) | fit spot: the slice; else one Fast solve | `Exact` / `KernelTested` | the Fast solve may | the Fast solve (above the book's default cap) |
| Cold (3) | route only | — | — | never inside the record or the book |

## 5. What A1 / Q0 may assume

- Publish through a `ServeBook`; read through one `ServeBookReader` per thread. Never hold a
  version across a blocking call longer than needed: it pins its records.
- A served answer carries its statement class; `sampled = true` answers are never certified.
- Readers never block on a build and never run a cold solve at the default cap.
- The record does not move the smile on a spot move (sticky strike); only a new fit does.
- `VolaSession` / `PricedSurface` do not consume records yet (they serve curve fits; A1).

## 6. Measurement (lane H1, 2026-10-23)

Evidence: `C:\atx-cache\quote-benchmark\H1-20261023-live-query\` (commands and hashes are in
the lane report).

The host was shared with three other lanes throughout (their quote benches and builds are in
every load line); every timing below is therefore **unqualified** (a development row).

### 6.1 Keys off: parity

The 9 default prediction hashes (W2c-j's `expected-9`) MATCH 9/9, and K4's 4 native-route
hashes MATCH 4/4. On the native route the families are on, so the hand-off hook runs. Both
sets were checked twice:
- with the binary at `25443254` (sha256 `41a2ef5a…1680`);
- with the final binary at `5c03aee5` (sha256 `cea6b843…0bb9`; evidence `parity-head\`).

The hook changes no prediction.

### 6.2 The fast path against today's path

`graded.serve_records` with `serve_record_profile: no_ladders` (the fast path reads tier 1 only),
binary `5c03aee5` (sha256 `cea6b843…0bb9`), workers 2. For every served row and every node
midpoint (both sides), `evaluate_quote_slice` without and with the record; every difference is
listed in the run's native report (`graded_stats.serve_records.differences` /
`midpoint_worst`) with both volatilities' Newton distances to `andersen_lake`'s root of the
served price on the record's contract (the fit's kernel options).

| run | CSV vs keys off | records (families handed / built) | rows compared / differ | rows max |Newton| fast path / today | rows today > 1e-7 | midpoints compared / differ | midpoints max |Newton| fast / today | midpoints today > 1e-7 | midpoint `evaluate_quote_slice` p50 today / with record |
|---|---|---|---|---|---|---|---|---|---|
| dev200 graded-v1 | differs (445 rows) | 963 (0 / 1486) | 50,348 / 445 | 5.1e-10 / 1.4e-4 | 86 | 48,986 / 15,343 | 2.4e-9 / 1.9e-4 | 1,103 | 728 / 443 µs |
| dev200b graded-v1 | differs (271 rows) | 1,009 (0 / 1332) | 54,355 / 271 | 4.7e-10 / 2.7e-4 | 78 | 52,766 / 12,583 | 1.6e-9 / 1.4e-4 | 744 | 820 / 756 µs |
| cohort5 graded-v1 | differs (31 rows) | 70 (0 / 93) | 8,375 / 31 | 7.4e-11 / 2.4e-5 | 1 | 8,238 / 3,487 | 3.0e-9 / 1.2e-4 | 362 | 757 / 48 µs |
| SPY, K4 native route | **identical** (MATCH `75ca0fc5`) | 4 (8 / 0) | 1,410 / 0 | — | 0 | 1,402 / 1,156 | 2.2e-10 / 1.4e-5 | 19 | 821 / 52 µs |
| cohort5, K4 native route | differs (30 rows) | 70 (25 / 68) | 8,375 / 30 | 7.4e-11 / 2.4e-5 | 1 | 8,238 / 3,335 | 1.3e-9 / 1.2e-4 | 360 | 825 / 53 µs |

Reading:
- **Not identical.** The served value set differs from today's on dev200, dev200b and
  cohort5.
- **Which rows differ.** Every differing row is one the fast path answers by construction:
  a between-node or moved-node volatility, with the price unchanged.
- **Distance to the accurate root.** On those rows the record's volatility is within 5.1e-10
  (rows) and 3.0e-9 (midpoints). Today's path is up to 2.7e-4 away, because its
  re-implication runs on the fit's inversion route.
- **No new error or refusal.** No record answer exceeds 1e-7, and the record refuses no
  midpoint that the record-free path answers.
- **SPY native route.** It serves the identical set, because its rows are unmoved nodes.
  Between nodes the record answers 16 times faster.

### 6.3 The fit clock and the `serve_build` stage

The hook's direct cost (bench row `serve/<ds>/handoff_stamp`, `atx-vol-serve-record-bench` at
`500d3d1c`, run `measure\full-1`): `detail::stamp_serve_handoff` costs 32-43 µs p50 per expiry
(max 74 µs). On SPY's 4 expiries that is about 0.17 ms against a fit clock of about 5 s,
0.003 %.

Cold trials (`atx-vol-quote-bench`, SPY K4 native route with families on, so the hook runs at
every certification). Each run is 10 trials pinned to CPU 6, a P core. The root binary is
`a969fd20`, sha256 `08de794c…`; the H1 binary is `5c03aee5`, sha256 `cea6b843…`. Pairs were
interleaved root / H1. Times are medians in seconds:

| run | cold_calibration_s min / median / p95 | fit clock median (stages less `serve_build`) | inverses | final_grid_validation | `serve_build` | SMT contended |
|---|---|---|---|---|---|---|
| root-a | 5.021 / 5.314 / 5.900 | 5.312 | 3.197 | 0.742 | `not_built` | 4/10 |
| H1, keys off, a | 4.934 / 5.160 / 5.982 | 5.157 | 3.125 | 0.722 | `not_built` | 0/10 |
| root-b | 4.612 / 6.123 / 8.837 | 6.119 | 3.506 | 0.865 | `not_built` | 6/10 |
| H1, keys off, b | 5.270 / 6.410 / 10.208 | 6.406 | 3.725 | 0.908 | `not_built` | 8/10 |
| H1, `graded.serve_records` (default profile) | 10.847 / 12.261 / 15.250 | **5.002** | 3.070 | 0.676 | 4.188 | 4/10 |

Every run publishes the same predictions (`75ca0fc5…`).

- **The fit clock does not move.** The paired differences are -2.9 % and +4.7 %, inside the
  trial noise: the host was shared, and p95 reaches 1.6 times the median.
- **Under the key** the fit clock is 5.002 s and the record work sits in `serve_build`:
  4.19 s median for 4 records with ladders. Per record:
  - the first usable bundle (families, tiers 1-2) is published 17.6 ms median after the fit;
  - the laddered record follows at about 1.0 s median.
- **The audit.** The key's fast-path audit adds 2.7-3.8 s to `cold_calibration_s` outside
  every stage (`stages_unattributed_s`). It is a measurement instrument and not a serving
  cost. Compare fit clocks, not `cold_calibration_s`, across the key.

### 6.4 H1 / H2 / H3 rows

**Measurement conditions.** All rows come from `atx-vol-serve-record-bench` (vol-rel-avx2,
`500d3d1c`, sha256 `a2ad725d…`), run `measure\full-1`:
- one pinned thread on mask 0x3f; TSC timing at 0.4006 ns per tick, timer overhead 17.6 ns;
- 5 repetitions per row, medians shown.

**Datasets.**
- `spy_surface`: dev200 SPY, 4 expiries, the H0-A archive.
- `spy23`: one 23-day expiry.
- `_tick`: the same data under `serve_record_policy_tick()` (16 × 8 ladder planes); otherwise
  the default 12 × 6 planes.

**Load line.**
- Before: CPU 40 / 35 %, with K4b's quote bench at 147 % and Defender at 65 %.
- After: CPU 35 / 51 %, with VS Code at 180 %, K4b at 159 % and clang-cl at 90 %.

These are **development rows on a shared host and not qualified**.

**H1: a record price with the primary Greeks (tier 0). Budget ≤ 150 ns p50, ≤ 1 µs p99.**
Times are in ns, given as p50 / p99 / p99.9 / max:

| dataset | record call (`tier0_price4`) | through the book, steady (`hit0`) | through the book under concurrent publication (`hit0`) |
|---|---|---|---|
| spy_surface | 167 / 240 / 364 | 246 / 533 / 1,315 / 27,060 | 459 / 860 / 1,802 / 153,300 |
| spy_surface_tick | 155 / 205 / 262 | 236 / 500 / 1,254 / 17,860 | 427 / 681 / 1,468 / 41,510 |
| spy23 | **125** / 148 / 170 | 212 / 565 / 1,122 / 10,410 | 457 / 911 / 1,560 / 22,960 |
| spy23_tick | **137** / 162 / 174 | 197 / 295 / 541 / 10,770 | 451 / 916 / 1,900 / 17,940 |

- **p99:** WITHIN everywhere, and also under publication.
- **p50, record call:** WITHIN on spy23 and 1.04-1.11 times over on spy_surface.
- **p50, through the book:** MISS by 1.3-1.6 times. The book adds a counter load, a binary
  search over expiries and the freshness checks.
- **p50 under publication:** MISS by 2.8-3.1 times. The publisher thread republishes
  continuously (236k-971k versions published and reclaimed in the row) and readers re-pinned
  25k-102k times.

**H2: record IV ≤ 500 ns p50; a 300-contract served chain ≤ 100 µs.**

| dataset | record IV p50 / p99 (ns) | IV `Ok` / contracts | 300 chain, tier 0 only (record) p50 / p99 (µs) | 300 chain through the book, one version, mixed tiers p50 / p99 (µs) |
|---|---|---|---|---|
| spy_surface | 276 / 397 | 0 / 1,100 (1,074 `Unconfirmed`, 26 band) | 19.6 / 21.4 (245) | 513 / 561 (245 tier 0 + 55 tier 1) |
| spy_surface_tick | 316 / 439 | **1,123 / 1,190** | 24.8 / 29.2 (266) | 183 / 194 (266 + 34) |
| spy23 | 261 / 377 | 0 / 266 (255 `Unconfirmed`) | 16.5 / 19.5 (232) | 464 / 503 (232 + 68) |
| spy23_tick | 295 / 408 | **259 / 279** | 25.1 / 30.9 (245) | 306 / 354 (245 + 55) |

- **Record IV:** WITHIN, at 261-316 ns. A ladder root is `Ok` only under the tick profile; the
  default 12 × 6 plane's statement over vega exceeds 1e-7.
- **Tier-0-only chain:** WITHIN.
- **Mixed served chain:** MISS by 1.8-5.1 times. Tier-1 (family) contracts cost 6-20 µs each.
  A chain whose ladders cover every contract meets the budget.

**H3: tails, miss rate and torn versions.**

| row | miss rate | all-query p99 / p99.9 / max | notes |
|---|---|---|---|
| steady (4 rows) | 0 | 9.3-19.5 µs / 12.4-23.8 µs / 53-616 µs | tier-0 tail: p99.9 0.54-1.3 µs, max 10-27 µs |
| concurrent publication (4 rows) | 0 | 11.1-18.1 µs / 13.4-23.0 µs / 80-664 µs | every published version is reclaimed |
| quote burst (4 rows) | 0.83-0.99 by design | 9.3-22.2 µs / 43.4-89.4 µs / 0.46-6.1 ms | see below |

In the quote-burst row every expiry is invalidated at once while readers keep quoting:
- Refusals are typed `Stale` / `Rebuild`: p50 186-334 ns, p99 6.1-9.2 µs.
- 1-4 rebuilds are queued. Another 77k-242k requests are deduplicated against them.
- `budget_exhausted` is 0 and no rebuild fails.
- Every expiry is fresh again within 25-193 ms. Queue age is at most 122 ms and a rebuild
  takes at most 165 ms.

**Torn versions = 0.** This comes from `LivePublication.ReadersNeverSeeATornVersion`:
- 7 runs, each with 4 readers and a publisher;
- 171k-209k versions per run;
- every chain checks one version tag end to end.

### 6.5 Accuracy, ladder coverage, consistency

Accuracy compares the served price with `andersen_lake` at the served σ, on node strikes at a
moved spot (`book/<ds>/accuracy`):

| dataset | tier 0 err p50 / max (USD) | tier 0 sampled / served | tier 1 err max (USD) | within own statement |
|---|---|---|---|---|
| spy_surface | 1.3e-5 / 9.5e-5 | 543 / 1,100 | 1.6e-4 | 1,282 / 1,282 |
| spy_surface_tick | 1.6e-10 / 2.2e-8 | 3 / 1,190 | 1.6e-4 | 1,282 / 1,282 |
| spy23 | 8.0e-6 / 3.5e-5 | 76 / 266 | 6.2e-9 | 299 / 299 |
| spy23_tick | 1.1e-10 / 4.9e-10 | 1 / 279 | 6.2e-9 | 299 / 299 |

**Ladder coverage per side** (`serve/<ds>/build`, run `measure\sides-2`). Each cell gives
kept (validated + sampled) of all node sides:

| dataset | calls | puts |
|---|---|---|
| spy_surface | 624 (412 + 212) of 705 | 476 (145 + 331) of 705 |
| spy_surface_tick | 658 (658 + 0) of 705 | 532 (529 + 3) of 705 |
| spy23 | 167 (150 + 17) of 167 | 99 (40 + 59) of 167 |
| spy23_tick | 167 (167 + 0) of 167 | 112 (111 + 1) of 167 |

- **Puts under the tick profile.** On SPY the put ladders validate: K3's per-point bounds
  cover their samples on these fits. The fixture's put ladders do not (`docs/serve-bundle.md`).
- **Puts under the default planes.** Most put ladders are `Sampled`.
- **Where puts drop.** Puts lose 24-41 % of their node sides as ineligible, against 0-12 % for
  calls. These sides are refused at build (exercise region or bound) and fall to tier 1.

**Consistency.**
- Every node answer at the fit spot equals the slice's node bytes: 3,920 / 3,920; 4,010 /
  4,010; 934 / 934; 947 / 947.
- Every between-node point is within its bound: 9,814 / 9,814 and 2,324 / 2,324.

### 6.6 Bytes and build clocks (review-H0A m4 corrected: all three holder geometries)

| dataset | symbol bytes | per expiry mean / max | geometries | ladders (1,368 B each) | first build (s) | ladder ms | first usable p50 / final p50 (book) |
|---|---|---|---|---|---|---|---|
| spy_surface (4 expiries) | 2.91 MB | 728 / 865 KB | 1.12 MB | 1.51 MB | 4.65 | 4,096 | 27.7 ms / 1.25 s |
| spy_surface_tick | 3.04 MB | 759 / 941 KB | 1.12 MB | 1.63 MB | 7.17 | 11,390 | 18.2 ms / 1.48 s |
| spy23 (1 expiry) | 714 KB | 714 KB | 280 KB | 365 KB | 1.03 | 881 | 20.7 ms / 1.07 s |
| spy23_tick | 732 KB | 732 KB | 280 KB | 383 KB | 1.75 | 1,786 | 22.0 ms / 1.84 s |
