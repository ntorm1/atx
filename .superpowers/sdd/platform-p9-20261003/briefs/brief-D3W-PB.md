# Brief D3W-PB: D3 part B as a stretch lane (pool-23, after D3W's merge; ruling D3W-PB)

Lane id `D3W-PB`. Branch: D3W's branch `feat/p9-d3w-20261005` continued in pool-23 after root has merged D3W (first
step: `git merge feat/platform-p9-20261003`). Stretch: merges only if APPROVE at the gate (GATE-1), else P10 whole.
Contract: `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`, `resume7-addendum.md`,
`resume8-addendum.md`. Rulings to grep: D3W-PB, D3-2, D2-1, D2-3, D2-7, ALCOMB-4, ALCOMB-5, P19, PRE-35, EXE-FREEZE.
Inputs: D3W's `task-D3W-report.md` "For part B"; `briefs/brief-D3.md` item 6; `wave3-carry.md` D3 section;
`handoff-D3.md` (pool-20) "Design decided" (fingerprint constraint).

## Goal

Move the `ew-theme-aim-v2` fit into the C++ composition fit (`atx-engine/.../research/composition/fit.*`, the
`atx-research-composition` CLI) with the resid diagnostics, so the Python fitter's copy can be deleted (D2-3) after
root's identity; the fitter keeps `producer_fingerprint` unchanged for every existing mode (no edit inside `fit` /
`fit_prior` or their closure; the post-step pattern D3W established).

## Scope

- The C++ fit mode and its CLI flag(s); `composition_fit.json` keys for it; gtests (closed form on a planted model;
  refusals with teeth; flag absent = bytes unchanged).
- The fitter post-step under `--engine-fit` for that mode; P12 comparison vs the Python side; the D2-3 deletion as the
  LAST commit of the branch (after the identity tests; root merges the pre-deletion head, runs identity, then the
  deletion commit, as A3 / B2 precedent).
- Tests: the composition gtests, the fitter pytests, `scripts/tests` seeds 0 / 1, the stage chain.
- Out of scope: the kind files (D3), the IC runner (ABS), any NAV change, part A items.

## Done criteria

Identity: every existing fit mode's outputs byte-identical; the new mode equals the Python fit to the P12 tolerance on
the fixtures; the deletion commit is separate; report `task-D3W-PB-report.md` with the TRAIN identity procedure for
root and "For PRE" (nothing: no cell reads it in P9).
