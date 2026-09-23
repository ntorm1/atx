# FQ1: daily point-in-time fundamental signal tables

User priority: build the professional quant platform even while host memory
blocks backfill. Read quant-research-workflow-gap.md for the capability audit,
but replace its file-only deliverable with actual warehouse research tables.
This task is the signal-panel builder; a separate fresh task will implement
general decile evaluation over these tables after the interface is fixed.
Do not claim alpha or production eligibility from builder completion.

## Ownership and interface

Own new src/atx_db/fundamental_signal_research.py,
scripts/research_fundamental_signals.py,tests/test_fundamental_signal_research.py,
docs/FUNDAMENTAL_SIGNAL_RESEARCH.md and migration bodies_0324.py with ONLY its
own registry/__init__ registration and required schema-contract fixtures.
0324 is reserved exclusively for FQ1, after DL1 commits0323 and releases the
registry. No activation/jobs/cli.py/CF1 edits. OPS1 is committed b814f4c1.
No runtime/live DB/import/tests/network until
root grants the sole test slot. Tiny fixtures, no full warehouse setup.

Public build API and CLI: build_fundamental_signal_panel(store, options),
`research_fundamental_signals.py build --db-path --start-date --end-date
--as-of-date --run-at --run-id [--signals-json] --memory-limit 256MB --threads 1`.
Require already migrated DB; no implicit migration or full-universe source load.
Require explicit bounded dates, UTC-aware cutoff/run time, unique run ID and
bounded specs (for example<=32signals,<=8terms each). Source IDs explicit/pinned.

Default predeclared family: positive eps_diluted_q_growth_yoy(q), positive
operating_margin_change_yoy(ttm), negative total_accruals(ttm), negative
net_debt_ebitda(ttm), plus an equal-weight complete-case combination of their
cross-sectional percentile ranks (directions applied before ranks).
Support declarative linear combinations of an audited dimensionless metric
allowlist, finite weights, and identity or cs_rank transforms. Do not accept
arbitrary caller SQL or unrestricted metric names. No raw currency/per-share
levels until units/FX lineage exists. Preserve negative finite values.
Freeze canonical specs and exact publisher definition contracts/hashes before
reading any outcome data. Inspect actual derived publisher hash construction;
never invent a weaker hash or accept a stale registry just because code matches.

## Persistent contract

Use new uniquely named tables such as fundamental_signal_runs,
fundamental_signal_definitions,fundamental_signal_values,
fundamental_signal_inputs and fundamental_signal_coverage. Keep CF1 unchanged.
Run manifest pins spec/hash,resolved metric definitions,query/code version,
source IDs,calendar digest,requested range/snapshot/cutoffs,diagnostic counts,
sealed panel digest,status,and blockers. Never use wall clock as input timing.
Values are long per(run,signal,date,security), nullable score and explicit
eligibility/reason/decision22UTC/nextsession entry/input clocks. Inputs retain
each selected derived_value_id,hashes,period/fiscal operand dates,origin,
history/value status and failure reason. Coverage includes every requested
decision session/signal even when no cohort or eligible score exists.

Runs are immutable experiments once complete; reuse of run ID errors.
Chunk commits are allowed only with manifest status building; partial runs
must never be consumable as completed panels. On failure retain diagnosable
failed run state; finalize manifest/count/digest only after all partitions.
Avoid giant global ART indexes on value/input tables. Include narrow explicit
PIT exemptions/catalog/schema contracts appropriate to immutable research
snapshots; do not broadly exempt unrelated live serving data.

## PIT and cohort semantics

- Build actual observed price-session calendar. T decision22UTC uses only
  information available by that cutoff and enters next observed session close.
  Keep input_end/clock and entry_date; no use of future labels in build.
- Historical membership must be actual visible dated US-common membership in
  universe_us_listed_membership; overlapping rows quarantine, not pick-one.
  Require one dated visible normalized CIK in security_identifier_history.
  Do not backdate current ticker directory or equate bar presence with common.
- Reconstruct complete visible derived event states at T without filtering on
  today's is_latest_revision. Rank by the correct period/event ordering before
  rejecting NULL/unavailable/legacy/definition mismatch. A newer invalid state
  cannot revive an older numeric revision or older fiscal period.
- Require period and operand fiscal end<=T,available_at<=T22UTC,reconstructed
  history,exact definition contracts and valid input hashes/lineage. Enforce
  configurable max age (default200days) on the selected root fiscal end AND
  newest actual selected leaf fiscal end/current anchor. Prior-year comparison
  operands must match the declared offsets/spans and visibility; they need not
  be within200days. Retain oldest/newest selected leaf fiscal ends for audit.
  Reuse/extend the reasoning in fundamental-desk-screen-acceptance.sql, including
  issuer qualification of actual selected operands rather than owner ID alone.
- Select the whole current state per accounting owner before issuer/proof
  screening. A prior <=decision verified owner/metric-to-CIK association can
  associate a newer unqualified state for conservative suppression/ambiguity
  only; it cannot qualify a score or backdate identity. This prevents a newer
  NULL/unverifiable owner state from silently exposing another older owner.
  Never infer association from owner text. Keep unmatched-state diagnostics.
- Missing or invalid term invalidates the whole combination; no zero-fill.
  Apply cross-sectional transforms to the complete eligible signal cohort
  before labels exist. Equal input values have equal percentile rank; constant
  cohorts must remain diagnosable. Keep cohort/coverage attrition counts.
- Empty/missing historical prerequisites produce explicit blocked/empty outcome
  with diagnostic rows, never a fabricated fallback sample or certified result.
  production_eligible is false. Pin unresolved modeled vintage/source coverage
  limits in manifests even when a fixture/research sample can be built.

## Bounded implementation and validation

SQL temporal selection and sequential bounded partitions; no all-universe
pandas frames or name/date/history Cartesian product materialized in Python.
Prefer run-scoped chunk publication with bounded insertion/recycling. Explain
worst partition shape and defaults in report. No unmeasured production run.
Focused fixtures: exact selected states at two historical decisions; late filing;
numeric then NULL invalidation; stale/future/legacy/wrong-hash/wrong-CIK legs;
overlapping common membership; negative values; missing leg invalidates mix;
rank ties; complete canonical spec/digest repeatability excluding run identity;
failed partial run not sealed; absent cohort yields per-date diagnostics;
minimal migration upgrade/replay and parser validation. Use small SQL fixtures.
After meaningful tests/root review, pathspec-only task commit with requested
literal trailer. Report exact API/table schema/tests in fundamental-signal-panel-result.md.
