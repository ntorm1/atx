# Saved-blend TRAIN export and fixed turnover-policy comparison (2026-09-26)

Evidence archive: `saved-blend-policy-20260926/` (60 copied files, 1,782,358 bytes,
`sha256-index.json` also hashes the dense TRAIN blend payloads that were not copied).

## Build and native qualification

- Imports: VM arena release `8527a839` -> `050c0efc`; ascending/maximum-first arena
  fixture `23f1541b` -> `bfb6b859`. Pool-3 v3/v4 archive `58c21bb0` -> `470eb1b6`.
- Helper `build-recent-strategy-targets-v1.ps1`: source = configured provenance
  `bfb6b859`, Jobs 2 (2,198 MiB free), 36.62 s, six CPPs and five links (target
  replay, provenance, target CLI, IC runner, both test TUs). No PCH/dependency rebuild.
- `atx-impl-strategy-ic-tests`: 35/35 pass in 5.141 s (prior 34 + arena case).
  `atx-impl-strategy-target-tests`: 7/7 pass in 0.198 s. Bounded receipts retained.

## TRAIN saved-blend export (deterministic repeat, not new strategy ideas)

- v5 (512 MiB-free floor not yet applied): guard `system-memory-limit` at candidate 11,
  peak 894,599,168 B, host free 793,882,624 B. The arena log shows release before
  growth. The stop came from host memory held by other processes. Preserved.
- Ruling: real-run guard floor 768 -> 512 MiB free (RSS 1536 MiB, 180 s unchanged),
  per the owner's instruction that RAM limits must not stall progress.
- v6 complete: 94.703 s wall, peak 898,715,648 B, executable
  `036078905d7b38a438024242421876116552f48d04a925a74f6dd35262d5f4f9`.
  Stages: VM 30.01 s, IC 30.66 s, composition 15.08 s, load 9.44 s, labels 1.66 s,
  save 4.35 s (v2 full TRAIN was 131.67 s).
- Parity with original v2: canonical orientation array `4a3e8004...` identical (48
  signs frozen); `train_planned_targets.csv` and `train_daily_ic.csv` byte-identical;
  candidate JSONL differs only in timing fields; summary differences are timing,
  resource budget, recipe/config and status fields only.
- Saved TRAIN blend manifest `train_combined.json` SHA256
  `51740eff34bc3a74f42ea028b45282117c92c8f32ed029de31cd6500471363dc`.

## Fixed policy comparison (6 bps one-way, 300 bps annual borrow; hypothetical)

Audit `audit-target-replay.py` (SHA `d8e5fb27...`, independently reviewed and corrected
before use): **pass**, 2,268 exact f64 baseline comparisons against the original planned
targets, zero textual differences; month reconciliation, declared costs, budget breach
disclosure and NaN missing-day handling verified.

| Metric | TRAIN baseline | TRAIN budget-v2 | VAL baseline | VAL budget-v2 |
| --- | --- | --- | --- | --- |
| Mean monthly one-way target change | 35.32% | 30.66% | 34.44% | 30.24% |
| Max monthly | 80.08% | 36.69% | 70.60% | 31.31% |
| Forced exits (total) | 1.161 | 1.056 | 0.558 | 0.512 |
| Mean gross | 0.901 | 0.847 | 0.906 | 0.853 |
| Max abs net | 0.0645 | 0.0578 | 0.0191 | 0.0255 |
| Complete / mature return days | 253/754 | 253/754 | 180/500 | 180/500 |

Monthly means above 30% are disclosed forced-exit breaches; discretionary change is
within budget. Validation uses the TRAIN-frozen policy with no sign, weight or
parameter tuning.

## Interpretation limits

- Target proxy: no drift, fills, NAV or capacity. Two thirds of days contain at least
  one missing held return (up to 232 names per day in TRAIN), so complete-day sums
  cover a non-random subset. No full-period Sharpe is claimed.
- Root diagnostic, not claimable: all-days observed-component gross annualized ratio is
  about 0.36 (TRAIN, in-sample signs) and 0.34-0.38 (validation), about 1.9%/yr at
  ~5% volatility in validation. The 300 bps borrow scenario is ~1.25%/yr, so the net
  result is near zero. The blend has negative market beta (-0.12 TRAIN, -0.20
  validation). **The Sharpe >= 1 objective is not met.**
- Next: a NAV backtest with a declared missing-price policy, ex-ante risk
  neutralization and alpha expansion, all selected on TRAIN only. 2025+ remains unused.
