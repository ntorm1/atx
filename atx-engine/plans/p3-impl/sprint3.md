# Single-file ORATS history loader: a self-contained real-data Panel (Sprint 3)

p3 Sprint 3 adds a **second real-data source** that needs no join: the ORATS SMV
`tbltickerhistory` export — one ~3.3 GB zip whose single TSV entry carries **price,
corporate-action factors, and universe fields in one file**. S3 writes a pure
high-performance C++ loader that streams that zip into an on-disk **atx-tsdb per-date
`.seg` partition**, wires it into the atx-engine data interface (a new multi-segment
Panel attach — the interface expansion), applies corporate actions via the
`cumulReturnFactor` column, and proves the whole chain by driving the **unchanged p2
`RobustResearchDriver`** over the assembled Panel. No new search operator, objective, or
solver — S3 is ingestion + adjustment + an interface seam. S3 feeds the (still-unopened)
S2 benchmark.

All types live in `namespace atx::engine::data` (headers under `atx/engine/data/`), with
the zip primitive in `atx::core::io` and the attach in `atx::engine::alpha`. Every entry
point returns `atx::core::Result<T>` — check `.has_value()`, read `.error()` on failure.

## The one call: `build_history_panel`

`build_history_panel(const HistoryDataConfig&)`
([`data/history_panel.hpp`](../../include/atx/engine/data/history_panel.hpp))
assembles the real-data `alpha::Panel` deterministically from the on-disk ORATS per-date
`.seg` partition, applies the `cumulReturnFactor` total-return adjustment, constructs the
universe, and returns the Panel with a **digest pin** + lineage. It *orchestrates* the
shipped seams (S3-2 loader, S3-3 TRI, S3-4 multi-attach, S1-4 universe, the shared S3-5
`digest_panel`); it re-implements none of them.

```cpp
using namespace atx::engine::data;

// 1. (operator, once) stream the zip into an on-disk per-date .seg partition
OratsLoadConfig load;
load.zip_path         = "<HOME>/Downloads/tbltickerhistory3_10y.zip";
load.out_dir          = "C:/atx-data/orats_history_1d";         // gitignored; keep OFF OneDrive
load.min_date_nanos   = *detail::date_to_nanos("2020-01-01");    // user date floor
load.created_at_nanos = 0;
auto stats = load_orats_history(load);   // Result<OratsLoadStats>: rows_read/kept/filtered, dates_written, ...

// 2. assemble the Panel from the partition
HistoryDataConfig cfg;
cfg.seg_dir              = load.out_dir;
cfg.window               = alpha::TimeWindow{start_ns, end_ns}; // half-open [start,end); default {} = all dates
cfg.universe.min_adv_usd = 5.0e6;
cfg.universe.adv_window  = 21;
cfg.universe.top_n_by_adv= 0;                                   // 0 = no count cap

auto res = build_history_panel(cfg);
if (!res) { /* res.error(): IoError / InvalidArgument — fails closed, no partial Panel */ }
const HistoryPanel& hp = res.value();

const alpha::Panel& panel = hp.panel;    // ready for Factory::mine / RobustResearchDriver::run
const atx::u64 digest     = hp.digest;   // determinism pin (== on a second build)
```

The returned `panel` is exactly the `alpha::Panel` the engine already mines, combines,
and reports over — see [`alpha-pipeline-reference.md`](alpha-pipeline-reference.md).

## What's in the Panel (canonical field order)

The digest hashes the fields in **this order** — changing it (or adding/removing a field)
is a deliberate, digest-moving change:

| Field | Meaning |
|------|---------|
| `close` | **total-return index** = `close × cumulReturnFactor` (the canonical adjusted price) |
| `raw_close` | raw as-traded ORATS close |
| `volume`, `high`, `low`, `open` | raw ORATS OHLCV |
| `market_cap` | `shares` × raw close |
| `sector` | GICS sector code, else `-1` sentinel |

The `in_universe` membership mask is applied to the Panel.

## Why the corporate-action math is correct (and non-leaking)

Canonical adjusted `close = close × cumulReturnFactor` — `close` is the as-traded
contemporaneous close; `cumulReturnFactor` is the total-return back-adjustment factor on
the **same row**. (`closeUnadjPr` is the *lagged prior-day close*, not the adjustment
input.) Full-history back-adjustment rescales price *levels* when a future split/dividend
arrives, but **returns are invariant to a constant multiplicative rescale** — and alphas
consume returns/ranks, so it leaks no signal. Implemented by reusing the S1-3
`adjust_total_return` whose `total_return_index` equals `close × cumulReturnFactor`
exactly. **Verified:** AAPL 2012-03-26 `606.98 × 0.0299354 = 18.17` ≈ Yahoo present-basis
adjusted close. Full derivation + every `file:line`:
[`data-ingestion-reference.md`](data-ingestion-reference.md) §7.

## The layering (and the one constraint that bites)

`io::ZipEntryReader` (atx-core, miniz confined to the `.cpp`, PRIVATE to atx-core) →
`load_orats_history` (streams the TSV, projects **16** of 71 columns keyed on
**`securityID`**, writes per-date `.seg` + symbology/manifest side-cars) →
`attach_multi_segment_panel` (unions the per-date segments into one **owned** Panel) →
`build_history_panel`. The atx-tsdb segment format caps field names at **15 chars**, so
the TSV column `cumulReturnFactor` (17) is stored under the segment name `cumReturnFactor`
(15) — a `consteval` static-assert guards the limit at compile time, and `resolve_header`
maps the short name back to the real TSV header. Only `securityID` is the instrument key
(stable across renames/delistings); `ticker_tk`/`todayTicker` go to the side-car
symbology, never the segment.

## Determinism

`digest` is `signal_set_digest` over the Panel's fields in canonical order, via the
**shared** `data::digest_panel` extracted from `real_panel.cpp` (one definition for both
paths) — the S1-5 golden pin `0x2a22a873483d9157` is unchanged by the extraction. Two
builds of identical inputs produce a byte-identical history digest. The `_symbology.parquet`
side-car is sorted by `securityID` so the partition directory is byte-reproducible.

## How it's proven (and the deferred real build)

- **Always-run in CI:** `OratsE2ESmoke.SyntheticPartitionRunsUnchangedRobustPipeline` writes
  a synthetic 60-date × 30-instrument `.seg` partition, runs `build_history_panel`, and
  drives the **unchanged** `RobustResearchDriver::run` over it — asserting a non-zero
  research digest (the mine→gate→admit loop executed). This proves the integration without
  any data dependency.
- **Guarded (skip in CI):** `RealPartitionRunsUnchangedRobustPipeline` (probes
  `data/orats_history_1d`) and `OperatorOratsZip` (the committed operator caller, env
  `ATX_ORATS_ZIP`).

The **real 11 GB build** is a documented manual operator step (gitignored output) — see
[`data-ingestion-reference.md`](data-ingestion-reference.md) §7.6 for the one-command
invocation. As of S3 close it is **deferred**; the code path is proven by the synthetic
always-run test.

## Known limitations (carried as caveats)

- **Real-universe smoke is deferred** — the headline E2E on the full US universe is the
  operator data-build (§7.6), not yet run. The synthetic always-run test proves the wiring.
- **Single parse thread** — the loader streams + parses on one thread; chunk-boundary-stitched
  parse-parallelism is a backlog perf lever (the partition is the bottleneck, not the engine).
- **Multi-segment attach materializes** an owned Panel (per-date segments have independent
  instrument axes); a borrowed/zero-copy attach via a global build-time axis is backlogged.
- **IV-surface fields ingested, not yet consumed** — `atmCenI_*`/`nEarnCnt_5d` land in the
  segment but no alpha operator reads them (a p4 capability; p3 adds no new search surface).
- **`closePr` semantics** and the **GICS single-digit → sector_code mapping** are ingested
  as-is; both are backlogged for verification before S2 screens on them.

## Where the numbers live

Per-unit commits, test counts, the review waves, and residuals are in the ledger
[`sprint-3-progress.md`](sprint-3-progress.md); the module status table is in
[`ROADMAP.md`](ROADMAP.md). The two reference maps are
[`alpha-pipeline-reference.md`](alpha-pipeline-reference.md) (the mine→prove pipeline) and
[`data-ingestion-reference.md`](data-ingestion-reference.md) §7 (the ORATS ingestion path).
