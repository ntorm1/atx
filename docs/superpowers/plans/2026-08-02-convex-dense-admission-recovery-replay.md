# Convex-dense strict-recovery replay: stratified-sample results on 2019-2026 SPY backfill rejections

**Title note: this is a STRATIFIED-SAMPLE replay, not the full 181-cell
census.** The full-census replay (Tasks 1-3's fix, rebuilt against every one
of the 2019-2026 SPY backfill's 181 real production rejections) was
deliberately cut short by explicit coordinator instruction partway through,
with **12 of 73 unique chunks completed (all of calendar year 2019, covering
49 of the 181 cells)** at kill time. The coordinator then directed a smaller
~25-cell stratified sample (by mask and year) plus a 2-date control set,
scored with the acceptance gates advisory rather than pass/fail. Both the
completed 2019 full-year data and the stratified sample are reported below;
neither should be read as a verdict on the full 181-cell census, which
remains only ~27% replayed (49/181 cells with full-census-level coverage).

Full process detail, commands, and every gap/adaptation encountered:
`.superpowers/sdd/2026-08-02-convex-dense-admission-recovery/task-4-report.md`.

## What was replayed

`atx-vol/tools/run_surface_db_backfill.py --phase build` (the actual
production orchestrator, invoked directly rather than hand-copied from
`orchestrator.log` — see the working report for why) against the read-only
`C:/atx-data/opra-hive`, using `build-rel/bin/atx-vol-surface-db-build.exe`
built from this branch (with the Task 1-3 strict-recovery fix, HEAD
`b21737c`), writing into throwaway roots under
`C:/atx-data/surface-db/recovery-replay/spy-{2019..2026}` and logs under
`C:/atx-data/logs/recovery-replay/`. No production path was ever written to.

- All 12 calendar-month chunks of 2019 were replayed in full (not just the
  failed dates in them) before the driver was killed.
- 19 additional single-date invocations filled out the stratified sample for
  years 2020-2026 plus one 2026 control date.

## Stratified sample: 24 failed cells + 2 controls

Sampled by mask and year per the coordinator's spec (~12×2080, ~5×2064,
~3×32, both 2096 cells, ~2×16; several 2019, some 2024-2026):

| date | year | mask | source |
|---|---|---|---|
| 2019-02-20, 2019-03-28, 2019-04-11 | 2019 | 2080 | harvested (completed chunk) |
| 2019-03-18 | 2019 | 2064 | harvested (completed chunk) |
| 2019-01-29 | 2019 | 32 | harvested (completed chunk) |
| 2019-01-30 | 2019 | 16 | harvested (completed chunk) |
| 2020-01-06 | 2020 | 2080 | fresh |
| 2021-03-16 | 2021 | 2080 | fresh |
| 2022-03-16 | 2022 | 2080 | fresh |
| 2023-02-03, 2023-03-02 | 2023 | 2080 | fresh |
| 2024-01-18, 2024-02-01 | 2024 | 2080 | fresh |
| 2025-01-02 | 2025 | 2080 | fresh |
| 2026-01-22 | 2026 | 2080 | fresh |
| 2022-04-25 | 2022 | 2064 | fresh |
| 2023-04-03 | 2023 | 2064 | fresh |
| 2024-01-16 | 2024 | 2064 | fresh |
| 2025-01-29 | 2025 | 2064 | fresh |
| 2020-01-07 | 2020 | 32 | fresh |
| 2022-03-22 | 2022 | 32 | fresh |
| 2025-10-22 | 2025 | 2096 | fresh (both 2096 cells in the full census) |
| 2026-05-20 | 2026 | 2096 | fresh (both 2096 cells in the full census) |
| 2020-01-29 | 2020 | 16 | fresh |

Control set (previously-successful dates, absent from the failed-cell list):
`2019-01-03` (harvested), `2026-07-01` (fresh).

## Recovery — per mask (sample)

| mask | sample n | recovered | rate |
|---|---|---|---|
| 2080 | 12 | 12 | 100.0% |
| 2064 | 5 | 5 | 100.0% |
| 32 | 3 | 3 | 100.0% |
| 2096 | 2 | 2 | 100.0% |
| 16 | 2 | 1 | 50.0% |
| **TOTAL** | **24** | **23** | **95.8%** |

Calendar masks {2080, 32, 2096}: **17/17 = 100%**.
Butterfly masks {2064, 16}: **6/7 = 85.7%** (butterfly recovery is
best-effort by design per the brief, expected to lag calendar recovery).

*Advisory gate check (sample, not census): >=60% overall — met (95.8%).
>=75% on calendar masks — met (100%).*

## Recovery — per year (sample)

| year | sample n | recovered | rate |
|---|---|---|---|
| 2019 | 6 | 5 | 83.3% |
| 2020 | 3 | 3 | 100.0% |
| 2021 | 1 | 1 | 100.0% |
| 2022 | 3 | 3 | 100.0% |
| 2023 | 3 | 3 | 100.0% |
| 2024 | 3 | 3 | 100.0% |
| 2025 | 3 | 3 | 100.0% |
| 2026 | 2 | 2 | 100.0% |

## Residual failures (sample)

One cell in the 24-cell sample did not recover:

- **2019-01-30**, SPY, mask 16 -> still mask 16 (pure butterfly, unchanged).
  `verify --db .../spy-2019 --from 2019-01-30 --to 2019-01-30` reports 0
  partitions matched, confirming nothing was written — a genuine residual
  rejection, not a scoring artifact.

## Regression check

**No regressions** in the sample: no residual failure now carries a mask bit
outside the allowed geometric-failure union {16, 32, 2048, 2064, 2080, 2096}.

Bonus (informational, from the completed-but-not-sampled 2019 cells): one
cell outside the formal 24-cell sample, `2019-08-01`, changed from mask 32
to mask **48** (= 32|16 — calendar recovered, a butterfly violation now also
shows on the same cell). 48's bits are entirely inside the allowed union, so
this is **not** a regression by the brief's definition, but is flagged here
for visibility.

## Bonus: full 2019 calendar year (49/181 census cells, not part of the formal sample)

All 12 of 2019's original chunks completed before the driver was killed,
covering every 2019 cell in the 181-cell census (not just the 6 curated into
the sample above):

| mask | 2019 n | recovered | rate |
|---|---|---|---|
| 2080 | 36 | 36 | 100.0% |
| 2064 | 4 | 4 | 100.0% |
| 32 | 6 | 5 | 83.3% |
| 16 | 3 | 2 | 66.7% |
| **TOTAL** | **49** | **47** | **95.9%** |

Not recovered: `2019-01-30` (mask 16, as above) and `2019-08-01` (mask
32->48, as above).

## Control set outcome

Both control dates fit and admit exactly as before (present in the replay
DB, absent from the failure section, `verify`/`query` both clean):

- `2019-01-03` — OK.
- `2026-07-01` — OK (`cells_ok=1, cells_failed=0`).

No regression on previously-healthy production dates in this (small)
control set.

## Wall-clock cost

- Release build (one-time): ~9 min.
- Full-census driver, 12/73 chunks completed before being killed: ~189s.
- Stratified-sample driver, 19 new single-date invocations: 147.4s.
- Total replay compute spent: ~336s (~5.6 min), plus the one-time build.

## Throwaway artifacts (left on disk for inspection)

- `C:/atx-data/surface-db/recovery-replay/spy-2019` — fully populated
  (entire calendar year).
- `C:/atx-data/surface-db/recovery-replay/spy-2020` .. `spy-2026` — sparse,
  holding only the sampled/control dates.
- `C:/atx-data/logs/recovery-replay/` — `failed_cells.csv` (full 181-cell
  census), `replay_manifest.csv` (every invocation with exit code and
  duration), per-invocation `build_*.csv`/`stdout`/`stderr`/`year_summary_*`,
  and the scored outputs `sample_recovered.csv` / `sample_not_recovered.csv`
  (the formal sample) and `recovered.csv` / `not_recovered.csv`
  (full-census scorer output — informational only, since the census was not
  completed).

## Bottom line

On the portion actually replayed against the fixed binary — a full year of
real 2019 production rejections (49 cells) plus a mask/year-stratified
25-cell sample of the rest (2020-2026) — the strict-recovery mechanism from
Tasks 1-3 recovers the large majority of real geometry-driven admission
rejections (95.8% sample-wide, 100% on calendar masks, 85.7% on butterfly
masks), with zero regressions to a mask outside the expected geometric
family and no impact on previously-healthy dates. This is evidence in favor
of the fix, not a substitute for completing the full 181-cell census, which
was deliberately not finished in this pass.
