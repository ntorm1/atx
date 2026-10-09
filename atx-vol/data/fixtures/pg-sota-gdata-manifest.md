# G-DATA fixtures — real OPRA data for the 2026-07-19 pricing/greeks SOTA sprint

Provenance + manifest for the real-data OPRA fixtures produced by task **G-DATA**
(sprint `atx-vol/sprints/2026-07-19-atx-vol-pricing-greeks-sota-sprint.md`, §3 WS-G).

**The Parquet payloads are intentionally NOT committed** (root `/data/` is
`.gitignore`d, line 16 — same convention as the existing `spy_fit_slices` corpus
in `tests/support/spy_fit_fixture.hpp`). Only this manifest is tracked. Regenerate
the payloads from the on-disk raw OPRA hive with the commands below.

## Cost / spend — TOTAL $0.00

The Databento account is on a **flat-rate / subscription license**: metered
historical OPRA cost is $0. Verified with the free `metadata.get_cost` preflight
**before** each pull (sprint §3 discipline):

| get_cost query (OPRA.PILLAR / cbbo-1m, stype_in=parent) | estimate |
|---|---|
| `SPY.OPT` 2026-07-17 14:00–14:01Z (1 min) | $0.000000 |
| `SPY.OPT` 2026-07-17 13:30–20:00Z (full session, ~390 min) | $0.000000 |
| `XOM.OPT` 2026-07-17 19:55–19:56Z (1 min) | $0.000000 |
| each `pull_opra_universe_batch.py` realized-spend report | $0.0000 |

Budget: ≤ $100 total (hard cap), STOP-and-report if any single pull > $40.
**Neither threshold was ever approached — every pull priced and realized at
$0.0000.** The prior-sprint YTD pull log (`C:/atx-data/spy-dispersion/pull_resume.log`)
independently shows the same `running_spend=$0.0000`.

## Tooling used (existing — no new loader written)

- Puller: `atx-vol/tools/pull_opra_universe_batch.py` (OPRA.PILLAR / cbbo-1m, one
  snapshot minute → per-symbol Parquet hive; free `get_cost` preflight + hard cap).
  Key read from `$DATABENTO_API_KEY` (exported from `C:/atx/.env`, never printed).
- Slicer: `atx-vol/tools/make_fit_slice.py` (raw OPRA day board → loader-schema
  single-minute fit slice; the tool `atx::vol::load_opra_cbbo_parquet` consumes).
- Raw hive (already on disk from prior sprints, outside git):
  `C:/atx-data/spy-dispersion/opra/{SYM}/{DATE}.parquet` (19:55Z pre-close boards,
  YTD 2026; SPY + XOM + 49 dispersion names).

## Fixture 1 — SPY 0DTE session sweep (validates G1 end-to-end)

Real SPY OPRA chain on a session that is itself a SPY expiration (2026-07-17, Fri),
sampled at four intraday minutes to give a **monotone T→0 progression through the
session** on same-day-expiry contracts. True expiry instant = **16:00 ET =
2026-07-17T20:00:00Z** (the PM-settled equity-option convention G1 stamps). Spot is
implied from put-call parity by the loader (no separate equity feed needed).

Path: `data/spy_fit_slices/SPY_2026-07-17T<HHMM>Z.parquet` (probed by
`find_spy_fit_parquet`). Each file = full SPY chain at that minute (34 expiries,
2026-07-17 … 2028-12-15), so the carry solve / fit have the full term structure; the
front (0DTE) expiry is included with hundreds of two-sided quotes.

| file | snapshot (ts, load-bearing) | ET | rows | 0DTE 2-sided | T to 16:00 ET | source |
|---|---|---|---|---|---|---|
| `SPY_2026-07-17T1335Z.parquet` | `2026-07-17T13:35:00Z` | 09:35 | 13984 | 294 (C173/P121) | 0.000732 yr (6.42 h) | **fresh pull** |
| `SPY_2026-07-17T1600Z.parquet` | `2026-07-17T16:00:00Z` | 12:00 | 14025 | 291 (C170/P121) | 0.000457 yr (4.00 h) | **fresh pull** |
| `SPY_2026-07-17T1800Z.parquet` | `2026-07-17T18:00:00Z` | 14:00 | 13958 | 285 (C167/P118) | 0.000228 yr (2.00 h) | **fresh pull** |
| `SPY_2026-07-17T1955Z.parquet` | `2026-07-17T19:55:00Z` | 15:55 | 14023 | 264 (C157/P107) | 0.000010 yr (5 min) | existing hive board |

### For the G1 agent — register in `kSpyFitFixtures` (`tests/support/spy_fit_fixture.hpp`)

G-DATA is data-only and does not edit lib/test code (sprint §4 ownership). Add these
entries (the `snapshot_iso` MUST equal the file's `ts` — the loader uses it to
compute every T and to gate 0DTE/expired expiries):

```cpp
{"0dte-open",  "SPY_2026-07-17T1335Z.parquet", "2026-07-17T13:35:00Z", "0dte/open"},
{"0dte-mid",   "SPY_2026-07-17T1600Z.parquet", "2026-07-17T16:00:00Z", "0dte/midday"},
{"0dte-pm",    "SPY_2026-07-17T1800Z.parquet", "2026-07-17T18:00:00Z", "0dte/afternoon"},
{"0dte-close", "SPY_2026-07-17T1955Z.parquet", "2026-07-17T19:55:00Z", "0dte/preclose"},
```

Regenerate (fresh pulls need `$DATABENTO_API_KEY`; 1955Z is pure local reslice):
```bash
export DATABENTO_API_KEY=...          # from C:/atx/.env
printf 'SPY\n' > /tmp/spy.txt
for HM in 13:35 16:00 18:00; do
  H=${HM/:/}
  python atx-vol/tools/pull_opra_universe_batch.py --symbols-file /tmp/spy.txt \
    --start 2026-07-17 --end 2026-07-17 --snap-utc $HM --out /tmp/stage/$H --cap 5
  python atx-vol/tools/make_fit_slice.py --src /tmp/stage/$H/SPY/2026-07-17.parquet \
    --out data/spy_fit_slices/SPY_2026-07-17T${H}Z.parquet --underlying SPY
done
python atx-vol/tools/make_fit_slice.py \
  --src C:/atx-data/spy-dispersion/opra/SPY/2026-07-17.parquet \
  --out data/spy_fit_slices/SPY_2026-07-17T1955Z.parquet --underlying SPY
```

Note: `find_spy_fit_parquet` probes `data/spy_fit_slices/` and up to `../../../`
plus `C:/atx/data/spy_fit_slices/`. Payloads currently live in the worktree
`data/spy_fit_slices/` (reachable when tests run from a build dir ≤ 3 levels under
the worktree). If a test run cannot see them, copy the four files into
`C:/atx/data/spy_fit_slices/` (the shared dev cache named in `spy_fit_fixture.hpp`).

## Fixture 2 — XOM high-dividend chain near Q2 ex-dividend (discrete-div + G2 dDiv)

Real XOM (high-div single name) OPRA chain, 2026-05-13 pre-close, with the Q2 2026
discrete dividend **upcoming in-horizon** — the intended stress input for the future
discrete-dividend PDE sprint and a G2 dP/dDiv sanity fixture.

Path: `data/xom_opra_cbbo1m_2026-05-13T1955Z.parquet` (`find_opra_parquet`
convention `<sym>_opra_cbbo1m_<DATE>T<HHMM>Z.parquet`, lowercase sym).

- snapshot `2026-05-13T19:55:00Z`; 1351 rows, **1187 two-sided quotes**, 19 expiries
  (2026-05-15 … 2028-12-15). Source: existing hive board (`$0`).
- **Ex-dividend localization (from PCP-implied forward term structure):** the robust
  median forward steps **down −0.337** between the 2026-05-15 and 2026-05-22
  expiries, so XOM's Q2 ex-date falls in the week of **2026-05-18 … 05-22** — i.e.
  the discrete dividend is ~5–9 calendar days ahead of this snapshot and clearly
  in-horizon (front American options carry it). The whole curve sits escrowed-
  dividend-depressed below S·e^{rT}. Cross-check the exact ex-date against OCC
  definitions (`C:/atx-data/spy-dispersion/definitions-*`) in the discrete-div sprint.
- Adjacent daily boards for a before/after-ex pair are on disk at $0:
  `.../XOM/2026-05-13.parquet` (pre, div visible) and `.../XOM/2026-05-18.parquet`
  (post-ex week, front forward already clean).

Regenerate:
```bash
python atx-vol/tools/make_fit_slice.py \
  --src C:/atx-data/spy-dispersion/opra/XOM/2026-05-13.parquet \
  --out data/xom_opra_cbbo1m_2026-05-13T1955Z.parquet --underlying XOM
```

## Not pulled (and why)

- **MO / T** (task's other preferred high-div names): absent from the on-disk OPRA
  hive (only XOM among high-div names is present). XOM is an explicitly-approved
  preferred name and already on disk, so it was used rather than opening a brand-new
  full-symbol pull. MO/T could be added later at $0 (flat-rate) via
  `pull_opra_universe_batch.py` if the discrete-div sprint wants them.
- **Optional hard-to-borrow name day** (priority 4c): skipped. No HTB name is in the
  hive, the two required fixtures are complete, and the borrow-solve stress is not on
  this sprint's critical path. Deferrable to a later pull at $0.
