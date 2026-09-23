# FQ2 independent first-pass review

Scope: read-only review of the FQ2 evaluator, CLI, migration 0325, focused
tests, docs, and the bounded forward-label publisher diff against
`fundamental-signal-evaluation-brief.md`. FQ1's validator implementation was
excluded because this reviewer implemented FQ1. No runtime or database was
used in this review. The implementer's guarded focused rerun and actual
publisher fixture remain pending at review time.

## Critical

None found.

## Important

1. **The evaluation result digest omits fields that define the reported
   experiment.** `fundamental_signal_evaluation.py` lines 382-400 hashes
   decile `status`, counts, scores and returns, but omits `session_number`,
   `entry_date`, `expected_end_date`, `split`, `cohort_count`, and
   `tied_boundary_count`. The stored result SHA therefore remains unchanged
   if a completed row's entry, endpoint, split, or tie diagnostic is edited;
   those fields are visible in the warehouse and acceptance output. Seal all
   decile columns that define a result, in a stable order, and add one focused
   digest-tamper assertion. The run's configuration/calendar hashes do not
   cover these row-level fields.

2. **A malformed terminal label can pass the validity gate.**
   `_join_labels` at lines 190-204 checks the recognized basis/version,
   finite final return, endpoint, and terminal source name. For
   `is_delisted_in_horizon=true`, it does not require a finite
   `terminal_return >= -1`, a non-null delist date within `(entry,end]`, or
   terminal consistency; for a non-delisted row it does not reject a populated
   terminal return/delist date. The physical label schema permits those
   combinations, and an explicit label source can contain them with the
   recognized strings. Such a row contributes to Q10/Q1 and the observed or
   policy count. Validate the terminal leg fields before `label_status='valid'`
   and add a latest malformed-terminal revision fixture showing no fallback to
   the older row.

## Minor

- The docs/result say a later source replacement is "detected" by the label
  digest. The code stores hashes and counts but has no read-side recomparison
  of a completed evaluation to current labels. The evidence supports a future
  comparison; it does not automatically detect changed labels on a read of
  `status='complete'`. State this retention limit precisely or add a bounded
  read-only verifier.
- Focused tests directly exercise embargo and split crossing, but do not yet
  assert a calendar-gap HAC result, a constant/small cohort row, or a policy
  terminal attrition row. These are useful small fixtures once the guarded
  semantic run is repaired; they do not require a full warehouse bootstrap.

## Checked contracts

The evaluator calls FQ1's public validator before outcome reads, pins its
build digest, uses pre-label SQL ranking and deterministic tie allocation,
selects the latest label revision before basis/status filtering, joins on
entry rather than decision date, uses observed-session endpoints, retains
requested horizon rows, applies split purge and 63-session embargo, and uses
calendar-aware HAC with whole-family variable Holm. Migration 0325 leaves
legacy label basis/version NULL. The publisher writes the actual validated
selected price basis and calculation version in its stage/shadow publication,
and its focused test covers raw versus adjusted arithmetic and atomic
source replacement. The documented limits on vintage, liquidity, costs, and
production eligibility remain explicit.
