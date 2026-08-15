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

Capability freezes `refs/heads/oracle/canonical` (or the requested base before the
first landing) and reads only the already committed digest receipt; it never opens
holdout membership. Every lane starts from that SHA, uses a run-unique branch and
durable heartbeat lease, commits explicit paths, and releases with the same
`run_id`. Success evidence contains only exit-code-zero command output. Licensed
row values, option membership, and holdout membership must never enter prompts or
logs.

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
three cohorts, and frozen hash validate with pasted aggregate evidence.

## Stage 2 - Mode A (`missing_mode_a`)

Implement `atx-vol-oracle-bench` Mode A first. It reads cohort-selected parquet
with predicate pushdown, prices SpiderRock inputs with `srVol`, compares aggregate
price/greek cells, and isolates all comparison semantics in
`oracle_conventions.*`. Mode B must not be implemented or stubbed in this stage.

Targeted unit tests cover band edges, tolerance accounting, sentinel/null handling,
and aggregate-only reporting. Run smoke only and write
`atx-vol/bench/oracle/bootstrap/mode-a.json`, an aggregate capability receipt with
the full git SHA, command, exit code, scorecard schema version, smoke manifest hash,
and rows processed (never rows themselves). Holdout is forbidden.

## Stage 3 - conventions (`missing_conventions`)

Using Mode A on smoke+tune only, resolve theta/vega scaling, rate/borrow/dividend
treatment, day count, `vo`/`va`, signs, and share scaling. Commit the winning map to
`atx-vol/bench/oracle/CONVENTIONS.md`, encode it only in the isolated convention
layer, and write aggregate `scorecards/iter-000.json` with the Mode A residual
floor. Record that evidenced floor in NORTHSTAR and append LEDGER. Do not read Mode
B or benchmark holdout.

## Stage 4 - Mode B (`missing_mode_b`)

After conventions are frozen, implement Mode B fitting from NBBO and aggregate
comparison of fitted vol, price, and greeks. Run targeted tests plus smoke+tune only.
Write `atx-vol/bench/oracle/bootstrap/mode-b.json` with the full git SHA, command,
exit code, schema version, smoke/tune hashes, and aggregate counts/timing. Do not
change conventions or holdout membership and do not benchmark holdout.

## Ready-state failure rule

In `ready`, Measure sees smoke+tune and produces a self-contained aggregate payload;
the tool-less Analyst sees only that payload, with no workspace, paths, hashes,
membership, or row values. If any
mandatory vol-sprint lane is blocked/incomplete, lacks a fresh APPROVE, or fails the
isolated integration gate, the oracle run returns `FAILED`: no holdout benchmark,
no Ratchet, and no REJECT-counter change. Ratchet alone opens holdout: it leases a
new worktree at the exact reviewed sprint integration SHA, recomputes membership,
and compares it with the receipt frozen at run start. ACCEPT atomically advances
`refs/heads/oracle/canonical` to the final Ratchet commit; REJECT leaves it unchanged.
The result contains pasted Ratchet evidence and evidence-indexed aggregate metric
deltas so PM reports can be verified without exposing licensed rows.
