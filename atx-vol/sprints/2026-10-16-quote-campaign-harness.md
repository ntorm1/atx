# Fixed development campaign harness

`tools/quote_benchmark_campaign.py` runs 1..64 predetermined common-model
experiments against one immutable development cohort and a fixed baseline run.
It is reusable across prepared quote datasets and has no parquet-specific symbol
or contract logic. Candidate policies are copied and hashed before any scoring;
score results never choose a later candidate or alter a model input. Existing
native execution, separate scoring, independent price/vol masks, strata and exact
paired comparisons remain in `quote_benchmark.py`.

Example plan (paths resolve relative to the plan file):

```json
{
  "schema_version": 1,
  "dataset": "development-cohort",
  "binary": "bin/atx-vol-quote-bench.exe",
  "baseline_receipt": "baseline.csv.run.json",
  "dividends": "funding-pv",
  "tick_size": 0.01,
  "trials": [
    {"id": "control", "common_policy": "auto"},
    {"id": "candidate", "common_policy": "candidate-policy.json"}
  ]
}
```

Run with `python tools/quote_benchmark_campaign.py --plan plan.json --output
fresh-campaign-directory` from `atx-vol`. The output directory must be new.
Duplicate/unknown configuration keys, nonfinite JSON constants, duplicate or
unsafe trial IDs, oversized plans/policies, foreign baseline cohorts and mixed
or validation cohorts are refused. Both manifest and actual raw quote split are
checked before labels are read. Every native run receives only the existing
closed raw quote CSV; labels are read only after its successful publication.

The immutable start receipt records the plan, source hashes, executable, raw
input/manifest and fixed-baseline hashes, and all frozen policy snapshots. Those
inputs are checked before each trial, after native execution and at completion.
Each trial has a durable start/run receipt and native progress from the existing
wrapper, followed by score/comparison and an exclusive durable outcome file.
An append-only, fsynced campaign journal records start, trial starts/outcomes and
completion with monotonically increasing sequence numbers and artifact digests.

A failed native run or evaluation is retained as an explicit failed trial; later
fixed trials continue. Changes to shared frozen inputs abort the remaining queue.
Keyboard interruption preserves prior outcomes and an incomplete journal without
publishing a completion receipt. No partial run is resumed or overwritten. A new
campaign can explicitly name remaining trials. This is process-interruption
evidence, not a filesystem power-loss or authenticated-writer guarantee.

`complete.json` means every listed trial was attempted and its outcome recorded.
It distinguishes `complete` from `completed_with_failures`. Results retain each
candidate's own coverage and exact paired errors separately. There is no pooled
changing-population score, automatic policy promotion, validation evaluation or
qualified speed claim. The CLI returns 0 for complete success, 1 for completed
campaigns containing failures and 2 for a fatal campaign error.

Implementation preceded validation. Twenty focused checks pass, including real
native execution with a failed policy between two successful trials, identical
predictions under frozen policies, fixed baseline scoring, source/policy/start
mutation, invalid plans, raw-versus-manifest split disagreement and interruption.
Ruff passes. Receipt:
`C:/atx-cache/quote-benchmark/common-campaign-20261016-parent-tests-v4.xml`.
The native CTest registration rejects skipped cases when the benchmark binary is
available. Independent review is pending; no development campaign has been run
through this new driver yet.
