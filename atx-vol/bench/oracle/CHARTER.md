# Oracle RSI bootstrap charter

Spec: `docs/superpowers/specs/2026-08-15-oracle-rsi-loop-design.md`.
`vol-oracle-iter` begins with a read-only capability inspection and selects the
first missing state in this exact order:

```
missing_data -> missing_mode_a -> missing_conventions -> missing_mode_b -> ready
```

A bootstrap invocation dispatches exactly one fixed implementation lane for that
state. That lane must complete Build, exact-SHA Review, optional Fix, fresh
exact-SHA Review, scoped verification in a newly leased worktree, and atomic
landing on `refs/heads/oracle/canonical`; only then does the run return BOOTSTRAP.
It does not call the planner, ordinary Measure, vol-sprint, analyst, or Ratchet,
and it never benchmarks holdout. The next invocation must observe the next state.
Only `ready` enters the RSI loop.

Capability runs the fixed `scripts/oracle-capability.ps1` probe through a
tool-restricted, no-read agent. It freezes `refs/heads/oracle/canonical` (or `main`
before the first landing), validates closed committed receipts, and internally
parses the three committed cohort manifests. It recomputes the canonical holdout
membership digest and disjointness but returns no membership. Every lane starts from that SHA, uses a run-unique branch and
durable heartbeat lease with an independent continuously renewing keeper, commits
explicit paths, and releases with the same `run_id`. Typed receipts prove
acquisition, keeper identity plus authenticated ready pulse, exact reviewed-SHA integration/HEAD, every scoped
gate, and release. Success evidence contains only exit-code-zero command output. Licensed
row values, option membership, and holdout membership must never enter prompts or
logs.

Before any Stage 1 ingest, `scripts/oracle-adopt-existing-data.ps1` performs the
mandatory one-time adoption check. It reads the three cohort manifests from the
frozen commit, recomputes their schema, membership digest, and both disjointness
rules internally, pins the complete required SpiderRock field/type schema, checks
the aggregate ingest manifest against Parquet footer row counts/date/bucket stats,
then projects only `undSecKey_tk` and uses Arrow compute against the closed cohort
target set. Only boolean matched-target state is retained; no keys, membership,
or row values are materialized or emitted. `ADOPTED` transactionally publishes the
v1 digest and data receipt and bypasses both extraction and the transient-disk
gate. `INGEST_REQUIRED` is fail-closed and is the only route to the licensed ZIP
and normal ingest path; there is no flag that bypasses or selects adoption.

### Closed capability receipts

Capability fails closed on missing, legacy, extra-key, corrupt, or unprovenanced
receipts. Data and Mode receipts are schema version 1; the convention receipt is
schema version 2. Every receipt has full `base_sha` and `tested_sha`;
the probe requires `base_sha` to be an ancestor of `tested_sha`, `tested_sha` to be
an ancestor of canonical, and the prior-stage receipt blob at `base_sha` to equal
the inherited blob at canonical.

- `bootstrap/data.json` has exactly: `schema_version`, `transition=data`, the two
  SHAs, `command_id=oracle_existing_store_adoption` for footer-plus-projection adoption or
  `oracle_ingest_and_cohort_validate` for the normal licensed ingest, `exit_code=0`, ingest
  manifest name/SHA-256, smoke/tune/holdout Git blob IDs, the holdout membership
  SHA-256, three schema-valid booleans, and the two tune/holdout disjointness
  booleans. The probe validates the external aggregate ingest manifest schema,
  digest, positive counts and bucket sum; validates all three cohort schemas and
  blob IDs; recomputes SHA-256 over the canonical ordered object
  `{schema_version:1,name,dates,underliers,buckets_et}` with each membership array
  sorted; and independently verifies both tune/holdout disjointness invariants.
- `bootstrap/mode-a.json` has exactly the common provenance fields,
  `transition=mode_a`, `command_id=oracle_mode_a_aggregate`, `exit_code=0`, the
  smoke blob ID, positive row count, and the complete closed Mode A target-ID set.
- `bootstrap/conventions.json` has exactly the common provenance fields,
  `transition=conventions`, `command_id=oracle_conventions_smoke_tune`,
  `exit_code=0`, smoke/tune blob IDs, `CONVENTIONS.md` and iter-000 blob IDs, and
  positive aggregate row count, the complete eleven-target registry, baseline,
  winning AND `production_conventions` complete convention maps, BOTH numeric
  candidate/baseline/delta floor arrays — the standard-relative
  `metrics`/`baseline_metrics`/`metric_deltas` and the symmetric-relative
  `symmetric_metrics`/`baseline_symmetric_metrics`/`symmetric_metric_deltas`,
  the latter being the ratchet baseline and the no-regression criterion (Stage 3
  below states why) —
  the eight-candidate price attribution, and the pinned quiet rel-avx2 speed.
  `production_conventions` is the map `winning_convention()` actually prices
  with; both committed artifacts carry it so the floor holds a standalone record
  of that map rather than only of the map the sweep resolved, and the probe
  requires the two to be equal. Iter-000 is itself an exact schema-v2
  `residual_floor` receipt with the same
  values plus Mode A, smoke+tune, both cohort blob IDs, empty oracle-suspect
  candidates until an NBBO gate supplies market evidence, explicit market-evidence
  status, and a non-citable dev diagnostic timing. The receipt and scorecard values
  must be EQUAL FIELD BY FIELD, numbers compared as doubles — never as
  re-serialized text, which in Windows PowerShell 5.1 compares source digits and
  makes an authored `0.0` differ from the same number written `0`.
- `bootstrap/mode-b.json` has exactly the common provenance fields,
  `transition=mode_b`, `command_id=oracle_mode_b_aggregate`, `exit_code=0`,
  smoke/tune blob IDs, positive row count, and the complete closed Mode B target-ID
  set.

The workflow target registry is price MAE, vol MAE, and all nine SpiderRock
greek columns as relative error for both modes: delta, gamma, theta, vega, rho,
phi, volga, vanna, and delta decay. Receipt strings are closed IDs/enums; no row
text or encoded payload field exists.

## Stage 1 - data (`missing_data`)

First run exactly `powershell scripts\oracle-adopt-existing-data.ps1`. If it
returns `ADOPTED`, commit only the generated `holdout.sha256` and `data.json` and
do not require transient disk or rerun ingest. It validates committed cohorts and
the existing aggregate store using Parquet footers plus an aggregate-only projection
of exactly `undSecKey_tk`; Arrow comparisons retain only boolean target matches and
never materialize option rows or stored key strings. Both outputs are staged and validated before a
journaled two-target publication; any exception restores prior bytes and returns
typed `INGEST_REQUIRED`.

Only `INGEST_REQUIRED` enters the normal ingest route. That route requires at
least 15 GiB free transient space on the work drive. If the precondition or
licensed ZIP is missing, report `BLOCKED`; do not partially ingest.

Run `python atx-vol/scripts/oracle_ingest.py --zip <licensed zip>` and create or
repair:

- the partitioned parquet store and checksum/row-count manifest;
- `cohorts/smoke.json`, `cohorts/tune.json`, and `cohorts/holdout.json`;
- `cohorts/holdout.sha256`, the SHA-256 of the compact canonical ordered object
  (`schema_version=1`, `name`, sorted `dates`, sorted `underliers`, sorted
  `buckets_et`) derived from `holdout.json`.

Smoke/tune/holdout must validate against `cohorts/README.md`; tune and holdout are
disjoint in both underliers and buckets. Stage 1 may validate holdout metadata and
hash, but must not run `atx-vol-oracle-bench` on it. Done means the ingest manifest,
three cohorts, frozen hash, and exact `bootstrap/data.json` receipt above validate
with pasted aggregate evidence.

## Stage 2 - Mode A (`missing_mode_a`)

Run the exact targeted Mode A gates before editing and return typed precheck
receipts. If both pass, an exact base-to-tested changed-path receipt must contain
only `bootstrap/mode-a.json`; any other path fails the workflow. If either gate
fails, the typed path switches to normal implementation and must include both an
implementation path and the v1 receipt. Otherwise implement `atx-vol-oracle-bench` Mode A.
It reads cohort-selected parquet
with predicate pushdown, prices SpiderRock inputs with `srVol`, compares aggregate
price/greek cells, and isolates all comparison semantics in
`oracle_conventions.*`. Mode B must not be implemented or stubbed in this stage.

Targeted unit tests cover band edges, tolerance accounting, sentinel/null handling,
and aggregate-only reporting. Run smoke only and write
`atx-vol/bench/oracle/bootstrap/mode-a.json`, an aggregate capability receipt with
the exact closed Mode A schema above (never rows themselves). Holdout is forbidden.

## Stage 3 - conventions (`missing_conventions`)

Using Mode A on smoke+tune only, run the closed deterministic staged sweep. Price
input attribution first evaluates all eight bounded candidates on smoke, including
the documented SpiderRock discrete-dividend forward
`fUPrc = uPrc * exp(rate * T) - ddiv`, then ranks only the stable-ID-tiebroken
top two on a deterministic tune sample ALONE — smoke decides the eight-way cut
and its evidence is never counted a second time. The winning input treatment is evaluated
on complete smoke+tune while bounded independent searches resolve theta/vega/rho/
phi/volga/vanna/delta-decay scaling and sources, theta day count, signs, and share scaling.
The scorecard's calendar DTE banding day count is a separate convention and stays
pinned; the theta unit pick must not re-bucket it. The map publishes both: the
`day_count` field is theta's, derived from the multiplier production applies
rather than from a descriptive field, and `dte_banding_day_count` records the
banding convention so a silent change to it is visible in the receipt. Each row
is priced under BOTH
the winner and the baseline map before anything is accumulated, so the candidate
and baseline floors always describe one row population — the gates enforce equal
per-metric counts. Over that one population the sweep publishes TWO eleven-metric
floor arrays, deliberately different OBJECTIVES rather than duplicates.
`metrics`/`baseline_metrics`/`metric_deltas` are STANDARD-RELATIVE,
`|m - o| / max(|o|, kSelectionAbsFloor)`, kept because this charter states the
Greek target relative to the ORACLE and the committed floor must stay directly
comparable to that "greeks within 1% rel" target.
`symmetric_metrics`/`baseline_symmetric_metrics`/`symmetric_metric_deltas` are
SYMMETRIC-RELATIVE, `|m - o| / max(|m|, |o|, kSelectionAbsFloor)`, which is the
loss the scale SELECTION minimises. `kSelectionAbsFloor` is the sweep's OWN
constant, not the scorecard's reporting tolerance, so retuning that tolerance
cannot silently move what selection optimised.

The hard no-regression gate AND the committed ratchet baseline are the SYMMETRIC
array. The symmetric loss is bounded and carries no smallest-scale gradient, so
selection runs on the FULL row population and excludes nothing —
`selection_count` now equals `count`. The standard-relative form instead pins its
denominator on rows whose oracle sits below the floor while its numerator keeps
growing with `|model * scale|`, so gating IT would systematically reward the
smaller multiplier and contradict the selector by construction rather than catch
a real defect. The standard array is still validated in every layer for shape,
finiteness, non-negativity, count parity and the
`candidate - baseline == delta` arithmetic; it is simply not the criterion, and
the two arrays must never be unified. Both counts stay published and every
validator still requires `selection_count >= count / 10`, so an objective that
ever narrowed the selection population again would have to move a published
number instead of doing it silently; the weakest ratio is surfaced in the gate
receipt's `audit_summary` as `min_selection_pct`. The sweep also emits
`production_conventions`, the
map `winning_convention()` actually prices with, and the gate fails closed while
it differs from the resolved winner. Row accounting must close as
`smoke_rows + tune_rows == rows_priced + engine_errors`, and a metric or input
candidate that admitted no observation at all is a hard error naming that metric
— never an `inf` written into the receipt.
Vol remains decimal identity. No Cartesian-product sweep is permitted. Commit the winning map to
`atx-vol/bench/oracle/CONVENTIONS.md`, encode it only in the isolated convention
layer, and write aggregate `scorecards/iter-000.json` with the Mode A residual
floor plus the exact `bootstrap/conventions.json` receipt. Run only these five
worktree-local gates, IN THIS ORDER: `convention_tests`, `mode_a_smoke_tune`,
`convention_speed_measure`, `residual_floor`, and `convention_speed`. The order
is executable rather than nominal: `residual_floor` hard-requires the committed
iter-000, and `convention_speed_measure` produces the number iter-000 needs, so
the measure gate necessarily precedes it. The residual gate verifies the exact-SHA sweep artifact
from the preceding smoke+tune gate against iter-000; it does not price the same
rows twice and fails if the artifact is absent or belongs to another SHA. On a
first-ever Stage 3 run no speed pin exists, so `convention_speed_measure` runs
BEFORE iter-000 is committed: it builds only the rel-avx2 oracle-bench target,
runs tune on a quiet host, and emits the measured rows_per_second with no pin
comparison. That receipt is the only sanctioned source of iter-000's speed
floor, and the floor is DERIVED from it rather than copied: `speed.baseline` is
the measured rows_per_second and `speed.pin` is `floor(baseline * 0.90)`. A
verbatim copy would leave `pin == baseline`, making the re-measurement a coin
flip on run-to-run noise, so the validators reject any pin above
`baseline * 0.95`. `convention_speed` then re-runs the same quiet measurement
against the committed pin. Record that evidenced floor in NORTHSTAR and append
LEDGER only after audited landing; the Stage 3 build lane must not edit either
memory file. Do not read Mode B or benchmark holdout.

## Stage 4 - Mode B (`missing_mode_b`)

After conventions are frozen, implement Mode B fitting from NBBO and aggregate
comparison of fitted vol, price, and greeks. Run targeted tests plus smoke+tune only.
Write `atx-vol/bench/oracle/bootstrap/mode-b.json` with the full git SHA, command,
exit code, complete Mode B target set, smoke/tune blob IDs, and positive aggregate
count exactly as specified above. Do not
change conventions or holdout membership and do not benchmark holdout.

## Ready-state failure rule

In `ready`, Measure returns exact typed JSON receipts for the fixed Mode A, Mode B,
and quiet-speed commands. Workflow code validates their complete numeric metric
sets and derives the self-contained aggregate payload; Measure cannot self-report
or override a baseline or speed pin. The tool-less Analyst sees only schema v2:
the complete enumerated Mode A/B target registry, two aggregate baselines, a
positive frozen speed pin, validated IDs, and
a closed convention enum map. It has no workspace, arbitrary-string field, paths,
hashes, membership, or row values. If any
mandatory vol-sprint lane is blocked/incomplete, lacks a fresh APPROVE, or fails the
isolated integration gate, the oracle run returns `FAILED`: no holdout benchmark,
no Ratchet, and no REJECT-counter change. Ratchet alone benchmarks holdout or opens
licensed holdout rows: it leases a
new worktree at the exact reviewed sprint integration SHA, recomputes membership,
and compares it with the receipt frozen at run start. Ratchet prepares typed
baseline/candidate/delta, digest, required-gate, pinned-speed, and per-suspect typed
NBBO receipts. Workflow code owns metric IDs/classes/directions/limits and exact
gate commands; Measure's exact typed speed result supplies the baseline/pin.
Ratchet candidate values are derived from typed gate results, not agent metric
prose. The workflow also owns bounded numeric suspect identifiers; it recomputes NBBO distances and verifies
the market is strictly closer to atx-vol before exclusion. It then computes ACCEPT/REJECT; the agent
verdict is never authoritative. For ACCEPT only, a minimal finalizer compare-and-
swaps `refs/heads/oracle/canonical` to the prepared commit, then an independent
auditor reads the actual ref. REJECT leaves it unchanged. If the finalizer report
is missing, audit truth is returned rather than assuming the old SHA.

The ready-state sprint never runs a full regression label, broad ctest/build, full
repository hygiene, or release gate. Its integration registry is derived exactly
from changed files and contains only affected anchored unit tests, hypothesis
OracleBench tests, aggregate smoke/tune Mode A/B scorecards, the quiet pinned-speed
microbenchmark, and owning PCH-off targets for changed headers. Full regression and
release qualification are separate from the oracle loop.

Sprint unit-test mappings name real fully-qualified tests emitted by
`gtest_discover_tests` and execute through the same fixed targeted adapter. Both
lane and integration receipts must prove positive executed/passed counts; an exit-0
`No tests were found` result is a contract failure.

Bootstrap stages 2-4 invoke only `scripts/oracle-targeted-gate.ps1 -Gate <closed-id>`.
That fixed adapter captures ordinary targeted ctest/OracleBench output, fails on a
nonzero exit, zero executed tests, zero processed rows, or an incomplete metric set,
and emits the exact semantic typed PASS receipt plus aggregate audit summary/raw
digest consumed by the workflow. Ctest uses `--no-tests=error`. Direct test
executables and broad build-wrapper verbs are invalid evidence.
