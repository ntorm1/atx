# Brief SIGN: the `sign_pm7_35` C++ verb route (pool-29, `feat/p9-sign-20261006`; ruling C3-11; stretch)

Lane id `SIGN`. Pool-29 was leased at root docs head `1b64ce45`; FIRST step: `git merge <C3b's APPROVE head>` (the
PM's prompt names it), then `git merge feat/platform-p9-20261003`; never rebase. Contract: `resume4-dispatch.md`,
`resume6-addendum.md`, `r6-wave3-common.md`, `resume7-addendum.md`, `resume8-addendum.md`. Rulings to grep: C3-11,
W3-6, G-P5, B2-10, W1-I1, W1-I2, E2-16, EXE-FREEZE, EXE-CUR, C2-9, C2-11. Stretch: merges only if APPROVE at the gate
(GATE-1), else P10 whole and the Python copy stays a named G-P5 exception.

## Goal

Retire the Python copy of the PM7-35 sign rule (`scripts/wave_rules.py:35-57` `sign_pm7_35`, `SIGN_RULES`) by a C++
verb that applies B1's header-only predicate `adm::pm7_35` (`atx-engine/include/atx/engine/research/admission/
sign_rule.hpp:86-92`, `SignRule::WaveZeroKeptV1`; decision table `atx-engine/tests/research/
research_admission_sign_test.cpp:46-75`) to a rows file and writes a receipt; the wave's screen stage calls the verb
on an exe the screen-stage wave already pins (no new pinned-exe key: W1-I2 / B2-10 unchanged); root's identity (the
screen rows of the ledgered Y-2, Y-3, Y-5, Y-S wave results and the decision table, row for row against the Python
rule) precedes the deletion commit.

## Scope

- The verb: a subcommand of an exe the screen stage already pins (find which exe `scripts/wave_stage_*.py` names for
  the screen stage and its `exes` key; prefer `atx-research-eval` or the IC exe, whichever the stage pins; state the
  choice and why). Input: the screen rows file the wave writes (its schema as the Python reads it: `status`,
  `runner_sign`, kind, prior); output: a receipt JSON with the verb's decisions `(verdict, reason)` per row and the
  input's SHA-256; refusals (missing / malformed rows, unknown kind, unknown rule id) before any output, with teeth.
- Gtests: every row of the decision table of `research_admission_sign_test.cpp:46-75` through the verb; refusals.
- The wave: the screen stage calls the verb (one call line in the stage; the Python `sign_pm7_35` is called no more);
  flag / route absent = the ledgered v1 manifests still validate and their results are untouched (as C3b did for
  `pm6-6`); the G-P5 row for `sign_pm7_35` in `scripts/tests/test_no_python_mirror.py` is deleted in the SAME commit as
  the function (the LAST commit of the branch, after the identity tests), as C3b / B2 precedent; the verb's name
  enters `FROZEN_PAIRS` or the file's equivalent so the name stays detected.
- Identity test (pytest, on fakes + the fixture rows): the verb's decisions equal the Python rule's on every
  `(status, runner_sign, kind, prior)` combination the Python test `scripts/tests/test_research_wave.py:62-71`
  enumerates, before the deletion; root re-runs it on the ledgered wave results' screen rows (listed in the report by
  path; the lane does not open them: they carry no statistic but are root's to read).
- Out of scope: any other wave rule; the NAV / IC composition; the kind files.

## Builds and suites

`equity-dev` (tags `p9-sign-a`, ...), the exe's target + its test target, named filter `SignVerb*` (or the name you
choose), whole target tests; Release once at the end; the wave suites, `test_no_python_mirror.py`, `scripts/tests`
seeds 0 / 1, the stage chain.

## Done criteria / report

`task-SIGN-report.md` + `handoff-SIGN.md` in pool-29's sprint dir (`git add -f`): the exe and key chosen; verb ->
file:line -> tests -> teeth; the pre-deletion head and the deletion commit named; the identity procedure for root
(argv; the ledgered results' screen rows by path); what root re-proves at merge. SIGN merges only if APPROVE at the
gate; hand off cleanly (WIP commit + handoff) rather than hold the gate.
