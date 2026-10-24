# Persisted history: surface timelines and risk runs

Wave-3 lane P2 (2026-10-24). Two binary stores on the PersistFile (ATXVPF) container, both
little-endian, CRC-32C framed, published atomically and never rewritten in place. Byte layout:
`docs/storage/PERSIST_HISTORY_FORMAT.md`. (This is a separate page because lane P1's
`docs/persist.md` did not exist on the lane-P2 branch; integration may fold it in.)

## Surface timeline — uid-major fitted-surface history

`SurfaceTimelineStore` keeps one `YYYY-MM.atxstl` period file per UTC month under a manifest,
each holding VERBATIM ATXVSA2 surface records behind a `(uid, ts)` index. One uid's history over
N snapshots is two binary searches plus N zero-copy views instead of N SurfaceDb point reads, and
every view is bit-exact to the partition's own view (same bytes, same `create_over_record`).

```cpp
// Fold SurfaceDb partitions (keys YYYY-MM-DDTHHMM, one market instant each) into the store.
auto n = SurfaceTimelineStore::build_from_surface_db(db, keys, "C:/atx-cache/persist/timeline", 0);
auto store = SurfaceTimelineStore::open("C:/atx-cache/persist/timeline").value();
std::vector<PricedSurfaceView> views;   // keep alive while the refs are used
std::vector<SurfaceRef> refs;
std::vector<std::int64_t> ts;
store.collect(uid, from_ns, to_ns, views, refs, ts).value();
project_series(spec, refs, ts, policy, out);   // == the same call over owned surfaces, bit for bit
```

Rebuilding a period merges its previous generation (a re-written partition replaces its snapshot);
a corrupted record is refused on `view()` (`SectionChecksum`), never served.

## Risk run — recorded frames, explains and VaR

`RiskRunWriter` records a `RiskStream` (or any producer of `RiskFrame` / `ExplainResult` /
`HistoricalVarResult`) into a directory of immutable 64 MiB segments; `RiskRunArchive` reopens it
with zero-copy column spans; `compact_risk_run` merges the segments bit-exactly, optionally into
the zstd + byte-shuffle cold tier.

```cpp
RiskStream stream(book, RiskKernel::family());
auto w = RiskRunWriter::create(dir, stream.prepared(), RiskRunOptions{}).value();
stream.start(s0).value();  w.append_frame(stream.current()).value();
for (const MarketState &s : rest) {
  const ExplainResult *r = stream.step(s).value();
  w.append_frame(stream.current()).value();
  w.append_explain(*r).value();
}
w.finish().value();
auto run = RiskRunArchive::open(dir).value();
std::span<const double> pnl = run.explain(RiskExplainColumn::PnlTotal, 5);  // borrowed
const ExplainTotals *t = run.totals(5);                                     // bit-equal to live
```

## Tool: `atx-vol-history-store`

```powershell
atx-vol-history-store timeline build --db <SurfaceDb root> --out <store root> [--keys k1,k2]
atx-vol-history-store timeline inspect <store root>
atx-vol-history-store timeline bench --out C:\atx-cache\persist\p2 [--uids 100 --days 22 --buckets 11]
atx-vol-history-store riskrun inspect <run dir>
atx-vol-history-store riskrun bench --out C:\atx-cache\persist\p2 [--legs 10000 --steps 64]
```

`--out` is required for the bench verbs (no absolute default is compiled in; the umbrella
path-literal guard forbids one outside `C:\atx-data`), and outputs under `C:\atx-data` are refused.
Bench JSON: `schema_version`, `host_cpu_busy` (system CPU busy fraction over the run), `preset` (the
label passed with `--preset`).

## Measured (lane evidence, `vol-rel`, i7-1260P, host shared with build lanes)

Unqualified lane numbers — the perf pins come from `vol-rel-avx2` at integration
(`bench/baselines/…-persist-history-synthetic.json`).

| Timeline (synthetic: 100 uids x 22 days x 11 buckets = 242 partitions, 24,200 surfaces; host 57% busy) | |
|---|---|
| partitions (ATXVSA2) / store | 46.6 MB / 41.4 MB in 2 period files |
| build (read + verify 242 partitions, encode, publish incl. fsync) | 0.505 s = 82 MB/s |
| store open (2 periods, Lazy) | 5.8 ms |
| `range()` per call | p50 155 ns, p99 528 ns (target <= 200 ns p50) |
| `view()` incl. record CRC | eSSVI p50 3.2 us / p99 14.1 us; ConvexDense p50 3.6 us / p99 16.8 us (target <= 3 us p50) |
| `project_series` over 242 refs, timeline / owned | eSSVI 15.5 / 15.1 ms = 1.02x; ConvexDense 14.1 / 12.8 ms = 1.11x (target <= 1.1x); bit-equal both |

| RiskRun (synthetic 10,260-leg book, 64 steps; host 63-71% busy) | |
|---|---|
| bytes per step | 3.10 MB (frame 1.37 + explain 1.71 + index) |
| append (frame + explain) | p50 0.47 ms, p99 11.2 ms (target <= 1 ms); first append 23 ms (one-off page pre-fault) |
| seal of a ~67 MB segment | 165-186 ms = build 65 ms + write/FlushFileBuffers 135 ms (target <= 80 ms: missed, see report) |
| reopen (3 segments, Lazy) / column span | 2.9 ms / first touch 0.9 ms (column CRC), then p50 28 ns |
| compaction, 3 segments -> 1 | none 0.85 s; zstd-1+shuffle 1.15 s, 1.66x; zstd-3+shuffle 1.46 s, 1.66x |
| zstd without / with byte shuffle (synthetic values) | zstd-1 1.13x / 1.66x; zstd-3 1.13x / 1.66x |
| cold-tier reopen (decode 119 MB of zstd at open) | 385-556 ms |
| 11-state RiskStream, synthetic SpiderRock day, 1,620 legs | step p50 18.8 ms, recording p50 0.10 ms; reopened totals bit-equal; zstd-1 1.71x, zstd-3 1.72x |
