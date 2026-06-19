# atx-impl RUNBOOK — Full 10-Year Universe Operator Guide

This runbook documents how to execute the `atx-impl` equity-alpha pipeline at
full scale against the real ~3.3 GB ORATS zip (~17 M rows, 1 621 dates, ~20 k
names). All commands and flag names are verified against
`atx-impl/src/config.hpp`, `atx-impl/src/config.cpp`,
`atx-impl/src/dispatch.cpp`, and `atx-impl/src/stage_run.cpp`.

---

## 1. Overview

### Six stages

The pipeline consists of six sequential stages, each exposed as a CLI
subcommand:

| # | Subcommand | Input | Output |
|---|------------|-------|--------|
| 1 | `load`     | ORATS `.zip` | `segs/` directory of `.seg` segment files |
| 2 | `panel`    | `segs/`      | `panel.bin` universe panel |
| 3 | `discover` | `panel.bin`  | `alphas/` directory of `.dsl` alpha files |
| 4 | `combine`  | `panel.bin` + `alphas/` | `combo.bin` blended signal |
| 5 | `optimize` | `panel.bin` + `combo.bin` | `books.bin` portfolio books |
| 6 | `report`   | `panel.bin` + `books.bin` | `report/` directory of TSV/CSV/TXT files |

### Two CLI shapes

**Staged subcommands** — run each stage individually, writing artifacts to a
persistent work directory. This is the recommended workflow for full-scale runs
because expensive stages (especially `discover`) can be skipped on re-runs when
only a downstream stage's config changes.

```
atx-impl load    --zip <zip> --out <work>/segs [--min-date YYYY-MM-DD]
atx-impl panel   --segs <work>/segs --panel-out <work>/panel.bin [...]
atx-impl discover --panel <work>/panel.bin --alpha-out <work>/alphas [...]
atx-impl combine  --panel <work>/panel.bin --alphas <work>/alphas \
                   --combo-out <work>/combo.bin [...]
atx-impl optimize --panel <work>/panel.bin --combo <work>/combo.bin \
                   --books-out <work>/books.bin [...]
atx-impl report   --panel <work>/panel.bin --books <work>/books.bin \
                   --report-out <work>/report [...]
```

**`run` wrapper** — a single command that chains all six stages through a work
directory in one invocation. Useful for a clean end-to-end run from a config
file without manual stage orchestration.

```
atx-impl run --config run.cfg --zip <zip> --out <work>
```

### The staged == run guarantee

Both shapes call the same underlying `run_load`, `run_panel`, `run_discover`,
`run_combine`, `run_optimize`, and `run_report` functions. The `run_all`
wrapper in `atx-impl/src/stage_run.cpp` wires them through the same work-dir
path layout that the staged commands use, so per-stage digests are identical
regardless of which shape is used. This guarantee is enforced by the
`StagedEqualsRun` test in `atx-impl/tests/e2e_pipeline_test.cpp` (S6 suite
`AtxImplE2E`).

---

## 2. Dataset

The full ORATS dataset is a single `.zip` archive containing daily option-chain
rows for the US equity universe. Expected characteristics:

- **Size on disk:** ~3.3 GB compressed
- **Row count:** ~17 million rows
- **Trading dates:** 1 621 dates (approximately 10 years of history)
- **Universe size:** ~20 000 names

The archive is referenced via the `--zip` flag to `load` (or to `run`). It is
not checked into the repository; the operator must supply its path:

```
atx-impl load --zip /data/orats/orats_full.zip --out /run/segs
```

The CI fixture used by the automated tests is a synthetic 10-instrument ×
100-date ORATS zip generated in-process. It exercises the identical code path
— the difference is config-scaled inputs only, with no code-path fork.

---

## 3. Staged (Cache-and-Resume) Workflow

This is the recommended procedure for full-scale runs. Run each subcommand in
order, writing artifacts to a single persistent work directory (`<work>`). If
you later change only a downstream stage's configuration (e.g. `--gross` for
`optimize`, or combine method), you can skip re-running `discover` — the
expensive search — by reusing the `alphas/` directory it already produced.

Pick a persistent work directory that survives across sessions:

```bash
WORK=/data/atx-run
ZIP=/data/orats/orats_full.zip
```

### Stage 1 — load

Inflate the ORATS zip into per-date segment files.

```
atx-impl load \
    --zip  $ZIP \
    --out  $WORK/segs \
    --min-date 2013-01-01
```

- `--zip` — path to the ORATS archive (required).
- `--out` — output directory for `.seg` files; `run_all` uses `<work>/segs`.
- `--min-date` — discard rows with a trade date before this value (ISO 8601).
  Omit to admit all dates.

Expected output line:

```
[atx-impl] stage=load digest=<hex16> rows=<n> segs=<n>
```

### Stage 2 — panel

Filter and screen the loaded segments into a binary universe panel.

```
atx-impl panel \
    --segs      $WORK/segs \
    --panel-out $WORK/panel.bin \
    --start     2014-01-01 \
    --end       2023-12-31 \
    --min-adv-usd  1000000 \
    --top-n-by-adv 500
```

- `--segs` — path to the `segs/` directory written by `load`.
- `--panel-out` — output path for `panel.bin`; `run_all` uses `<work>/panel.bin`.
- `--start` / `--end` — ISO 8601 date window (inclusive). Omit for all dates.
- `--min-adv-usd` — exclude names whose average daily dollar volume is below
  this threshold (USD). See §5 for recommended values.
- `--top-n-by-adv` — after the ADV floor, keep only the top-N names by ADV.
  Set `0` to disable the cap.

Expected output line:

```
[atx-impl] stage=panel digest=<hex16> dates=<n> names=<n>
```

### Stage 3 — discover

Genetic search for alpha expressions. This is the dominant compute stage.

```
atx-impl discover \
    --panel    $WORK/panel.bin \
    --alpha-out $WORK/alphas \
    --seed      42 \
    --population 200 \
    --generations 50 \
    --seed-expr "rank(close)" \
    --seed-expr "ts_mean(close,5)"
```

- `--panel` — path to `panel.bin` from stage 2.
- `--alpha-out` — output directory for `.dsl` alpha files; `run_all` uses
  `<work>/alphas`.
- `--seed` — unsigned integer seed for the genetic search (determines
  reproducibility; see §7).
- `--population` — number of genomes per generation.
- `--generations` — number of generations to evolve.
- `--seed-expr` — repeatable flag; each occurrence adds one seed expression to
  the initial population. Provide at least one.
- `--min-dsr` — deflated-Sharpe admission bar. **No effect on the default
  (ungated) path** — that path emits the search's top-N survivors by raw fitness
  and exposes no per-genome deflated Sharpe. Under `--gated` (below) it becomes
  the live S1 anti-snooping floor: a candidate is admitted only if its deflated
  Sharpe `>= --min-dsr`.

Expected output line:

```
[atx-impl] stage=discover digest=<hex16> admitted=<n>
```

#### Gated discovery (`--gated`) — robust, low-turnover, low-correlation DB

By default `discover` writes the search's top-N genomes by raw fitness with no
quality screen. Passing `--gated` instead routes every distinct candidate through
`factory::Factory::mine_into`: candidates are ranked by **deflated Sharpe** (the
multiple-testing-corrected statistic) and admitted into a persistent on-disk
`library::Library` (a queryable alpha database at `<alpha-out>/_library/`, a
SQLite catalog + lifecycle/dedup DBs + an `.alib` segment carrying each alpha's
PnL/positions) only if the candidate clears the `AlphaGate` floors **and**
`dsr >= --min-dsr`. Admitted alphas are also written as `<alpha-out>/alpha_NNN.dsl`
(best-deflated-first) so `combine` consumes them unchanged, and a metric-annotated
`<alpha-out>/_manifest.txt` is produced.

The gate encodes the desired alpha qualities:

- `--min-sharpe` (default 1.0) — standalone-Sharpe floor.
- `--min-fitness` (default 1.0) — WorldQuant fitness floor (**high fitness**).
- `--max-turnover` (default 0.70) — per-alpha turnover cap (**low turnover**).
- `--max-pool-corr` (default 0.70) — reject if `|corr|` to any already-admitted
  alpha exceeds this (**low correlation** / mutual diversity).
- `--min-dsr` (default 0.0) — deflated-Sharpe floor (**robustness** under
  multiple testing; combined with the CPCV out-of-sample folds the search uses).
- `--target-aum` (default 0.0; e.g. `100000000`) — when `> 0`, activates the
  ADV-aware capacity cost objective in the search fitness (**high capacity**).
- `--workers` (default 0 = auto = cores−1) — search parallelism. Digest-invariant
  (affects speed/memory, never bits). Each worker holds a full per-genome buffer;
  on a wide panel with limited RAM, cap it (e.g. `--workers 5`) to bound peak
  memory.

The default gate floors (`min-sharpe`/`min-fitness` = 1.0) are WorldQuant
production bars; on a frictionless research sim they may admit zero, so calibrate
to the data (e.g. `--min-sharpe 0.4 --min-fitness 0.15`) and tighten as warranted.

Gated output line (additional keys):

```
[atx-impl] stage=discover digest=<hex16> gated=1 admitted=<n> evaluated=<n> \
    duplicates=<n> reject_hist=<a,b,c,d,e,f> factory_digest=<hex16> ...
```

### Stage 4 — combine

Blend admitted alphas into a single combined signal.

```
atx-impl combine \
    --panel    $WORK/panel.bin \
    --alphas   $WORK/alphas \
    --combo-out $WORK/combo.bin \
    --method   equal \
    --fit-begin 0 \
    --fit-end   0
```

- `--panel` — path to `panel.bin`.
- `--alphas` — path to `alphas/` directory from stage 3.
- `--combo-out` — output path for `combo.bin`; `run_all` uses `<work>/combo.bin`.
- `--method` — combination method (e.g. `equal`).
- `--fit-begin` / `--fit-end` — fit window bounds (integer date indices;
  `0` = default full window).

Expected output line:

```
[atx-impl] stage=combine digest=<hex16>
```

### Stage 5 — optimize

Optimize portfolio book weights from the combined signal.

```
atx-impl optimize \
    --panel           $WORK/panel.bin \
    --combo           $WORK/combo.bin \
    --books-out       $WORK/books.bin \
    --risk-aversion   1.0 \
    --turnover-penalty 0.5 \
    --gross           1.0 \
    --name-cap        0.05 \
    --rebalance       weekly
```

- `--panel` — path to `panel.bin`.
- `--combo` — path to `combo.bin` from stage 4.
- `--books-out` — output path for `books.bin`; `run_all` uses `<work>/books.bin`.
- `--risk-aversion` — mean-variance risk aversion coefficient.
- `--turnover-penalty` — penalty on per-period turnover.
- `--gross` — gross leverage target.
- `--name-cap` — maximum weight per name (fraction of gross).
- `--rebalance` — rebalance frequency: `daily` or `weekly`.

Expected output line:

```
[atx-impl] stage=optimize digest=<hex16>
```

### Stage 6 — report

Generate the performance report from the optimized books.

```
atx-impl report \
    --panel     $WORK/panel.bin \
    --books     $WORK/books.bin \
    --report-out $WORK/report
```

- `--panel` — path to `panel.bin`.
- `--books` — path to `books.bin` from stage 5.
- `--report-out` — output directory for report files; `run_all` uses
  `<work>/report` (or the value of `--report-out` from the top-level config if
  supplied).

Output artifacts in `report/`:

| File | Content |
|------|---------|
| `pnl.tsv` | Daily PnL decomposition |
| `leverage.tsv` | Daily gross/net leverage |
| `exposure.tsv` | Daily factor exposures |
| `census.tsv` | Daily name-count census |
| `equity_curve.csv` | Cumulative equity curve |
| `summary.txt` | Human-readable performance summary |

Expected output line:

```
[atx-impl] stage=report digest=<hex16>
```

### Digest comparison for cache validation

Each stage emits exactly one digest line to stdout in the format:

```
[atx-impl] stage=<name> digest=<hex16> [k=v ...]
```

Where `<hex16>` is a 16-character lowercase hexadecimal FNV-1a-64 digest over
the canonical artifact bytes. To confirm a re-run reproduced a cached stage
bit-for-bit, capture and compare the digest values:

```bash
# First run — save digests
atx-impl panel --segs $WORK/segs --panel-out $WORK/panel.bin ... \
    | tee panel_run1.log

# After changing a downstream config and re-running panel to verify it is
# unchanged, compare:
diff <(grep 'stage=panel' panel_run1.log) <(grep 'stage=panel' panel_run2.log)
```

No diff output means the stage artifact is bit-for-bit identical to the cached
version — the upstream inputs and config were unchanged.

---

## 4. `run` One-Shot

The `run` subcommand chains all six stages in a single invocation, wiring
artifacts through the work directory automatically:

```
atx-impl run --config run.cfg --zip $ZIP --out $WORK
```

Flags supplied on the CLI override any corresponding keys in `--config`. The
config file is read and merged after CLI parsing; CLI-present flags always win.

**When to prefer `run`:** clean end-to-end runs where no stage caching is
needed and a single config file fully specifies the run.

**When to prefer staged:** when iterating on a late stage (e.g. tuning
`optimize` parameters) and you do not want to re-run `discover` (the dominant
cost). Run stages 1–3 once, cache `alphas/`, then iterate stages 4–6.

### Sample config file

The config file format is newline-separated `flag=value` pairs. Lines beginning
with `#` are comments; blank lines are ignored. The `flag` name is exactly the
CLI flag name without the leading `--`. The `subcommand` key is ignored when
the file is supplied via `run --config`; the effective subcommand is always
`run`.

```ini
# run.cfg — full 10-year universe run
zip=/data/orats/orats_full.zip
out=/data/atx-run
min-date=2013-01-01

# panel screening
start=2014-01-01
end=2023-12-31
min-adv-usd=1000000
top-n-by-adv=500

# discovery
seed=42
population=200
generations=50
seed-expr=rank(close)
seed-expr=ts_mean(close,5)
# min-dsr=0.5  # reserved no-op — not yet enforced (see §3 Stage 3 notes)

# combine
method=equal
fit-begin=0
fit-end=0

# optimize
risk-aversion=1.0
turnover-penalty=0.5
gross=1.0
name-cap=0.05
rebalance=weekly

# report (optional override; defaults to <out>/report)
report-out=/data/atx-run/report
```

Note that `seed-expr` is repeatable: each occurrence in the config file adds
one expression to the initial population, exactly as `--seed-expr` does on the
CLI.

---

## 5. Universe Screening for Tractable Search

The `discover` stage runs a genetic search over the full panel. On a ~20 k-name
universe with 1 621 dates the panel is large; narrowing it before search
dramatically reduces per-generation evaluation cost.

### Recommended starting screen

Apply both a liquidity floor and a name-count cap via the `panel` stage flags:

```
--min-adv-usd  1000000     # exclude names with ADV < $1 M/day
--top-n-by-adv 500         # keep the 500 most liquid names after the floor
--start        2014-01-01  # 10-year window (adjust to taste)
--end          2023-12-31
```

**Rationale:**

- **`--min-adv-usd`** — removes micro/nano caps where signals are hard to
  execute. A $1 M/day floor is a conservative starting point; tighten to
  $5 M–$10 M for a tighter, more tradeable universe.
- **`--top-n-by-adv`** — caps the panel at a tractable size regardless of how
  many names clear the ADV floor. 500 names × 1 621 dates is a reasonable
  target for overnight discovery runs; lower to 200–300 for daytime
  exploratory runs.
- **Date window** — bounding the in-sample window keeps per-generation
  evaluation fast. A 10-year window is appropriate for the full run; use a
  3–5 year window for rapid iteration.

These are recommendations, not requirements. The flags are independent; omit
either to relax that dimension of the screen.

---

## 6. Performance and Resource Notes

All wall-clock and peak-memory cells below are marked **TODO** because the
3.3 GB zip has not been run against this pipeline. Fill these in after the first
full operator run.

| Stage | Bound | Release vs Debug | Notes |
|-------|-------|-----------------|-------|
| `load` | I/O-bound (zip inflate) | Release strongly preferred | The dominant bottleneck is zlib inflate of ~3.3 GB compressed data. The `-DATX_FAST_INFLATE=ON` CMake option (see below) links zlib-ng in `atx-core` and can provide a significant speedup on this stage. Wall-clock: **TODO**. Peak RSS: **TODO**. |
| `panel` | CPU + I/O | Release preferred | Reads all `.seg` files, applies ADV and date filters. Fast relative to load/discover. Wall-clock: **TODO**. Peak RSS: **TODO**. |
| `discover` | CPU-bound (dominant) | Release required | Genetic search over the panel. Each generation evaluates every genome against the full date × name matrix. This is by far the most expensive stage; run it last when iterating on downstream stages. Wall-clock: **TODO**. Peak RSS: **TODO**. |
| `combine` | Memory-bound | Release preferred | Reads panel + all alpha files. Cheap relative to discover. Wall-clock: **TODO**. Peak RSS: **TODO**. |
| `optimize` | CPU + memory | Release preferred | Quadratic portfolio optimization over all dates. Wall-clock: **TODO**. Peak RSS: **TODO**. |
| `report` | I/O-bound | Either | Reads panel + books; writes TSVs. Fast. Wall-clock: **TODO**. Peak RSS: **TODO**. |

### Build configurations

The project ships two build configurations:

- **Debug** (`build/`) — assertions enabled, no optimization. Use for
  development and CI. Adequate for the bounded CI fixture but impractical for
  the 3.3 GB full run in `discover`.
- **Release** — configured with `-DCMAKE_BUILD_TYPE=Release` (or equivalent
  preset). Use for all full-scale operator runs.

To configure a Release build:

```
& "scripts\atx-build.ps1" configure -Groups data -Config Release
& "scripts\atx-build.ps1" build atx-impl
```

### ATX_FAST_INFLATE

The `load` stage's zip inflate can be accelerated by building with the
`-DATX_FAST_INFLATE=ON` CMake option. This option is defined in the root
`CMakeLists.txt` (default OFF) and, when enabled, makes `atx-core`'s
`ZipEntryReader` use zlib-ng instead of the bundled zlib for DEFLATE
decompression. On the 3.3 GB full zip this is the dominant lever for `load`
throughput.

To enable:

```
& "scripts\atx-build.ps1" configure -Groups data -Config Release -CMakeArgs "-DATX_FAST_INFLATE=ON"
```

Note: zlib-ng must be available to the build (via vcpkg or a system install).
Consult `atx-core/CMakeLists.txt` lines 119–125 for the exact linking logic.

---

## 7. Determinism and Reproducibility

### Digest stability

Each stage computes an FNV-1a-64 digest over its canonical output artifact
bytes and emits it as `[atx-impl] stage=<name> digest=<hex16> ...`. The digest
is computed deterministically from the artifact content: given the same inputs
and the same config, a stage re-run on the same machine produces a bit-for-bit
identical artifact and therefore an identical digest.

This means:

- You can confirm a cached stage was not invalidated by comparing the digest
  from the new run against the one recorded from the previous run.
- The `run` one-shot and the staged workflow produce identical per-stage
  digests (verified by `AtxImplE2E/StagedEqualsRun`).

### Discovery reproducibility

The genetic search in `discover` is seeded via `--seed` (an unsigned 64-bit
integer). Given the same `--seed`, `--population`, `--generations`, and
`--seed-expr` values, and the same input `panel.bin`, `discover` produces an
identical `alphas/` directory and therefore an identical `discover` digest.
Changing the seed produces a different (but equally valid) set of alphas.

### Report byte-identity (R8)

The report stage is byte-deterministic: given identical `panel.bin` and
`books.bin`, two runs of `report` produce bit-for-bit identical output files
(`pnl.tsv`, `leverage.tsv`, `exposure.tsv`, `census.tsv`). This is verified by
`AtxImplE2E/ReportBytesDeterministic` (S6 suite).

---

## 8. Troubleshooting

### `discover` admits zero alphas

**Symptom:** `admitted=0` in the `discover` digest line; subsequent `combine`
has no inputs.

**Causes and remedies:**

1. The date window is too narrow. Widen it by moving `--start` earlier or
   `--end` later to include more history.
2. The universe screen is too tight — only a handful of names with insufficient
   cross-sectional spread remain after the ADV filter. Widen with a lower
   `--min-adv-usd` or a higher `--top-n-by-adv`.
3. The seed expressions (`--seed-expr`) do not have signal in the selected date
   window. Try alternative expressions or widen the `--start`/`--end` window to
   include more history.
4. `--population` and `--generations` are too small for the search to converge.
   Increase both.
5. Verify `--seed` is set explicitly so the run is reproducible while
   diagnosing.

Note: `--min-dsr` is currently a reserved no-op and is **not** a lever for
`admitted=0` outcomes — see the flag description above.

### Out of memory during `load` or `discover`

**Symptom:** process OOM-killed or system swap exhaustion.

**Remedies:**

1. Build and run with `-DCMAKE_BUILD_TYPE=Release`. Debug builds carry
   significant memory overhead from assertions and un-optimized data structures.
2. Enable `-DATX_FAST_INFLATE=ON` for the `load` stage — the zlib-ng path has
   lower peak working-set than the miniz path on large zips.
3. Narrow the date window (`--start` / `--end`) to reduce the in-memory panel
   size before `discover`.
4. Reduce `--top-n-by-adv` to cap the number of names carried through
   discovery.

### Digest mismatch on re-run of a cached stage

**Symptom:** you expected a stage to reproduce its cached digest (because you
did not change its config), but the new digest differs.

**Cause:** the upstream artifact changed. Check:

1. Was the upstream stage re-run (intentionally or accidentally) between the
   two runs? Even a byte-identical re-run of an earlier stage should produce
   the same digest, but a config change anywhere upstream propagates.
2. Did the `--seed` change for `discover`? This changes every downstream
   digest.
3. Is the input zip the same file? Verify with a checksum (e.g.
   `sha256sum <zip>`).

### `run` fails with "run: --out (work dir) required" or "run: --zip required"

Both `--out` and `--zip` are mandatory for the `run` subcommand. Ensure they
are supplied either on the CLI or in the `--config` file. CLI flags always
override the config file.

### Stage artifacts missing after a partial run

If a staged run was interrupted (e.g. OOM on `discover`), earlier artifacts
(`segs/`, `panel.bin`) are still present and valid. Resume from the failed
stage. Do not re-run earlier stages unless their config changed; their digests
remain valid as long as the files are present and unmodified.
