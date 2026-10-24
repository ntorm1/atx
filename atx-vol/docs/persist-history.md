# Persisted history: surface timelines and risk runs

Wave-3 lane P2 (2026-10-24). Two binary stores on the PersistFile (ATXVPF) container, both
little-endian, CRC-32C framed, published atomically and never rewritten in place. Byte layout:
`docs/storage/PERSIST_HISTORY_FORMAT.md`. The container itself (framing, verify levels, the
directory / manifest pattern, market-state stores) is lane P1's: `docs/persist.md` and
`docs/storage/PERSIST_FORMAT.md`.

Layout bump (lane E, 2026-10-24): RiskRun records the 21st explain column `pnl_carry`,
`ExplainTotals` is 160 B and `FrameCounters` 112 B (lane A2's counters); fingerprint
`0x07b280ae55b35c46` (was `0xc881a1e8c5e87255`): older segments are refused with `SchemaMismatch`.

## Surface timeline — uid-major fitted-surface history

`SurfaceTimelineStore` keeps one period file per UTC month (`<YYYY-MM>-g<seq12>.atxstl`) under a
manifest, each holding VERBATIM ATXVSA2 surface records behind a `(uid, ts)` index. One uid's
history over N snapshots is two binary searches plus N zero-copy views instead of N SurfaceDb
point reads, and every view is bit-exact to the partition's own view (same bytes, same
`create_over_record`).

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
a corrupted record is refused on `view()` (`SectionChecksum`), never served. `open` refuses a manifest
row whose header CRC, file CRC or instant range differs from the period file it names
(`SegmentMismatch`), so a forged or stale row can neither serve a file nor mis-route a query.

Accepted deviations from the design (ruling 2026-10-24):

- **Month-only period routing.** The frozen API has no period knob: a record's period is the UTC
  calendar month of its instant.
- **`SurfaceTimeline::uids()` is served from an owned copy** of the uid directory. The frozen
  return type is `std::span<const std::uint32_t>`, and the on-disk directory rows are 24-byte
  records (uid, entry range, ticker), not a bare u32 column.
- **`TimelineSource.ts_ns == 0` means "each record's own instant"** (its
  `PricingContext::now_ts_ns`); a nonzero `ts_ns` stamps every record of that source.

## Risk run — recorded frames, explains and VaR

`RiskRunWriter` records a `RiskStream` (or any producer of `RiskFrame` / `ExplainResult` /
`HistoricalVarResult`) into a directory of immutable segments (sealed at `segment_bytes`, 16 MiB
by default — see the seal table below); `RiskRunArchive` reopens it with zero-copy column spans plus owned
materializations; `compact_risk_run` merges the segments bit-exactly, optionally into the
zstd + byte-shuffle cold tier.

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
RiskRunStepInfo info = run.step_info(5).value();     // instants, needs, counters, segment
ExplainResult e = run.materialize_explain(5).value(); // owned deep copy, buckets included
RiskFrame f = run.materialize_frame(5).value();
for (std::size_t i = 0; i < run.n_var(); ++i) HistoricalVarResult v = run.var(i).value();
```

### Seal durability

`RiskRunOptions::seal_durability` (seals triggered by `segment_bytes`, default `Publish`) and
`finish_durability` (the last seal, default `Flush`) choose how durable a seal is when it
returns. Both write a unique temp beside the target and rename it, the segment first, then the
manifest, under the directory's writer lock. A reader never sees a partial file, and a crash in
between leaves an orphan, never a dangling entry.

- `Publish` — no flush. A process crash loses nothing that was sealed. A power loss before the
  OS writes the pages back can tear a segment. Open at Full / Paranoid refuses it by CRC; at
  Lazy (the default) a torn column is found on first touch: `frame()` / `explain()` return an
  empty span (as for an absent column; `step_info` and `materialize_*`, which says
  `SectionChecksum`, tell the two apart).
- `Flush` — FlushFileBuffers on the segment and the manifest before their renames: that seal
  survives a power loss. A `Flush` `finish()` first flushes every segment the writer sealed at
  `Publish`, so once it returns the whole run is durable. That flush needs write access to the
  files: while a reader holds one mapped, `finish()` returns `Busy`, changes nothing and can be
  retried.

The writer never starts a thread. A caller that wants a `Flush` seal off its hot path runs the
appends (or `finish()`) on its own thread.

### Single writer

A run directory has one writer, enforced through the manifest. `create` refuses a directory that
already lists a run (`AlreadyExists`, under the writer lock). Each seal publishes only if the
manifest still ends with the row this writer published last, or is empty for its first segment.
Otherwise it refuses with `SegmentMismatch`; a listed key is never replaced (`DuplicateKey`). From
then on every append and `finish()` returns that refusal and the writer's unsealed steps are lost,
while the run stays openable on what the manifest lists. A second writer on one directory is
therefore refused at its first seal instead of splicing its segments into the run.

### Compaction contract

`compact_risk_run(dir, codec, level[, durability])` (3-argument form = `Flush`) reads the run
fully verified into memory (no mapping is held on files the manifest stops naming), publishes the
merged segment under a fresh name, then, under the writer lock, re-reads the manifest. It replaces
the manifest with one naming only the merged segment, or refuses with `SegmentMismatch` when
anything was published since the run was read (every entry's seq and file CRC are compared).
Compact finished runs only: a compaction that lands under a live writer makes that writer's next
seal refuse (see Single writer), losing its unsealed steps. The
manifest write lives in this lane (`risk_run_archive_publish.cpp`, byte-compatible with
PersistDirectory's) because `PersistDirectory::publish` can add or replace only one key;
`PersistDirectory::replace_all` was deferred (ruling 2026-10-24). Superseded files stay on disk
(a reader may still map them); removing them is offline gc.

Accepted deviation: **the cold tier decodes at open, serially.** P1's container decodes every
zstd section at open on one thread: 253–335 ms for ~119 MB of compressed sections (the full
64-step run; 385–556 ms on a busier host). Spans are then allocation-free. A per-section fan-out
in P1's decoder is the suggested follow-up.

## Tool: `atx-vol-history-store`

```powershell
atx-vol-history-store timeline build --db <SurfaceDb root> --out <store root> [--keys k1,k2]
atx-vol-history-store timeline inspect <store root>
atx-vol-history-store timeline bench --out C:\atx-cache\persist\p2 [--uids 100 --days 22 --buckets 11]
atx-vol-history-store riskrun inspect <run dir>
atx-vol-history-store riskrun bench --out C:\atx-cache\persist\p2 [--legs 10000 --steps 64 --segment-mib 16]
```

`--out` is required for the bench verbs (no absolute default is compiled in; the umbrella
path-literal guard forbids one outside `C:\atx-data`), and outputs under `C:\atx-data` are refused.
`riskrun inspect` and the bench read runs through the public API only (`RiskRunArchive`,
`PersistDirectory`, `PersistFile`). Bench JSON (schema 2): `schema_version`, `host_cpu_busy` (system
CPU busy fraction over the run), `preset` (the label passed with `--preset`), and for risk runs
`seal_ms_publish` / `seal_ms_flush` (the same run recorded with each streaming durability) plus
`seal_split` (framing, Publish write + rename, Flush publish of one segment image).

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

| RiskRun (synthetic 10,260-leg book, 64 steps, 64 MiB segments; host 45–48% busy) | |
|---|---|
| bytes per step | 3.10 MB (frame 1.37 + explain 1.71 + index) |
| append (frame + explain) | p50 0.38–0.41 ms, p99 9.4 ms (target <= 1 ms); first append 17 ms (one-off page pre-fault) |
| reopen (3 segments, Lazy) / column span | 2.0–2.4 ms / first touch 0.6–0.9 ms (column CRC), then p50 19–22 ns |
| compaction, 3 segments -> 1 | none 0.55–0.78 s; zstd-1+shuffle 0.64–0.89 s, 1.66x; zstd-3+shuffle 0.98–1.18 s, 1.66x |
| zstd without / with byte shuffle (synthetic values) | zstd-1 1.13x / 1.66x; zstd-3 1.13x / 1.66x |
| cold-tier reopen (decode ~119 MB of zstd at open, serial) | 253–335 ms |
| 11-state RiskStream, synthetic SpiderRock day, 1,620 legs | step p50 14.1–14.8 ms, recording p50 0.06–0.08 ms; reopened totals bit-equal; zstd-1 1.71x, zstd-3 1.72x |

Seal latency by durability and segment size. `seal_ms_*` is the append call that sealed; the
split frames one segment image with P1's `build_persist_file`, then writes it both ways.

| segment_bytes (segment) | Publish seals | Flush seals | split: framing / write + rename / Flush publish |
|---|---|---|---|
| 64 MiB (~70 MB), run A | 115, 113 ms | 127, 125 ms | 46 / 80 / 82 ms |
| 64 MiB (~70 MB), run B | 135, 134 ms | 140, 135 ms | 66 / 81 / 97 ms |
| 32 MiB (~36 MB) | 77–113 ms, median 90 | 74–92 ms, median 86 | 22 / 32 / 41 ms |
| 16 MiB (~18 MB) | 23–48 ms, median 29 | 34–62 ms, median 36.5 | 10 / 7 / 18 ms |

This table is the trade-off behind the **16 MiB default** (ruling 2026-10-24; it was 64 MiB).
The <= 80 ms contract is for the Publish seal, and 64 MiB segments cannot meet it on this host. The
floor is P1's framing (46–66 ms for 70 MB: a copy plus two CRC passes) plus the OS's cached write
of a new 70 MB file (~80 ms, about 0.9 GB/s; Python's raw `WriteFile` of 70 MB measures 78–92 ms on
the same disk). The flush adds little on this NVMe (Flush seals 0–12 ms slower). The cached write
grows faster than the file (7 ms for 18 MB, 32 ms for 36 MB, 80 ms for 70 MB), so 16 MiB segments
meet the target (median 29 ms). The cost is more segments (11 instead of 3 for the 64-step run)
and a slower reopen (5.5 ms instead of 2.4 ms); compaction merges them afterwards. The latency is
reported by the bench, never asserted by a test: on a loaded host it is evidence, not a gate.
