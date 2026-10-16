# Resume 4: direction reset toward per-expiry fitting (October 16, 2026)

Root tree `C:/atx-wt/pool-33`, branch `feat/vol-resume4-20261016`, base
`5e978fed` (resume3 handoff). Lease RunId `vol-20261016-root4`.

## Evidence that set the direction

- Diagnosis (read-only, dev200 development cohort): the common Markov model's
  error is a systematic low price level caused by ATM volatility stuck under
  resource caps (class B) and by the 0.25 sparse prior (class A). The per-expiry
  quote-slice fitter is far more accurate where it serves, but static admission
  limits it to 16.7% of eligible rows. 59% of eligible rows are refused by caps
  that engineering can lift. See the ledger line of this date and
  `C:/atx-cache/quote-benchmark/diagnosis-20261016/`.
- Research (cited): SpiderRock labels are per-expiry channel-constrained spline
  smiles priced per contract with per-expiry sdiv and forward offset. See
  `C:/atx-cache/quote-benchmark/research-20261016/`.

## Lanes dispatched this resume

| Lane | Branch | Purpose |
| --- | --- | --- |
| bench-e0-queue | none (archived seed-free binary) | E0 byte parity, then frozen R104/R416 queue |
| time-knots-lane | `feat/vol-markov-time-knots2-20261016` | finish the time-knot fitter WIP, migrate `interval_groups()` consumers, gates |
| policy-experiments | `feat/vol-policy-exp-20261016` | JSON-only X1 seed+prior, X2 five knots, X3 per-interval, X4 caps, X5 combo; status-based hybrid composition tool |
| implied-forward-lane | `feat/vol-implied-forward-20261016` | reusable per-expiry implied forward / sdiv from put-call pairs; opt-in carry mode for the quote-slice path |
| slice-coverage-lane | `feat/vol-slice-coverage-20261016` | graded admission, cross-expiry term prior, channel-penalty objective, optional knot spline for the per-expiry fitter |

All results are development-cohort evidence; no default promotion, no holdout
labels opened, no main merge. Integration, gates and receipts are appended below
as lanes report.
