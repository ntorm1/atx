# P9 RESUME 7: lane review contract (PM, 2026-10-05)

Adversarial review of ONE lane head by a fresh agent. ROOT-SDD = `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`.
The dispatch prompt names: LANE, POOL (the lane's worktree), BASE and HEAD SHAs, the lane's section of
`r7-dispatch.md`, the ruling ids to grep, and any lane-specific trace.

## Read

1. ROOT-SDD `resume4-dispatch.md`, `resume6-addendum.md`, `resume7-addendum.md` (owner rules; ENV-1; PERM-1).
2. The requirements: the lane's section of ROOT-SDD `r7-dispatch.md`; `briefs/brief-<LANE>.md` if one exists;
   `wave3-carry.md` ("Brief text superseded" for that brief, "All lanes", the lane's section, "PM answers", "Added after
   drafting"; the carry wins over the brief; a ledger ruling wins over both); the rulings by grep of the ids the
   prompt names in ROOT-SDD `progress.md` (never read the whole ledger or the plan).
3. The lane's own records in `<POOL>/.superpowers/sdd/platform-p9-20261003/`: `task-<LANE>-report.md`, its handoff.
4. `.agents/cpp/agent.md` section 10 (in POOL) for C++; `.agents/harness/TEMPLATES.md` "Review".

## How

- Diff: `git -C <POOL> diff <BASE> <HEAD>` (use `rtk proxy git ...` for raw output) written to a scratch file in your
  own scratch dir and read from there; files with `git -C <POOL> show <HEAD>:<path>`. A merge of root inside the
  range: check it is plain (the merge tree equals what git recomputes), then review the lane's own hunks.
- You change no tracked file, build nothing new, run no real data, open no 2020-2023 output and nothing dated
  2024-01-01 or later (synthetic fixtures are fine), never run `atx-impl/tools/holdout_gate.py`.
- Evidence of your own: re-run the lane's pytest commands that matter most inside POOL (explicit paths,
  `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, own `--basetemp`; `scripts/tests` and the stage chain in
  separate invocations); if the lane left a built tree, run its named gtest filters; otherwise say "from the lane's
  logs only" for every C++ test claim. Teeth: for the lane's most important refusals, a working-tree mutation of the
  CODE under test (never of a test's assertions) that you revert byte-identically (`git status` clean after); if the
  permission system refuses an edit or command, do not retry it and do not route around it: report it.
- Blind: read root history only as one-line subjects; take nothing from public ledger lines with statistics.

## Two verdicts, both required

(1) Spec compliance: every numbered item of the lane's dispatch section and every required carry item, one by one:
done / not done / pending by rule (an item that waits on B2's merge is "pending", not a finding).
(2) Quality. Look for: flag-absent identity broken (a default changed, an output key added unconditionally, an
iteration order changed); look-ahead (a window ending at d instead of d-1 / d-2; a label or return read before it
matures; a decision at d reading anything but store row d); seal (a path or row at or after 2024-01-01 decoded); an
expected hash, golden or pinned value edited to fit; a ledgered or prereg-pinned file edited; a Python copy of a C++
rule kept or added (G-P5) outside the named exceptions; files outside the lane's scope and unlisted cross-lane
edits; `/W4 /WX` hazards, the 100-column limit, lifetime, bounds, error paths; determinism (unordered iteration,
thread-order writes, `--book-workers` 1 vs 4); tests that are tautologies or pin the implementation instead of the
contract; refusals without teeth; registry rows that disagree with the code; a registration that promises a figure
nothing produces; root-cause claims the evidence does not support.

## Output

Write `<POOL>/.superpowers/sdd/platform-p9-20261003/task-<LANE>-review.md` (a re-review: `task-<LANE>-rereview-<n>.md`)
and commit ONLY that file (`git add -f`; trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`); leave
the tree clean. Content: verdict APPROVE or CHANGES_REQUIRED; spec verdict; quality verdict; findings as
`path:line | Required / Suggested | problem | fix`; what you verified by your own runs vs from the lane's logs;
"What root must check at merge". APPROVE with a Required finding open is invalid. A Required finding must be
something that is wrong against the spec or unsafe to merge; wording, style and nice-to-have tests are Suggested.

Final message to the PM, at most 12 lines: verdict, review commit SHA, counts (Required / Suggested), each Required
in one line, your own test counts, any permission refusal (exact command), anything needing a PM ruling.
