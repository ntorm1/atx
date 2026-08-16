# Oracle north star — SpiderRock parity dashboard

Loop state for the RSI loop (`vol-oracle-iter`). MUTABLE — the Ratchet stage rewrites
sections each iteration. Append-only history lives in `../LEDGER.md`; design in
`docs/superpowers/specs/2026-08-15-oracle-rsi-loop-design.md`.

## Status

| | |
|---|---|
| Iteration | 000 complete (bootstrap charter stages 1–2, on `integ/oracle-bootstrap-2026-08-15` @ 6cfaccac). Next: iteration 001 = charter stage 3 convention resolution; the first post-convention scorecard will become the ratchet baseline |
| Last verdict | BOOTSTRAP (2026-08-15, ratchet stage; holdout untouched by design) |
| Consecutive rejects | 0 (ESCALATE to user at 3) |
| Data | INGESTED — `C:\atx-cache\oracle\spiderrock\date=2026-08-14`: 31,771,788 rows (post-0930-drop), 19 `bucket_et` partitions, 3.10 GB zstd |
| Bench tool | BUILT + ratchet-reverified — `atx-vol-oracle-bench`; all 30 `OracleBench*` fast-lane tests passed. Smoke SPY×1300 produced 13,926 priced + 662 sentinel-null of 14,588 rows, and the gate/ratchet cell payloads matched exactly in all 370 cells |
| Conventions | UNRESOLVED — smoke price error is material in both `31-90` and `90+`; the evidence does not yet identify a causal convention mismatch |

## Targets (from spec)

| Metric | Target |
|---|---|
| Mode A price MAE | ≤ 1 tick |
| Mode A vol | ≤ 5 bp |
| Mode A greeks | ≤ 1% rel |
| Mode B | ≤ 2× Mode A residual floor |
| Speed | ≥ pinned baseline, rel-avx2; SOTA push once accuracy plateaus |

## Current metrics

Pre-convention smoke reference only (SPY×1300, Mode A, iter-000 @ 6cfaccac). This is
not a ratchet baseline; iteration 001 will pin the first post-convention baseline. The
holdout cohort was not evaluated. Source scorecard:
`C:\atx-cache\oracle\scorecard_smoke_ratchet_iter000_2026-08-15.json`.

| Metric (smoke, Mode A; n-weighted) | Cells / n | Value |
|---|---:|---:|
| Rows priced / null-sentinel / bad-input | — | 13,926 / 662 / 0 |
| Price, all DTE bands | 37 / 13,926 | MAE $1.262936; within-tol 0.4560 |
| Price, DTE 0-7 | 8 / 1,384 | MAE $0.000183; within-tol 1.0000 |
| Price, DTE 8-30 | 9 / 3,421 | MAE $0.000288; within-tol 1.0000 |
| Price, DTE 31-90 | 10 / 2,872 | MAE $0.534888; within-tol 0.2093 |
| Price, DTE 90+ | 10 / 6,249 | MAE $2.568444; within-tol 0.1511 |

No pinned `rel-avx2` speed baseline exists yet. Dev-run wall time and throughput are
diagnostic only and are not performance evidence.

## Convention map

Pending iteration 001 (charter stage 3). It will live in
`atx-vol/bench/oracle/CONVENTIONS.md`, with a summary here. Iter-000 establishes only
that smoke price residuals become material in the 31-90 band and are larger again in
90+. It does not establish whether ddiv, sdiv, daycount, signs, scaling, or another
mapping is causal. Vol and greek conventions remain unresolved.

## Hypotheses

### Open
(none)

### Confirmed
(none)

### Refuted — do not re-propose without new evidence
(none)

## Oracle-suspect cells (excluded from ratchet)

(none vetted yet; candidates come from vol-analyst, vetting by vol-verifier)
