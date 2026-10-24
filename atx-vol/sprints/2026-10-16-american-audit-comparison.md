# Saved independent-reference comparison

Lane: `feat/vol-audit-compare-20261016`, frozen base
`f76cefc2336be782495b51f09cee05ba4e1a00cd`, fresh pool-36 lease with RunId and
HeartbeatId `vol-audit-compare-20261016`. Graph tools remained unavailable; `rg`
fallback was used. The tool was implemented first, then checked, as requested.

The lane adds only `tools/american_audit_compare.py`, its Python tests and this
note. No production fitter, model, build registration or constraints changed.

## Interface

```powershell
python atx-vol/tools/american_audit_compare.py `
  --input C:/atx-cache/quote-benchmark/markov-audit-20261016-sinh-v2.json `
  --reference C:/atx-cache/quote-benchmark/american-reference-20261016-atm-refined-v1.json `
  --replacement-reference C:/atx-cache/quote-benchmark/american-reference-20261016-negative-rate-refined-v2.json `
  --baseline-grid default65 `
  --output C:/atx-cache/quote-benchmark/<new-immutable-report>.json
```

`compare_documents(source, reference, replacement_reference=None,
baseline_grid="default65", limits=Limits())` is the reusable in-memory entry.
It needs Python's standard library only. QuantLib is not imported or required.
The report is comparison schema 1. Inputs are native audit schema 2 and saved
independent-reference schema 1; unsupported schemas fail explicitly.

Exact reference joins use positive spot, the entire interval prefix (both clocks,
funding, supplied yield, volatility, cash), strike, side and exercise. Numeric
fields normalize to finite doubles, with negative zero normalized to zero and
integers that cannot be represented exactly refused. No tolerant rounding,
fixture-only join or model price determines reference truth. The saved reference
document's `cases` and its source/model-price comparisons are not used.

Numeric fixture identity is full spot/interval schedule. Sibling grids group by
that identity even when fixture names differ; the names survive as labels. Pair
comparisons qualify exact contract keys by this numeric fixture identity, so
the same prefix in distinct later schedules remains two distinct audit cases.
Duplicate numeric fixture/grid pairs, produced contracts and reference signatures
are refused rather than ambiguously selecting one.

Explicit `requested_strikes` plus top-level `requested_sides` and
`requested_exercises` define every expiry's intended contracts even for refused
cases. Produced contracts must be unique members of that set. All grids then
use the union of the same numeric fixture's requested/produced contracts.
Different requested sets get `not_requested` rows; missing entire cases get
`case_absent`; missing prices get `missing_price`. Grid/model/workspace/batch/
scalar refusals remain visible. A successful price status cannot have no rows.

For historical schema-2 artifacts without requested contracts, the denominator
is explicitly labeled as an inferred union of produced contracts, any requests,
and references with an exactly matching model prefix. This cannot establish
unknown originally intended strikes. When nothing can be inferred, the numeric
case, error and unknown-universe count remain visible; coverage is null, never
an invented zero-error result. Mixed explicit/legacy cases use the same fallback.

## Error populations and reference refinements

Whole-grid and fixture summaries distinguish price coverage, source failure
statuses, unmatched references, observed-stable errors and unresolved diagnostic
errors. Baseline pairing uses exact common observed-stable contracts only, with
baseline-only, candidate-only and neither counts alongside status transitions.
Errors are source price minus independent reference in price units. Paired MAE
changes therefore cannot silently credit selective refusals.

Observed stability remains an empirical solver refinement classification, not a
continuous-American proof. Each row preserves the reference uncertainty estimate
and threshold; solver failures and unsupported references remain unscored with
their reasons. Existing source tests and finite-grid consistency are not treated
as reference truth.

An optional replacement reference is explicitly authorized by its CLI argument.
Every replacement key must already exist in the original reference; additions
and duplicates fail. The report records original and replacement records,
including statuses/prices/uncertainty, and labels each row's chosen source.
Both reference files retain their separate file hashes and declared tool/input
hashes. A replacement need not have the same local reference ID; exact arguments
are its identity.

## Bounds, integrity and checks

Defaults bound each JSON file to 64 MiB, source cases to 4,096, source price rows
to 500,000, references to 100,000 and expanded comparison rows to 500,000.
In-memory Limits can lower these or raise to explicit hard caps. Interval count
is at most 64, requested strikes 4,096 per case, JSON nesting 32. Duplicate JSON
keys, nonfinite numbers, invalid dual clocks, negative prices/uncertainty,
unknown statuses and malformed contract schemas fail; there is no silent skip.

The script hash is captured at import. Input hashes cover exactly the bytes read,
including BOMs. The CLI rejects path, symlink-resolved and hardlink aliases, and
opens the output exclusively after successful validation and serialization.
Existing outputs are immutable. It flushes/fsyncs a successful report. Filesystem
failure during writing can leave a partial new file; the command returns failure
and will not overwrite it on retry. A new output path is required.

Postimplementation checks: **44 pytest cases passed, zero skips**, and Ruff passed:

```powershell
python -m pytest --noconftest atx-vol/python/tests/test_american_audit_compare.py -q
python -m ruff check atx-vol/tools/american_audit_compare.py atx-vol/python/tests/test_american_audit_compare.py
```

Cases cover exact dual-clock/coefficient/prefix joins, renamed fixtures, source
and reference duplicates, refusal/missing/absent/not-requested coverage,
zero-denominator legacy refusals, stable versus unresolved populations,
explicit replacement provenance/subset checks, signature integer precision,
nonfinite/negative malformed data, bounded expansion, BOMs/depth/size, immutable
output, hardlink aliases and import-time hashing. All use synthetic fixtures.

## Actual saved-panel result

Final-code report:
`C:/atx-cache/quote-benchmark/common-lane-sinh-v2-compare-20261016-v2.json`
SHA256 `d0b2bf6848222fa302f37b31e03c55339a46f0598ec39632cb5f5e5d865097df`.
Input SHA256 `b491940f5da502af80998dd8e86537e1a31e8a75a7e7a1ef15c0381bad61362e`.
Original-reference SHA256
`074d33d1b7d217efcc03ab043b85bc3f917fdde8b9e5ab7f818901c7dc7db5e2`.
Replacement-reference SHA256
`411f45df26c549b626a182e5717f9b0fb98fecdbcb08d3405c8355dbeb997edf`.

All 24 exact reference keys match. Two explicit replacement records are retained.
The report covers 38,880 rows over 15 grids, each with 2,592 intended contracts.
Only the saved ATM reference subset is independently price-scored:

| Grid | All requested prices | Stable reference prices | Stable-subset MAE | Max gap |
| --- | ---: | ---: | ---: | ---: |
| default65 | 2592/2592 | 24/24 | 0.163272839667 | 0.531240652704 |
| sinh65-variance | 2592/2592 | 24/24 | 0.002067433065 | 0.004816616844 |
| sinh129-variance | 2592/2592 | 24/24 | 0.000476122095 | 0.001079237017 |
| sinh129-tight | 576/2592 | 2/24 | 0.000253785358 | 0.000466475443 |

The tight grid refuses 9/11 models and loses 22 baseline reference contracts;
its own-population MAE is not comparable to the 24-contract MAEs. The paired
report exposes that selection explicitly. No new independent numerical solve
or vendor-label tuning was done. These are synthetic ATM diagnostics, not a
full-tail, continuous-American accuracy or HFT performance qualification.
