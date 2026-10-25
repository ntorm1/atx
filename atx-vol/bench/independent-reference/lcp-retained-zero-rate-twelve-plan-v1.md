# Prospective retained-policy twelve-request PDE control

Status: prospective scope for review before implementation. The saved-system
candidate at `c670ebbd` passed all four original float/exact 1e-10 gates in one
fixed run, recorded in `0729eaee`. That does not establish a complete PDE or
Greek control. This plan connects the already reviewed primitive to the existing
fixed twelve-request harness; it introduces no new study framework.

## Fixed requests and numerical boundary

Use exactly the existing `zero_rate_closed_form-put-03` model: S=K=100, T=1,
sigma=0.2, r=q=0, put. Retain both `dirk_l` and `dirk_third` and all six existing
v2 grids in their original order:

| Label | nx | nt | Domain multiplier |
|---|---:|---:|---:|
| space-coarse | 4097 | 512 | 1 |
| center | 8193 | 512 | 1 |
| space-fine | 16385 | 512 | 1 |
| time-coarse | 8193 | 256 | 1 |
| time-fine | 8193 | 1024 | 1 |
| domain-wide | 16385 | 512 | 2 |

Keep `greek_reference.py`, `pde_reference.py`, `lcp_correction.py` and
`lcp_policy_retention.py` byte-identical. The new explicit mode is
`one_correction_retained_admissible`. A fresh private Greek module receives a
caller-owned collector, which calls the reviewed retained primitive for each
actual stage. Each stage owns a fresh selector; no policy memory crosses stage
boundaries, requests or processes.

The primitive retains the original n+2 algorithm bound and original unscaled
1e-10 residual admission. Its separately declared effective linear-call budget
remains `min(2*nx,256)` (n=nx-2): at most 128 paired updates per stage. Do not
enlarge this to the old correction mode's `2*nx` call budget. A reached cap is a
refusal, with no retry or tuning. The original correction/default mode retains
its previous behavior and old receipt schema.

Retain the original PDE's graded time mesh, boundary/payoff, exact grid/geometry
checks and 2^27 stage-node guard. The fixed twelve requests contain at most
13,288 stages, so the additional paired-call cap bounds attempted linear calls
by 3,401,728 across the full set. These are ceilings, not predicted work or a
speed claim. No automatic full144 run follows, whatever the result.

## Minimal source changes

Extend only the existing `lcp_stage_adapter.py`, `lcp_zero_rate_control.py` and
`lcp_zero_rate_run.py`, plus their focused tests and documentation. Add a
separately versioned retained-mode receipt, selected explicitly at the CLI.
Defaults, existing original/private parity and the old one-correction receipt
reader must remain compatible. Do not relabel archived old outcomes as the new
mode. The new verifier dispatches by schema/mode and binds all newly used
retention sources to their reviewed immutable identities.

Reuse the current source snapshots, immutable historical144 receipts, fixed
request declaration, streamed/flushed outcome rows, owned failed-stage NPZ,
independent model/geometry/stencil checks and summary machinery. Preserve the
existing old one-correction result archive as a read-only comparison, without
rerunning its PDE requests. Comparison of completed-work and refusals can be
recorded separately after the new result; no baseline outcome is fitting input.

## Work and refusal accounting

For each completed or typed numerical-refusal stage, retain the original work
counters plus `base_mode` and the primitive's `policy_work`. Validate the
distinct effective call budget, original n+2 bound, attempt/completion phases,
global residual admission and PDE aggregate joins. Selector counters must be
nonnegative integers, preserve the 32*n reservation, and satisfy actual
selection/row conservation. For full observed work, completed selections equal
attempted policy solves, initial rows equal n iff any selection occurred, and
admitted-retained plus failing-reselected rows total (selections-1)*n. Retained
overrides cannot exceed retained rows; changes cannot exceed reselected rows.

These stage-level counters are source-bound observations. Unlike the four saved
trajectory reader, this harness does not store every stage's policy arrays and
must not claim independent reconstruction of every policy selection.

A retention allocation/workspace failure without original solver counters
retains the available typed selector diagnostics and explicitly unknown original
work. It cannot become an invented zero-solve stage or an algorithmic-limit
failure. Unexpected snapshot/export exceptions leave an incomplete run.
Numerical refusals retain owned original arrays and the matching last checked
vector, when present; the existing reader recomputes its residual.

Keep all twelve outcomes, all714 uncomputed contracts and six uncomputed cold
Greek fields. Retain the original independent Black-Scholes price/Delta/Gamma
diagnostics and exact v2 refinement/analytic-interval rule. A control pass still
requires twelve computed requests and both original refinement rules passing.
Statuses remain `not_promoted`; no historical accuracy decision changes.

## TDD and execution gate

Use only tiny nx=33/nt=4 (or smaller) numerical adapter controls and synthetic
protocol rows during implementation. Test actual private routing and fresh
selector lifetime, original-mode bit equality, unchanged global namespaces,
complete stage accounting, cap/typed/unknown allocation refusals, owned failed
snapshots and the new selector conservation checks. Add rehashed wrong-mode,
budget, source, partial-work and phase mutants. Verify existing immutable
one-correction receipts with the default reader, without rerunning PDE.

Fresh exact-source review is required after focused tests and source freeze.
Only then execute the twelve declared requests once into a new directory with
clean before/after source/input identities and the existing independent reader.
No change of grids, tolerance, selection rule, cap or decision criterion is
authorized after seeing outcomes. Any further PDE or reference-promotion study
requires its own prospective scope.
