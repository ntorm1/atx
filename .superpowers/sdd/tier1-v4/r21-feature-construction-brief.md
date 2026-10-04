# R21 implementation scope: historical feature construction

Read task-21-cutoff-scope-ruling.md and the owner's explicit clarification first.
This work adjusts construction boundaries in existing Task6 and Task7. It does
not change their economic formulas, original gates, PIT rules or evaluation.
The first implementation dispatch has not occurred; root must assign one owner.

Preserve Task6's last approved c416c684 and Task7's last approved706fdf75 as
historical checkpoints. Each has used two review repair rounds. Do not reset
either cap or treat a new component name as a new allowance. Record this owner
requirement adjustment separately from correctness findings.

Task7 blanket construction restrictions identified statically:

- char_monthly_kernels.py compute rejects return-derived values after2022.
- char_monthly.py native_month skips return-derived cross-sections, and
  validate_unit forbids their later nonnull values.
- char_monthly_sources.py price_history blanks later daily/monthly historical
  returns and excludes later factor observations irrespective of feature date.
- char_monthly_registry.py and generated configuration/docs conflate evaluation
  with feature construction. Qualified-formula gate observation years and its
  explanatory note need consistency with original2020-2025 coverage scope.

Replace only those blanket construction boundaries with the already required
feature-date, source-availability and complete trailing-history conditions.
Late/future inputs still reject or remain missing. Preserve every source-quality
predicate and missingness reason other than the superseded calendar blanket.
Do not remove any char_monthly_evidence.py replication or forward-label boundary.

Task6 has source/history/risk filters in _factor_sources.py, _factor_native.py,
factors.py and risk_inputs.py. Audit each occurrence by purpose. Benchmark
evaluation months2013-2022 and comparator boundaries stay. Historical risk
predictors may use source-qualified past returns/factor inputs after2022;
separate their construction from portfolio performance assessment. Do not
silently use a future same-period factor return or unqualified later snapshot.

Own only Task6/7 producer/private modules, directly covering tests and existing
per-stage docs/config/registries/reports. Task8's new char_replication* modules,
Task12/13, frozen Task16 code and every foreign dirty path are off limits.
Coordinate semantic migration with Task8 before its final contract freeze.
New semantic digests must invalidate incompatible old snapshots/receipts.

Focused fixtures must show a later predictor uses only eligible trailing data,
rejects future or equality-cutoff inputs, and preserves complete-history/source
quality requirements. Separately show post2022 replication/evaluation and any
forward window crossing2023 still refuse. Use existing meaningful fixtures,
no broad unchanged reruns; pytest-n0/no default xdist, under60s/file and
DuckDB128MB/one thread. No actual source/data/hash/HTTP/evaluation jobs without
root's serialized explicit grant. Root is validating fundamentals concurrently.

Refreeze and commit exact construction config/code before actual measurements.
Append Task6/7 reports with the owner's scope change, precise tests and actual
gates still unmeasured/failed. Explicit owned-path commits only, legacy trailer.
Independent review is required before actual construction; this brief grants
no producer run and no post2022 evaluation.
