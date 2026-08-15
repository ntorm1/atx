# Oracle RSI bootstrap charter

Spec: `docs/superpowers/specs/2026-08-15-oracle-rsi-loop-design.md`. This charter is the
task input to `vol-sprint` when `vol-oracle-iter`'s Measure stage reports missing data or
tooling. Stages run in order; a bootstrap iteration executes ONE stage.

## Stage 1 — data (status: missing_data)

Run `python atx-vol/scripts/oracle_ingest.py --zip <the tbloptionintradayhist zip in
C:\Users\natha\Downloads>` (disk-checks first; ~15 GB transient on the work drive).
Then pick cohorts from the printed top-underlier list / manifest JSON and fill in
`atx-vol/bench/oracle/cohorts/{smoke,tune,holdout}.json` per `cohorts/README.md` rules
(holdout: disjoint underliers AND buckets). Single lane; no C++ changes. Done: manifest
exists, three cohort files validate against README rules, row counts recorded in report.

## Stage 2 — oracle bench tool, Mode A first (status: missing_tooling)

Build `atx-vol-oracle-bench` (C++, `atx-vol/tools/`, wired like the existing tools
targets; Arrow/Parquet from vcpkg):

- Input: cohort JSON → reads the partitioned parquet store (predicate pushdown on
  underlier + bucket_et; never full-file scans).
- **Mode A**: per row, price American with SpiderRock's own inputs (`uPrc, rate, sdiv,
  ddiv, years`, vol = `srVol`) through the atx-vol engine; greeks likewise. Compare to
  `srPrc` and `de ga th ve rh ph vo va deDecay`.
- Convention layer isolated in ONE translation unit (`oracle_conventions.*`) — iteration
  0 rewrites it; the rest of the tool must not care.
- Output: scorecard JSON (schema below) to a `--out` path + rows/s to stderr.
- Mode B (fit from NBBO) is a LATER stage — leave a clean seam, do not stub it.

Scorecard cell keys: `<mode>.<metric>.<moneyness-band>.<dte-band>.<cp>` with
moneyness bands `deep-itm/itm/atm/otm/deep-otm` (0.8/0.95/1.05/1.2 on strike/uPrc) and
dte bands `0-7/8-30/31-90/90+`. Per cell: n, mae, rmse, p50, p95, p99, max,
within_tol_rate. Header: iter, git sha, cohort, mode timings, tolerance definitions.

Done: tool builds target-scoped, runs on the smoke cohort in seconds, scorecard
validates, unit tests for band edges + within-tol accounting + sentinel-null handling.

## Stage 3 — iteration 0: convention resolution

Round-trip their numbers (price(their inputs, srVol) vs srPrc) across candidate
conventions (theta/day vs /year; vega per point; sdiv continuous borrow; ddiv discrete;
years daycount; vo/va = vanna/volga hypothesis; signs; share scaling). Commit the
winning map to `atx-vol/bench/oracle/CONVENTIONS.md` + encode in `oracle_conventions.*`.
Done: residual floor measured and recorded in NORTHSTAR.md + ledger; scorecard iter-000
pinned as the baseline.
