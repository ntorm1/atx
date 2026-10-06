# Brief ABS: the IC exe's `absent-members-v1` capability (pool-28, `feat/p9-abs-20261006`; ruling ABS-1)

Lane id `ABS`. Pool-28 was leased at root docs head `1b64ce45`; FIRST step: `git merge <D3W's APPROVE head>` (the
PM's prompt names it), then `git merge feat/platform-p9-20261003`; never rebase. Contract: `resume4-dispatch.md`,
`resume6-addendum.md`, `r6-wave3-common.md`, `resume7-addendum.md`, `resume8-addendum.md`. Rulings to grep: ABS-1,
OD3P-1, ALCLOCK-2, PRE-31, PRE-10, EXE-FREEZE, EXE-CUR, S2-2, D3-2. Input of record: the contract
`C:/atx-wt/pool-21/.superpowers/sdd/platform-p9-20261003/od3p-absent-contract.md` (OD3P; sections 2-5 are the spec)
and D3W's `task-D3W-report.md` "For ABS" (where `IcComposition::add` is left extensible).

## Goal

The IC exe (`atx-equity-strategy-ic`) accepts `--absent-members <path> --absent-members-sha256 <64 hex>` (both or
neither, each at most once) and treats every listed member on every date exactly as a member with no data on that
date (the engine's existing missing-member rule: it adds nothing to the composite; its mass stays in its theme; a
theme with every member listed = a theme with no present member); the pinned TRAIN weights, signs, theme blocks and
verify are unchanged (theme-erc-v1 and ic-shrink bases accepted). Capability `absent-members-v1` appears in the
`capabilities` array of `--list-rules --json` (`strategy_ic_rules.cpp` `exe_capabilities`, sorted). Flag absent =
every byte of every pass unchanged.

## Scope (D3W's files, after D3W: the IC runner / composition files)

- Parsing and refusals (contract section 5's list: one flag without the other; repeated; file missing / unreadable;
  SHA-256 mismatch; not JSON / not an object; `schema` != `atx.od3-flagged-members/v1`; `members` missing / not an
  array / empty / non-string / repeated / bad id; an id not a candidate of `--library`), each before any output, each
  with a test with teeth.
- Semantics in `IcComposition` (`add` / `add_standardised`: the listed member's cell is treated as non-finite /
  missing on every date; masses fixed at create from the pinned weights are untouched; the no-theme blend keeps its
  fixed denominator). The v3 walk-forward rows (D3W) treat a listed member the same way.
- `summary.json` gains one top-level key with the flag: `absent_members {rule, list_sha256, members}`; absent
  without it. Caches: the list changes no signal-cache / IC-result-cache key or bytes, and a cache dir shared by runs
  with and without the flag never serves one run an entry whose use would change its bytes (state how: a cache key
  term, or the composite computed outside the cached entries).
- `--no-composition` with the flag: refuse the pair (the simplest; the cycle never sends it).
- Gtests (`atx-impl/tests/strategy_ic_runner_test.cpp` fixtures; tiny world; a library where member m's DSL reads a
  field no other candidate reads): the defining equivalence of contract section 5 for cases 1-6 (byte-for-byte over
  every published output except wall-clock / stage-seconds / cache keys and the `absent_members` record; fresh
  caches both runs); flag absent = bytes unchanged (the existing goldens and identity tests hold as they are);
  the capability line; the refusals with teeth (one extra build with every refusal disabled, revert).
- Out of scope: the Python side (done in OD3P), the NAV exe, the kind files, part B.

## Builds and suites

`equity-dev` (tags `p9-abs-a`, ...), targets `atx-equity-strategy-ic,atx-impl-strategy-ic-tests` (+ the canary targets
with `-Canary` if the runner's bytes could move: they must not), named filter `AbsentMembers*` then the whole
`atx-impl-strategy-ic-tests`; Release once at the end; `scripts/tests` seeds 0 / 1 and the stage chain only if a
Python test executes anything you changed (the cycle's test of the capability reads a fake: check
`scripts/tests/test_research_cycle_od3.py` still passes against the real `--list-rules --json` if a test uses it).

## Done criteria / report

`task-ABS-report.md` + `handoff-ABS.md` in pool-28's sprint dir (`git add -f`): each refusal -> file:line -> test ->
teeth log; the equivalence cases 1-6 with the output set proved (if per-member rows cannot hold as stated, say exactly
which outputs are proved and why; the PM rules); the flag-absent identity procedure for root (argv; the files
expected byte-identical); what root re-proves at merge (the cycle's plan on `scripts/specs/p9/od3/` refusing without
the capability and planning with it, on fakes). ABS must be merged and APPROVE at the gate (EXE-FREEZE), else the
clean-source line prints "not read: no producer".
