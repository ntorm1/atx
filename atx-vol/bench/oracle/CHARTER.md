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
before the first landing), tests only committed receipt existence, and reads only
the already committed digest receipt; it cannot open holdout membership. Every lane starts from that SHA, uses a run-unique branch and
durable heartbeat lease with an independent continuously renewing keeper, commits
explicit paths, and releases with the same `run_id`. Typed receipts prove
acquisition, keeper identity plus authenticated ready pulse, exact reviewed-SHA integration/HEAD, every scoped
gate, and release. Success evidence contains only exit-code-zero command output. Licensed
row values, option membership, and holdout membership must never enter prompts or
logs.

### Closed capability receipts

Capability fails closed on missing, legacy, extra-key, corrupt, or unprovenanced
receipts. All receipts are schema version 1 with full `base_sha` and `tested_sha`;
the probe requires `base_sha` to be an ancestor of `tested_sha`, `tested_sha` to be
an ancestor of canonical, and the prior-stage receipt blob at `base_sha` to equal
the inherited blob at canonical.

- `bootstrap/data.json` has exactly: `schema_version`, `transition=data`, the two
  SHAs, `command_id=oracle_ingest_and_cohort_validate`, `exit_code=0`, ingest
  manifest name/SHA-256, smoke/tune/holdout Git blob IDs, the holdout membership
  SHA-256, three schema-valid booleans, and the two tune/holdout disjointness
  booleans. The probe validates the external aggregate ingest manifest schema,
  digest, positive counts and bucket sum; validates smoke/tune schemas and blob
  IDs; and checks the holdout blob ID plus frozen membership digest without opening
  `holdout.json`.
- `bootstrap/mode-a.json` has exactly the common provenance fields,
  `transition=mode_a`, `command_id=oracle_mode_a_aggregate`, `exit_code=0`, the
  smoke blob ID, positive row count, and the complete closed Mode A target-ID set.
- `bootstrap/conventions.json` has exactly the common provenance fields,
  `transition=conventions`, `command_id=oracle_conventions_smoke_tune`,
  `exit_code=0`, smoke/tune blob IDs, `CONVENTIONS.md` and iter-000 blob IDs, and
  the closed theta/vega/rate/dividend/day-count/sign enum map. Iter-000 is itself an
  exact versioned `residual_floor` receipt with Mode A, smoke+tune, positive count,
  complete target IDs, and ancestor provenance.
- `bootstrap/mode-b.json` has exactly the common provenance fields,
  `transition=mode_b`, `command_id=oracle_mode_b_aggregate`, `exit_code=0`,
  smoke/tune blob IDs, positive row count, and the complete closed Mode B target-ID
  set.

The workflow target registry is price MAE, vol MAE, and delta/gamma/theta/vega
relative error for both modes. Receipt strings are closed IDs/enums; no row text or
encoded payload field exists.

## Stage 1 - data (`missing_data`)

Precondition: at least 15 GiB free transient space on the work drive. If the
precondition or licensed ZIP is missing, report `BLOCKED`; do not partially ingest.

Run `python atx-vol/scripts/oracle_ingest.py --zip <licensed zip>` and create or
repair:

- the partitioned parquet store and checksum/row-count manifest;
- `cohorts/smoke.json`, `cohorts/tune.json`, and `cohorts/holdout.json`;
- `cohorts/holdout.sha256`, the SHA-256 of canonical sorted membership fields
  (`dates`, `underliers`, `buckets_et`) from `holdout.json`.

Smoke/tune/holdout must validate against `cohorts/README.md`; tune and holdout are
disjoint in both underliers and buckets. Stage 1 may validate holdout metadata and
hash, but must not run `atx-vol-oracle-bench` on it. Done means the ingest manifest,
three cohorts, frozen hash, and exact `bootstrap/data.json` receipt above validate
with pasted aggregate evidence.

## Stage 2 - Mode A (`missing_mode_a`)

Implement `atx-vol-oracle-bench` Mode A first. It reads cohort-selected parquet
with predicate pushdown, prices SpiderRock inputs with `srVol`, compares aggregate
price/greek cells, and isolates all comparison semantics in
`oracle_conventions.*`. Mode B must not be implemented or stubbed in this stage.

Targeted unit tests cover band edges, tolerance accounting, sentinel/null handling,
and aggregate-only reporting. Run smoke only and write
`atx-vol/bench/oracle/bootstrap/mode-a.json`, an aggregate capability receipt with
the exact closed Mode A schema above (never rows themselves). Holdout is forbidden.

## Stage 3 - conventions (`missing_conventions`)

Using Mode A on smoke+tune only, resolve theta/vega scaling, rate/borrow/dividend
treatment, day count, `vo`/`va`, signs, and share scaling. Commit the winning map to
`atx-vol/bench/oracle/CONVENTIONS.md`, encode it only in the isolated convention
layer, and write aggregate `scorecards/iter-000.json` with the Mode A residual
floor plus the exact `bootstrap/conventions.json` receipt. Record that evidenced
floor in NORTHSTAR and append LEDGER. Do not read Mode
B or benchmark holdout.

## Stage 4 - Mode B (`missing_mode_b`)

After conventions are frozen, implement Mode B fitting from NBBO and aggregate
comparison of fitted vol, price, and greeks. Run targeted tests plus smoke+tune only.
Write `atx-vol/bench/oracle/bootstrap/mode-b.json` with the full git SHA, command,
exit code, complete Mode B target set, smoke/tune blob IDs, and positive aggregate
count exactly as specified above. Do not
change conventions or holdout membership and do not benchmark holdout.

## Ready-state failure rule

In `ready`, Measure sees smoke+tune and produces a self-contained aggregate payload;
the tool-less Analyst sees only schema v2: the complete enumerated Mode A/B target
registry, two aggregate baselines, a positive frozen speed pin, validated IDs, and
a closed convention enum map. It has no workspace, arbitrary-string field, paths,
hashes, membership, or row values. If any
mandatory vol-sprint lane is blocked/incomplete, lacks a fresh APPROVE, or fails the
isolated integration gate, the oracle run returns `FAILED`: no holdout benchmark,
no Ratchet, and no REJECT-counter change. Ratchet alone opens holdout: it leases a
new worktree at the exact reviewed sprint integration SHA, recomputes membership,
and compares it with the receipt frozen at run start. Ratchet prepares typed
baseline/candidate/delta, digest, required-gate, pinned-speed, and per-suspect typed
NBBO receipts. Workflow code owns metric IDs/classes/directions/baselines/limits,
the speed pin, exact gate commands, and bounded numeric suspect identifiers; it recomputes NBBO distances and verifies
the market is strictly closer to atx-vol before exclusion. It then computes ACCEPT/REJECT; the agent
verdict is never authoritative. For ACCEPT only, a minimal finalizer compare-and-
swaps `refs/heads/oracle/canonical` to the prepared commit, then an independent
auditor reads the actual ref. REJECT leaves it unchanged. If the finalizer report
is missing, audit truth is returned rather than assuming the old SHA.
