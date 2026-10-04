# Wave-2 lane review contract (PM)

Adversarial review, read-only, of ONE lane head. ROOT-SDD = `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`.
The dispatch names: LANE, POOL (the lane's worktree), BASE and HEAD SHAs.

Read: ROOT-SDD/`briefs/brief-<LANE>.md` (requirements); ROOT-SDD/`wave2-carry.md` ("All lanes", "Brief text
superseded", the lane's section; the carry wins over the brief; "required" items are required); rulings in
ROOT-SDD/`progress.md` by grep of the lane id and of ids the carry cites (never read the whole ledger or plan); the
lane report `<POOL>/.superpowers/sdd/platform-p9-20261003/task-<LANE>-report.md`; `C:/atx/.agents/cpp/agent.md` §10
and `C:/atx/.agents/harness/TEMPLATES.md` "Review".

Diff: `git -C <POOL> diff <BASE> <HEAD>` written to a scratch file and read from there; files with
`git -C <POOL> show <HEAD>:<path>`. Edit nothing, build nothing, run no real data, open no 2020-2023 output and nothing
dated 2024-01-01 or later (synthetic test fixtures are fine). Re-run at least one of the lane's pytest commands inside
POOL with explicit paths, `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`; give exit code and output tail as
evidence. Kill only processes you started.

Two verdicts, both required: (1) spec compliance against brief + carry + rulings, every required carry item checked
one by one; (2) quality. Look for: flag-absent identity broken (a default changed, an output key added
unconditionally, iteration order changed); look-ahead (a window ending at d instead of d-1 / d-2; a label or return
read before it matures); seal (a path or row at or after 2024-01-01 decoded); an expected hash or expected value
edited to fit; a Python copy of a C++ rule kept or added; files outside the lane's scope and unlisted cross-lane
edits; because lanes did not compile: C++ that cannot build (missing includes, wrong signatures against the headers
called, CMake entries), `/W4 /WX` hazards (unused, sign conversion, shadowing, narrowing), 100-column limit;
lifetime, bounds, error paths; determinism (unordered iteration, thread-order writes); tests that are tautologies or
pin the implementation instead of the contract; registry rows that disagree with the code; drift from K-P9-n
contracts; root-cause claims that the evidence does not support.

Write `<POOL>/.superpowers/sdd/platform-p9-20261003/task-<LANE>-review.md` (the only file you write; do not commit):
verdict APPROVE or BLOCK; spec verdict; quality verdict; findings as
`path:line | severity (blocker / major / minor) | problem | required fix`; "what root must check at merge".
APPROVE with a blocker or major open is invalid. Final message to the PM: at most 12 lines -- verdict, counts by
severity, each blocker / major in one line, anything needing a PM ruling.
