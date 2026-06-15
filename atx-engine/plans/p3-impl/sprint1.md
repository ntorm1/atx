# Real-data hardening: a PIT-correct, digest-pinned real-data Panel (Sprint 1)

p3 Sprint 1 ties the as-built ingestion stack, the p2 S6 BYO-data layer, and the
on-disk real US equity data into **one coherent, PIT-correct, digest-pinned
real-data path** — corporate actions applied, a real universe constructed — so the
unchanged p2 mine→prove pipeline can be pointed at real data **without fabricating
returns or leaking the future**. No new search operator, objective, or solver: this
is plumbing + adjustments + honest measurement.

All types live in `namespace atx::engine::data` (headers under `atx/engine/data/`).
Every entry point returns `atx::core::Result<T>` / `Status` (`tl::expected`) — check
`.has_value()` and read `.error()` on failure.

## The one call: `build_real_panel`

`build_real_panel(const RealDataConfig&)` ([`data/real_panel.hpp`](../../include/atx/engine/data/real_panel.hpp))
assembles the real-data `alpha::Panel` deterministically from the on-disk databento
daily OHLCV hive ⋈ the corporate-action security master ⋈ the derived universe, and
returns it with a **golden digest pin** + the composing lineage. It *orchestrates*
the shipped seams (S1-2 ingest, S1-3 adjust, S1-4 universe, the S6 `Dataset`/`Catalog`/
`adapt_*` layer); it re-implements none of them.

```cpp
using namespace atx::engine::data;

RealDataConfig cfg;
cfg.databento_hive_root  = "data/databento/equs_ohlcv_1d_by_date";
cfg.security_master_path = "data/us_security_master_smoke/security_master/security_master.parquet";
cfg.seg_cache_dir        = "build/seg-cache";          // reserved (no .seg round-trip today)
cfg.window               = alpha::TimeWindow{start_ns, end_ns};  // half-open [start,end) trading dates
cfg.universe.adv_window     = 21;                      // causal trailing ADV window (also fixes adv{w})
cfg.universe.min_adv_usd    = 1.0e6;                   // liquidity floor ($/day)
cfg.universe.min_mktcap_usd = 0.0;                     // 0 = no market-cap floor
cfg.universe.top_n_by_adv   = 0;                       // 0 = no count cap; else keep top-N each date

auto res = build_real_panel(cfg);
if (!res) { /* res.error(): IoError / InvalidArgument — fails closed, no partial Panel */ }
const RealPanel& rp = res.value();

const alpha::Panel& panel = rp.panel;   // ready for Factory::mine / ResearchDriver::run
const atx::u64 digest     = rp.digest;  // the determinism pin (== on a second build)
// rp.lineage == {"corp_actions", "price", "universe"}  (ascending)
```

The returned `panel` is exactly the `alpha::Panel` the engine already knows how to
mine, combine, and report over — see [`alpha-pipeline-reference.md`](alpha-pipeline-reference.md)
for the entry points (`Factory::mine`, `ResearchDriver::run`, `RobustResearchDriver::run`).

## What's in the Panel (canonical field order)

The digest hashes the fields in **this order** — changing it (or adding/removing a
field) is a deliberate, digest-moving change:

| Field | Meaning |
|------|---------|
| `close` | **total-return index** (split + reinvested-dividend adjusted) — the canonical price |
| `raw_close` | unadjusted databento close, retained for signals that want it |
| `volume`, `high`, `low`, `open` | raw databento OHLCV |
| `dollar_volume`, `vwap`, `adv{w}` | causal datafields appended by `price_to_panel` |
| `market_cap` | `shares_outstanding` (PIT as-of) × **raw** close (split-invariant) |
| `sector` | GICS if present, else SIC fallback, else `-1` sentinel |

The S1-4 `in_universe` membership mask is applied to the Panel.

## Why this is PIT-correct (the two load-bearing arguments)

1. **Adjustment = total-return, and it does not leak.** `close` is a split + reinvested-
   dividend total-return index. Full-history back-adjustment rescales price *levels*
   when a future split/dividend arrives, but **returns are invariant to a constant
   multiplicative rescale** — and alphas consume returns/ranks, so back-adjustment
   leaks no signal (asserted bit-for-bit by `ReturnInvariantUnderConstantPriceRescale`).
2. **The leak guard is on the fundamental as-of join, not on price.** `shares_outstanding`
   and sector are forecasts-as-of-a-filing: S1-2 forward-fills each using only filings
   with knowledge-date `shares_filed_date ≤ d`, so a not-yet-filed value never appears
   early. Corporate-action facts (`cum_adj_factor`, `cash_dividend`) are mechanical
   ex-date facts, joined on their own event date.

Full derivation + every `file:line`: [`data-ingestion-reference.md`](data-ingestion-reference.md) §3.

## Determinism

`digest` is `signal_set_digest` over the Panel's fields in canonical order. Two builds
of identical inputs produce a **byte-identical** digest — symbol interning (first-seen
master-file order), the date axis, NaN placement, and field order are all fixed. The
smoke pin is **`0x2a22a873483d9157`** over the 3-symbol smoke window
`[2024-07-01, 2024-08-01)` (22 trading dates). A digest move is a reviewed change
(re-pin + note why).

## Known limitations (carried as caveats, backlogged to P4)

- **Coverage is a 3-symbol smoke** (`AAPL` + 2). S1-6's databento-universe expansion
  crawl was **deferred** this sprint; the selection rule (top-N≈500 by trailing-21d
  dollar-volume) + the one-command polite resume are recorded in
  [`data-ingestion-reference.md`](data-ingestion-reference.md) §6.1. S2 runs on the
  smoke subset until the crawl runs.
- **Survivorship bias** — the security master is **listed-only** (Nasdaq Trader active
  symbols); a delisted name never appears, which flatters backtests. Documented, not
  fixed (a delisting-event source is P4).
- **No dividend cash-flow sim** — dividends are folded into the return series
  (total-return), correct for *signal + relative PnL*; absolute-NAV cash-flow accounting
  is P4.
- **GICS is sparse** (licensed); SEC SIC is the open fallback. Some `sector` values are
  the `-1` sentinel.
- **As-built read path:** `build_real_panel` reads the databento parquet **directly**
  (via `atx::core::io::read_parquet`), not through `build_dated_segments`/
  `attach_segment_panel` — those seams hard-code `data.parquet` + an i64 fixed-point
  read the real f64 hive doesn't satisfy. `seg_cache_dir` is reserved but unused.

## Where the numbers live

Per-unit commits, test counts, tolerances, and the as-built divergences are in the
ledger [`sprint-1-progress.md`](sprint-1-progress.md); the module status table is in
[`ROADMAP.md`](ROADMAP.md). The two reference maps are
[`alpha-pipeline-reference.md`](alpha-pipeline-reference.md) (the mine→prove pipeline)
and [`data-ingestion-reference.md`](data-ingestion-reference.md) (the ingestion stack).
