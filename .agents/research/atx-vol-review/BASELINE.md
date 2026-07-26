# atx-vol SOTA sprint — BASELINE (vega-flat SPY dispersion surface backtest)

Purpose: pinned pre-sprint baseline for the **price+risk throughput** hot path
(`run-surface-backtest`, straddle-book family). Fits are done once in `build-corpus`;
the backtest reloads cached `.atxvsa` surfaces and exercises **price + risk only** (American
Andersen–Lake solves per query). Established 2026-07-19.

Exe (prebuilt rel-avx2, run directly from Git Bash, no MSVC env needed):
`C:/atx-wt/wt-bt-sota/build-rel-avx2/bin/atxvol_spy_dispersion_backtest.exe`

---

## PnL to pin (assert byte-identity after the sprint)

```
surface-only projected backtest complete: dates=82 final_nav=247.4065016
```

- **final NAV (full precision): `247.4065016443293`**  (surface_backtest.tsv, last row, field 17)
- entry-day NAV (2026-01-02): `-0` (0.0)
- steps (dates): **82**; open lots/step: **22** (1 SPY index straddle + 10 name straddles × call+put)
- Byte-identical final_nav reproduced across 4 consecutive runs.

Regression check after the sprint:
```
run-surface-backtest --run C:/atx-data/spy-dispersion/runs/bt-sota-baseline
# MUST print final_nav=247.4065016 (full precision 247.4065016443293), dates=82
```

---

## Working recipe (exact commands)

Inputs were fixed in a scratch dir (source NOT edited). Two fixes vs. the stock example:
1. **Universe TSV converted to LF line endings.** `read_universe` (dispersion_workflow.cpp:181)
   compares the header via `text.substr(0, first_end)` where `first_end` is the `\n` index — this
   keeps the trailing `\r` of a CRLF file, so a CRLF header never matches the literal and the build
   dies with **"bad universe schedule header"**. (Row parser strips `\r`; the header check does not.
   run_spec reader strips `\r`, so the spec may stay CRLF.)
2. **`occ_ess_root` dropped from the run spec.** `build_corpus_command` → `persist_occ_ess_evidence`
   requires an occ-ess `.txt` for *every loaded date*, but `C:/atx-data/spy-dispersion/occ-ess`
   only covers 3 dates (2026-01-02/05/06). occ-ess is **not** read by `run-surface-backtest` and does
   **not** enter the fit/corpus (evidence-only), so dropping it enables the full Jan–Apr range without
   changing any surface or the PnL.

Scratch inputs: `C:/atx-data/spy-dispersion/scratch-bt-sota/{run_spec.tsv, universe.tsv}`
(universe = 10-name equal-weight proxy: AAPL AMZN AVGO LLY GOOGL JPM META MSFT NVDA XOM; SPY added by the driver).

```bash
EXE="/c/atx-wt/wt-bt-sota/build-rel-avx2/bin/atxvol_spy_dispersion_backtest.exe"
SCRATCH="/c/atx-data/spy-dispersion/scratch-bt-sota"
OUT="/c/atx-data/spy-dispersion/runs/bt-sota-baseline"

# 1. POPULATE (fit hot path; ~902 surfaces fitted, HFT preset / LinearVariance / calendar floor)
"$EXE" build-corpus --spec "$SCRATCH/run_spec.tsv" --out "$OUT"
#  -> built qualified corpus: admitted=902 quarantined=0 source_failed=407

# 2. BENCHMARK (price+risk throughput on cached fits)
"$EXE" run-surface-backtest --run "$OUT"
#  -> surface-only projected backtest complete: dates=82 final_nav=247.4065016
```

The run dir is self-contained/re-runnable (`run_spec.tsv`, `universe_schedule.tsv`,
`surface_manifest.tsv`, `archives/` all copied in; no occ-ess/definitions dependency for the benchmark).

---

## Corpus

- Location: **`C:/atx-data/spy-dispersion/runs/bt-sota-baseline`**
- Range 2026-01-02 … 2026-04-30, universe = 10 names + SPY.
- `admitted=902  quarantined=0  source_failed=407` (source_failed = weekend/absent OPRA cells).
- 85 date archives (`archives/*.atxvsa`); qualified clock feeds 82 backtest steps.

---

## Throughput baseline (run-surface-backtest)

Wall times over 4 runs (host was busy with other builds — **PROVISIONAL**):
`0.665, 0.745, 0.775, 0.786 s`  →  best **0.66 s**, median **~0.76 s**.

Host-noise-insensitive op counts (the reliable regression metric):

| Metric | Value |
|---|---|
| Backtest steps (dates) | **82** |
| Lots priced+risked / step | **22** (11 straddles × 2 legs) |
| **Total lot-repricings** (Σ n_open_lots) | **1,804** |
| American solves (report A4: dual full-book pass/day) | ≈ 2× base ≈ **~3,608** base solves + shifted-scenario solves for pnl_totals |

Derived throughput (from provisional wall time):

| | best (0.66 s) | median (0.76 s) |
|---|---|---|
| lot-repricings / s | ~2,714 | ~2,375 |
| surface-steps / s | ~123 | ~108 |

**(dates × surfaces × contracts) repriced = 82 steps × 22 contracts = 1,804 contract-steps** per full
backtest — this count is fixed by the data/config and must stay identical after the sprint (same PnL).

---

## Counters — BLOCKER

The prebuilt exe was **NOT** compiled with `-DATX_VOL_PROFILE` / `-DATX_VOL_COUNTERS`
(no `backtest_counters`/`backtest_profile` strings in the binary; no `backtest_counters.tsv` /
`backtest_profile.tsv` emitted). Per-op counter/phase-profile metrics are therefore **unavailable**
from this binary. To capture them, a rebuild with those flags is required (out of scope here —
READ-ONLY on source/build). Use the op counts above (1,804 lot-repricings, 82 steps) as the
host-insensitive throughput anchor until a profiled build exists.
