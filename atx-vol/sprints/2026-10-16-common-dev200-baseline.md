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

## Completed isolated experiments

The storage/progress rerun completed in 535.60 diagnostic wall seconds. Every
prediction byte matches the original 72,348-row CSV, SHA256
`3f6965872f72e137956f8a8534adbd0e2755f6804023cec333860e7faf835cd3`.
Expanded configuration and all 191 snapshot decisions also match after excluding
the newly introduced stage/evaluation timing metadata. Stage sums are 519.02
seconds fitting, 15.26 equivalent IV, 0.016 market, 0.008 policy and 0.001 grid.
There are 13,988 known objective evaluations and 22 failed-fit counts explicitly
unavailable. Different concurrent host loads prevent a qualified speedup claim.

The midpoint-0.1 experiment completed in 552.69 diagnostic seconds. Expanded
configuration differs only in that one weight on all three profiles. Price
coverage remains 25,014/63,942 with exactly the same 24,581 price-scored rows.
Matched price MAE falls from 0.500204628 to 0.459036561 (8.23%). Containment rises
from 90.8291% to 91.5887%. Price p99 absolute error rises from 4.02242 to 4.06426,
so the gain is not uniform. Price error improves on 46.47%, worsens on 17.73%, and
ties on 35.80% of the matched rows.

Volatility coverage changes from 18,978 to 19,018 rows. On the exact 18,867-row
vol intersection, MAE falls from 2,797.746780 to 2,698.377824 bp; 111 baseline-only
and 151 candidate-only scored vols remain visible. Ordinary-profile matched price
MAE falls from 0.527242 to 0.476551 on 14,419 contracts; sparse falls from 0.461840
to 0.434186 on 10,162. These are development results, not holdout qualification.

Against the older quote-slice model, the midpoint candidate still has worse
matched price MAE: 0.403867558 versus 0.174061385 on the same 1,252 contracts.
The next numerical and term-structure work remains necessary. The production
defaults are unchanged; no automated promotion is authorized by these scores.

Both experiments use archived native SHA256
`d5a3d5e6f9f6aac4cb2dae58f225c2071677aba64cc85ddac7fb4e19f00593d0`
in `C:/atx-cache/quote-benchmark/common-storage-progress-20261016-bin/`, with
adjacent DLLs and build provenance. Their complete durable journals and immutable
start receipts accompany the following artifacts in the original dataset folder:

| Artifact | SHA256 |
| --- | --- |
| `20261016-common-storage-parity-v2.csv.run.json` | `df85b627e2b4cf991249698840a8f90a0ccdd23183971b8a9310560cfe5a1a77` |
| `20261016-common-storage-parity-v2-native.json` | `1575729e86f5c9dc2a04c6e19673f01471511d518328628f30791970c1963524` |
| `20261016-common-midpoint010-v1.csv.run.json` | `3abf2c3b52b6febc2d77a337981414687343955bcf50cdde0636178d868cbe18` |
| `20261016-common-midpoint010-v1-score.json` | `a87439afdf4acb989c89fae64d36e74ae820c06f2db221c36468686ebadc76a4` |
| `20261016-common-vs-midpoint010-v1-compare.json` | `7b54d2b393b9a6a7be2c6dd60f5ea93a87ad08cb5318be236f46cfa7405ad55e` |
