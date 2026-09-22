# Learned feature availability and validation audit

Status: source-reviewed research for the next iteration; no learning code changed
or new learning tests run in this checkpoint. Accounting work is the current gate.

## Findings

1. `src/learn/latent.cpp::select_interactions` filters feature rows by
   `row_date <= t - embargo`, then excludes nonfinite `Y[0]`. A finite historical
   forward label can still depend on prices after `t`. For example, a date-9
   five-period return is finite in a complete dataset but unavailable at date 10.
   `FeatureMatrix` currently retains the labels but not their horizons or
   availability dates, so this selector cannot enforce label maturity. Embargo
   and label availability are separate requirements.
2. `src/learn/linear_alpha.cpp::fit_linear` and `src/learn/gbt.cpp::fit_gbt`
   assign `m.aug = aug`, then copy `m` into every `fold_shell`. They refit
   standardization on `f.train_rows`, but reuse the supplied fitted PCA basis and
   chosen interactions. A full-window augmentation can therefore contaminate
   fold evaluation even though scaling, coefficients and tree bins are local.
   Current pipeline and ensemble call sites pass an empty augmentation; those
   paths do not activate this particular contamination. Public nonempty-
   augmentation calls remain exposed.
3. The same fit APIs train on every usable row in their supplied matrix. They
   have no explicit deployment as-of boundary. Their full-window refit is valid
   only if callers have already limited features and matured labels correctly.
   This is a caller contract, not evidence that all historical refits are safe.
4. `oos_deflated_sharpe` consumes the frozen cross-sectional IC series. That is a
   model-skill statistic, not realized long/short portfolio returns after costs.
   It must not be presented as investment Sharpe or evidence of trading capacity.

## Bounded correction to design and review

- Preserve label horizon/availability metadata when `build_features` constructs
  a matrix. Require it where maturity matters; do not invent a one-period horizon
  for unannotated manually constructed matrices. Keep synthetic label fixtures
  explicit about their convention.
- Give supervised interaction selection an explicit label channel and enforce
  both feature cutoff and label availability. Use subtraction/bounds checks to
  avoid date-plus-horizon overflow. At a decision close, a return ending at that
  close can be used only under the documented same-close data convention.
- Represent augmentation fitting instructions separately from fitted artifacts.
  Fit requested PCA and supervised selections on each fold's admissible training
  rows; use that artifact unchanged on both sides. Refit the deployment artifact
  separately on the admissible deployment window. Explicit fixed interactions
  need no statistical refit, but their provenance must not be confused with
  interactions selected using all labels.
- Validate with held-out perturbation invariance at the **fold training artifact**
  level. The aggregate score may legitimately change when held-out labels or
  features change, so asserting score invariance would be the wrong test.
- Keep a sealed forward holdout and portfolio-level costed returns separate from
  CPCV diagnostics. Do not expand this correction into a new model family.

## Primary research

[scikit-learn common pitfalls](https://scikit-learn.org/stable/common_pitfalls.html),
accessed 2026-09-19, explains that fitting transformations or feature selection on
test data biases evaluation, including PCA and scaling. This supports fold-local
fitting; the specific label-maturity issue above follows from this repository's
forward-return definition and was identified by source inspection.

## Verification scope

Read `feature_matrix.hpp`, `feature_matrix.cpp`, `latent.hpp`, `latent.cpp`,
`train.hpp`, `linear_alpha.cpp`, `gbt.cpp`, and pipeline/ensemble call sites.
Codebase-memory MCP tools were unavailable, so discovery used targeted `rg`.
No claim of measured leakage magnitude, model improvement, or investment
performance is made. Candidate target: `atx-engine-learn-tests`; focused suites
should cover feature matrices, latent selection, linear models and GBT models.
