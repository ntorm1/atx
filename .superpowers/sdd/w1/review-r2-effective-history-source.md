# Independent R2 source review

Reviewer: pool5 / w0_gate_audit. Reviewed read-only integrated pool2 snapshot
`948498c2`, R2 imports `4de4cb81`, `8f7c13c9`, `5467efc2`, `d59a0d5a`,
`44d3a11f`, `2872cbdd`, `7585ff0c`, `727535b9`, format-only `cac29671`.
Read the lane report and root D6/R2 integration review before source inspection.
No source edits in pool2, compiler invocation, runtime rerun, numerical experiment or
market-data read was performed. This is independent of the root/implementation author.

## Finding R2-R1 (medium): validation workspace admission omits accumulator/output storage

`atx-engine/src/risk/model_validation.cpp`, `validate_risk_model_21d` initial admission
checks n times randomized/optimized book counts. It then allocates `minvars`, `opt`,
`custom(cfg.books.size())`, 10 decile accumulators, two single accumulators, and later
`eigen.resize(k)`. Each `Acc21` retains a 12-double ring plus counters and moments.
`custom` and all the accumulator payloads are not included in that preflight, nor the
simultaneously retained output metrics/name/cohort storage. Per-snapshot k-by-k/m-by-k
checks independently reuse the entire budget rather than subtracting retained storage.

Concrete shape admitted by the present guard: n=1, zero randomized/optimized books,
max_working_bytes=128, 10,000 valid one-element custom books. The first checks accept
1 <= 128/128, yet custom rings alone need 960,000 bytes, excluding every other field,
metric and workspace. A valid m=k=1 snapshot also passes its separate budget conditions.
This is an allocation contract defect, not a claim of observed host OOM.

Required bounded correction: overflow-safe aggregate admission before any owned group
allocation, subtract persistent randomized input matrices + all accumulator groups and
anticipated result storage from the workspace allowance, then admit factor-k accumulators
and simultaneous per-snapshot matrices/vectors against the remainder. Clearly document
whether caller-owned books/returns/factory-created model storage are outside the budget.
Add a tiny low-budget refusal case that proves the factory is not invoked when the known
persistent groups already exceed admission. Include eigen accumulator and output capacity
in the checked model-dependent portion. Root accepted this finding and owns the narrow
repair after the active build; no conflicting source mutation was requested.

## Other inspected contracts

- Covariance V2 carries supplied strictly increasing newest-first session ages through
  EWMA weights and exact lag joins. Zero-lag/lag estimates use the same column means and
  the declared older-endpoint weight, with zero-lag pair mass normalization. Missing
  observations are excluded; they do not become adjacent sessions or literal zero
  factor returns. Infinity and arithmetic overflow fail. Pairwise reconstruction is
  symmetrized and eigen-floored before downstream use. This review verifies the declared
  arithmetic, not empirical calibration of missing-data or NW choices.
- Effective-history eigen adjustment validates finite symmetric SPD input even when
  inactive. Active simulations use rounded Kish effective T and require T>K. Combined
  matrix/simulation workspace arithmetic avoids unchecked subtraction. Failed simulated
  covariance does not count as neutral adjustment evidence.
- Prior VRA requires available_age > realized_age (larger means earlier), explicit
  unknown-clock sentinels remain unavailable, and factor/specific records stay separate.
  The shared cleaning path requires prior specific cap weights and preserves unavailable
  adjustment status rather than using final-fit residuals as historical forecast records.
  Evidence identity is caller-bound provenance, as disclosed; it is not cryptographic
  validation of an external forecast archive.
- Fundamental V2 forwards original regression-date ages and missing factor coordinates.
  Hybrid V2 validates current caps/exposures, uses t+1 historical exposure rows, trains
  statistical factors only on complete residual histories, retains thin assets with zero
  APCA loading plus structural specific-risk fallback, and does not silently delete them.
  Current industry/cap identity assumptions remain explicit input contracts.
- Specific risk distinguishes time-series estimates, structural/population fallback and
  decile shrinkage. Its exposure fit fails into a reported population fallback instead
  of introducing an unreported ridge. Thin reliability and cap-decile formulas are model
  assumptions; the source report does not disguise them as vendor/empirical reproduction.
- The separate 21-session validator uses fixed forecast-date holdings, a sum of daily
  arithmetic returns, and horizon times daily long-run predicted variance. A nonzero
  holding with any missing realized daily return marks that book unavailable; it is not
  survivor-renormalized. Custom holdings absent from the snapshot also become unavailable.
  Frozen-exposure GLS residual diagnostics account for projection leverage and are not
  fed back to VRA. Eigenportfolio observations require an invertible mimicking system.
- MRAD now uses 12 contiguous forecast slots when step=21. Missing slots invalidate
  crossing windows without compressing time; reversing newest-first window order does
  not change each sample-standard-deviation statistic. Full-sample absolute bias deviation
  is separate, and pooled specific-decile residuals are excluded from rolling MRAD.
  Cohort aggregation weights actual eligible book/window pairs. Exact zero realizations
  retain the documented infinite QLIKE with an explicit count/null JSON representation.

## Verdict and remaining evidence

Source HOLD on R2-R1 workspace contract; no additional confirmed production correctness
blocker found in this bounded review. The numerical/causality paths above are suitable for
focused compilation qualification once the narrow budget repair is reviewed. Existing
legacy paths remain explicitly selectable; no new application default or all-artifact
serialization claim is inferred. Runtime/hygiene, K60/T252 calibration, regime-shift
acceptance and large-universe RSS remain separate gates, exactly as the owner report states.


## Fix-only closure: 57b4279d / 8c246ebf

Independently reviewed root production `57b4279d88e031f99410ee70a0546bc5b252e96e`
and postimplementation fixtures `8c246ebf229c368429c272050182f09202825e08` by frozen
Git objects. R2-R1 is CLOSED at source level; the earlier HOLD above is historical.
No source change, compiler, runtime rerun or market-data read was performed.

The initial admission now reserves fixed overhead, all known accumulator groups,
anticipated metric/string storage, random input matrices, label and custom names
before allocation or factory invocation. The reserve helper retains reserved <=
max_working_bytes; short-circuit refusal prevents subtraction underflow. Book-count
caps bound group and coefficient arithmetic. The per-snapshot admission subtracts
factor-k accumulator/output capacity, then K-squared workspace, then admits MK
workspace against the same remainder. Division guards precede products, K is
nonzero, and m > n is refused before per-model vectors. Metric capacity matches the
known group count plus K; fixed overhead and per-group slack cover bounded cohort
and name/container storage. Caller-owned input books/returns and factory-created
model storage are explicitly outside this owned scratch/result admission contract.

The two source fixtures discriminate the reported defect: 10,000 custom books at
128 KiB refuse before the factory; two books succeed; a long result name again
refuses before another factory call. The K=M=20 model fails the combined 64 KiB
budget after the necessary factory call and succeeds at 1 MiB. These are reviewed
assertions, not independently observed passing runtime results.

Verdict: APPROVE this narrow source repair for focused compilation qualification.
No additional confirmed blocker in the inspected correction. Runtime/hygiene and
all empirical/large-universe gates remain separate as listed above.
