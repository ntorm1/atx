# First common-model development baseline

The unchanged automatic common-Markov policy completed the fixed development
cohort in 839.18 wall seconds (837.64 seconds native processing). This is an
unpinned diagnostic run with concurrent development, not a latency qualification.
The native executable was frozen at source `f1505f05b8a95302126cc9e84be23118a04cefd2`,
SHA256 `8da12138b5a7718ac98fb488654fc99c4535e8a886a0eb55ebf1397a68184825`.
The input is the existing immutable `20261014-dev200` dataset; no validation
labels were opened. Execution consumed only the closed raw quote input. Separate
development scoring used `srPrc` and `srVol` after predictions were published.

## Coverage and matched errors

| Measure | Earlier automatic quote-slice | Common Markov |
| --- | ---: | ---: |
| Eligible input rows | 63,942 | 63,942 |
| Price predictions | 10,694 (16.7245%) | 25,014 (39.1198%) |
| Vol predictions | 10,694 (16.7245%) | 18,978 (29.6800%) |
| Matched price MAE, 1,252 contracts | 0.174061385 | 0.461556331 |
| Matched vol MAE, 1,152 contracts, bp | 338.506580 | 932.306838 |

Coverage improves; accuracy on the shared contracts worsens. The common model
improves price absolute error on only 18.13% of matched contracts, worsens 78.51%,
and ties 3.35%. Its own 24,581 price-label matches have MAE 0.500204628; its own
18,978 vol matches have MAE 2,794.114163 bp. Those separate-population figures
must not be compared directly to earlier own-population MAEs. Price quote
containment is 90.8291%; midpoint-to-reference MAE on the same price-scored rows
is 0.333876124. Reference quote containment on its eligible label denominator is
98.7764%, a separate diagnostic rather than an attainable model certificate.

There are 191 standard-deliverable snapshots. Of these, 162 fit: all 93 resolved
sparse snapshots and 69 of 91 ordinary snapshots. Twenty-two ordinary snapshots
refuse the unchanged rollback-work limit; two fail parameter capacity, two raw
market quote capacity, two carry dispersion and one a market numeric limit.
Full prediction output retains 20,056 fit failures, 12,968 market-capacity
refusals, 4,766 policy refusals, 372 carry refusals, 50 numeric refusals, 716
unsupported deliverables and 8,406 input rejections. Six thousand thirty-six
served prices have unidentifiable equivalent IV, explicitly blank rather than
filled from local sigma.

## What the raw fit diagnostics establish

Sparse fits took 32.16 summed snapshot seconds; ordinary snapshots took 805.47.
Sparse successful fits use one shared sigma, median 33 evaluations; ordinary
fits use three shared state knots, median 163 evaluations. Across successful
fits, 147 terminate on step tolerance and 15 on the sweep limit. Step tolerance
does not establish a global optimum or adequate market fit. The default
midpoint weight is zero everywhere. Wide quote intervals can therefore admit
many marks, with regularization choosing among them. This is a concrete reason
to isolate quote-midpoint anchoring as a separate development experiment; it
does not prove that it explains all observed errors.

The next two fixed experiments are declared before their outcomes:

1. Rerun the same automatic policy with shared immutable storage and optional
   durable progress. Require identical prediction bytes against this baseline;
   compare expanded configuration and numerical fit diagnostics, excluding
   explicitly added storage/timing/progress metadata.
2. Keep every default profile, bound, prior, grid and classifier unchanged, but
   set `midpoint_weight=0.1` on each profile. This applies only to positive-bid
   quotes, preserving the bidless upper-bound observation. Evaluate full coverage
   and exact matched price/vol cohorts. No default promotion follows from this
   single development panel.

Quote-only starting estimates and a separately audited variance-resolvent
transition remain distinct experimental changes. They must not be conflated
with this objective comparison. No resource cap or admission condition is
relaxed to improve coverage.

## Immutable evidence

Artifacts are under `C:/atx-cache/quote-benchmark/20261014-dev200/`:

| Artifact | SHA256 |
| --- | --- |
| `20261016-common-auto-funding-v1.csv.run.json` | `633de13d2aa6ee211dc2946e18bfedb84369cfca1e921809fe0710d41b07b387` |
| `20261016-common-auto-funding-v1.csv` | `3f6965872f72e137956f8a8534adbd0e2755f6804023cec333860e7faf835cd3` |
| `20261016-common-auto-funding-v1-native.json` | `44fd9cf4a2517ba32e25e7534907d025ac3ea61a5a337148ab97532f406f21bb` |
| `20261016-common-auto-funding-v1-score.json` | `a283195fe53e87d0df73e4feaa53f9b283bfef1f19605d25ce93412f4a46158c` |
| `20261016-auto-vs-common-funding-v1-compare.json` | `645460c7331a86e6be77849ea277acf96c986366dbdc0f249bc246ad6a613d25` |

The original executable and adjacent runtime DLLs are retained separately in
`C:/atx-cache/quote-benchmark/common-auto-baseline-20261016-bin/`. Original run
receipts and predictions are not overwritten.
