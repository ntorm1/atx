# Oracle north star — SpiderRock parity dashboard

Loop state for the RSI loop (`vol-oracle-iter`). MUTABLE — the Ratchet stage rewrites
sections each iteration. Append-only history lives in `../LEDGER.md`; design in
`docs/superpowers/specs/2026-08-15-oracle-rsi-loop-design.md`.

## Status

| | |
|---|---|
| Iteration | none run |
| Last verdict | — |
| Consecutive rejects | 0 (ESCALATE to user at 3) |
| Data | NOT INGESTED (`C:\atx-cache\oracle\spiderrock\` empty; zip in Downloads) |
| Bench tool | NOT BUILT (`atx-vol-oracle-bench`, charter stage 2) |
| Conventions | UNRESOLVED (iteration 0 pending; residual floor unknown) |

## Targets (from spec)

| Metric | Target |
|---|---|
| Mode A price MAE | ≤ 1 tick |
| Mode A vol | ≤ 5 bp |
| Mode A greeks | ≤ 1% rel |
| Mode B | ≤ 2× Mode A residual floor |
| Speed | ≥ pinned baseline, rel-avx2; SOTA push once accuracy plateaus |

## Current metrics

None — first scorecard pending (iter-000 after bootstrap stages 1-3).

## Convention map

Pending iteration 0. Will live in `atx-vol/bench/oracle/CONVENTIONS.md`; summary row here.

## Hypotheses

### Open
(none)

### Confirmed
(none)

### Refuted — do not re-propose without new evidence
(none)

## Oracle-suspect cells (excluded from ratchet)

(none vetted yet; candidates come from vol-analyst, vetting by vol-verifier)
