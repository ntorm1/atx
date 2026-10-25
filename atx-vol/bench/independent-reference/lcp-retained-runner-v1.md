# Retained-policy archive and fixed-four launcher

This completes the outer layer of prospective plan `25a32861`. The retention
primitive `db743583` and observer `a1cff1a7` have independent source approval.
No retained-policy saved-stage candidate or PDE request has run. This archive
and launcher require their own exact-source review before that experiment.

The public command accepts only input and new-output directories. It preserves
the predecessor's four input hashes, original 1e-10 tolerance, 128-update and
256-call observation caps, and underlying n+2 algorithm limit. It has no mode,
parameter, stage-subset or retry switch. The reader requires the reviewed Git
commit and compares every archived source to it, including the unchanged
original solver, PDE, strict observer and reused file/input helpers.

`lcp_retained_archive.py` exports separate `values`, `raw_policies` and
`used_policies` arrays under a new schema. It invokes only the new retained
reader. Exact Fraction/Decimal100 selected-equation audits explicitly receive
the actual preceding used masks. Substituting raw comparisons changes the
equations and is rejected. A pure one-row control independently distinguishes
these two masks: the used continuation equation has residual 2 while the raw
obstacle equation would have residual 1.

`lcp_retained_run.py` uses the frozen predecessor input loader and bounded file
helpers. It stores all four outcomes, including capped/numerically refused
prefixes. Original historical terminal equality is diagnostic only. The stage
summary preserves selector reservation, retained overrides, retained trajectory
storage and original input storage separately; generic solver temporary storage
remains unmeasured. The 24 MiB per-stage and 128 MiB total file caps are unchanged.

Each producer audit runs once and each independent reader audit runs once:
at most 131,064 distinct final/confirmed-repeat rows and 262,128 evaluations
across those two passes. The tiny four-stage test observes exactly eight audit
calls. The reader never invokes the observer or a linear solve. Complete is
published only after reader success and final input/source checks. An exception
leaves an incomplete archive, and an existing directory cannot be overwritten.

Seven initial refusal-stub tests failed at runtime. The final ten controls pass,
covering raw-mask substitution, work/exact-summary tampering, the four original
zero-solve phase mutants, actual-mask shape/caps, old-schema rejection, source
Git/pin/cleanliness joins, rehashed input/source changes, extra/missing/duplicate
denominators, control changes and incomplete publication on producer/reader
failure. All numerical fixtures are tiny synthetic inputs; the deliberately
scripted divergent trace checks evidence handling rather than solver accuracy.

```text
python -m unittest -v test_lcp_retained_run test_lcp_observe_run test_lcp_retained_observe test_lcp_policy_retention test_lcp_observe test_lcp_correction test_pde_reference.ObstacleTests
python -O -m unittest -v test_lcp_retained_run test_lcp_observe_run test_lcp_retained_observe test_lcp_policy_retention test_lcp_observe
```

The combined suite passes 84 tests; the optimized reader suite passes 69.
Evidence is at
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/lcp-retained-runner-v1/receipt.json`.
The prospective candidate can establish only the outcome of the four saved
systems. No convergence theorem, PDE/Greek accuracy, fitting or latency claim
follows from these controls.
