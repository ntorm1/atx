# atx-vol DAG development harness — design

## 2026-08-15 correctness hard cutover

`vol-sprint` freezes the requested base ref to a full SHA before planning. Every
planned lane is mandatory. Build reports carry structured command/exit/output
evidence tied to the lane's required checks, and reviews bind their verdict to the
exact lane SHA. A BLOCK may receive one Fix; that new commit always gets a fresh
review. Any dead, BLOCKED, contract-invalid, or non-APPROVE lane aborts before
integration.

Pool leases are atomic v3 records keyed by a workflow `run_id`, desired branch,
frozen base SHA, acquisition time, and an explicit durable owner. The owner is
either a caller-supplied PID plus exact process-start timestamp or a run-unique
heartbeat with an independently running continuous keeper. Acquisition waits for
an authenticated ready pulse before publishing keeper identity. Foreground commands and
before/after pulses are not ownership; the short-lived lease launcher is rejected.
Release requires the same `run_id`; stale recovery is explicit and refuses while
the durable owner is alive. Corrupt/truncated records fail closed. Tests use a temp
pool root and never touch production markers.

After all mandatory lanes are freshly approved, their leases are released before
integration begins. The verifier then obtains a new run-owned heartbeat lease for
a run-unique integration branch. Its typed receipt proves keeper ownership. The
verifier reports one integration receipt per exact reviewed SHA, an exact HEAD
receipt, one successful receipt for every required scoped gate, and a typed release;
it performs merges, gates, and ledger work only under
`C:\atx-wt\pool-N`; `C:\atx` is never an integration worktree. Failure still
releases the integration lease. A success claim is valid only when every supporting
evidence item has `exit_code=0`; failed attempts are kept separately as diagnostics.
This section supersedes v1 error/cleanup semantics below where they differ.

Date: 2026-08-15. Status: v3 correctness hard cutover implemented. Goal: faster dev loops, durable long-term memory, real parallelism for atx-vol work, built on what the repo already has (worktree pool, `atx-build.ps1`, `.agents/` role docs, `.superpowers/sdd/` brief convention) instead of replacing it.

## Research grounding (why this shape)

- Anthropic taxonomy: predefined-path **workflows** beat free-form agents for structured dev work; add agency only where decomposition is genuinely dynamic (the Plan stage). [anthropic.com/engineering/building-effective-agents]
- Multi-agent ≈ 15× token cost; pays only for genuinely parallelizable slices, and "most coding tasks involve fewer truly parallelizable tasks than research" — hence the ≤4-lane cap, disjointness rule, and single-lane default. [anthropic.com/engineering/built-multi-agent-research-system]
- Duplicate work comes from vague delegation → lane briefs carry explicit files-in-scope **and files-forbidden**, output contracts, falsifiable done criteria.
- Fresh-context adversarial review (reviewer sees diff + criteria, not writer's reasoning) beats self-grading; reviewers report everything, filtering happens downstream. [code.claude.com/docs/en/best-practices]
- Memory: small auto-loaded CLAUDE.md spine + grep-retrieved curated files beats embedding stores for code work; aggressive summarization loses late-relevant context, so the ledger is append-only one-liners with pointers. [anthropic.com/engineering/effective-context-engineering-for-ai-agents]
- File collisions: per-agent worktrees. Repo already solved this better than stock `isolation: worktree` — the warm pool (5 s no-op vs 132 s cold, submodule-safe) — so builders lease pool trees instead.

## Components

| Piece | Path | Role |
|---|---|---|
| Memory spine | `CLAUDE.md` (root), `atx-vol/CLAUDE.md` | Auto-loaded facts: build wrapper, pool, ladder, labels, gates, traps, memory protocol |
| Fact ledger | `atx-vol/docs/LEDGER.md` | Append-only `date \| area \| fact \| source` lines; grep before re-deriving; verifier appends on gate |
| Contracts | `.agents/harness/TEMPLATES.md` | Brief / report / review formats (formalizes `.superpowers/sdd/` convention) |
| Agents | `.claude/agents/vol-{planner,builder,reviewer,verifier}.md` | DAG stage roles; builder/reviewer read `.agents/cpp/agent.md` at start (single source of truth, not duplicated into prompts) |
| DAG | `.claude/workflows/vol-sprint.js` | Plan → pipelined Build/Review/Fix per lane → barrier Gate; JSON-schema outputs at every edge |
| Playbook | `.claude/skills/vol-dag/SKILL.md` | Shape decision (inline vs single-lane vs DAG), invocation, memory duties |
| Permissions | `.claude/settings.json` | Allowlist for wrapper/lease/git-read commands — fewer prompts, faster lanes |

## DAG semantics

- **Pipeline, not barriers**, between Build→Review→Fix: lane A reviews while lane B builds; wall-clock = slowest lane chain.
- **Barrier before integration** (needs all lanes): every mandatory lane must be
  DONE and freshly APPROVED at its final SHA. Release lane leases, acquire a new
  isolated integration lease, merge exact reviewed SHAs, prove HEAD, and run the
  exact workflow-derived changed-file registry. Every changed path must have one
  exact path or anchored path-pattern owner with mandatory gates; unknown paths and
  unrelated target/test mappings fail closed. Planner additions cannot substitute
  for those mandatory gates. Unit-test mappings name real fully-qualified
  discovered tests and run through the semantic targeted adapter, so zero-test
  exit-0 receipts fail. The
  registry contains affected anchored unit tests,
  hypothesis OracleBench tests, aggregate smoke/tune Mode A/B scorecards, quiet
  pinned speed, and owning PCH-off targets for changed headers. Labels, broad/full
  runners, full-repo hygiene, and release suites fail closed and remain outside the
  oracle loop. Then append ledger and release integration.
- **Per-sprint concurrency is capped at four lanes; the machine pool may grow to 20
  slots** so unrelated workflows can proceed concurrently. Builder without a lease
  reports BLOCKED rather than touching main. Lane leases stay held through Fix and
  mandatory re-review, then are released before integration acquisition.
- **One fix round plus mandatory re-review.** A second BLOCK fails the sprint before
  integration — no unbounded loops and no stale verdict reuse.
- **Evidence discipline end-to-end**: builder claims void without pasted output or
  with a nonzero success exit code; diagnostics are separate. APPROVE plus any blocker
  is invalid. The verifier reports the exact integrated SHA list and is the only
  authoritative pass/fail; golden-replay is SKIPPED (not passed) when absent.

## Speed levers

1. Warm pool trees (5 s/27 s vs 132 s cold) as the parallel substrate.
2. Closed workflow-owned per-file mappings derive anchored unit/OracleBench commands;
   unknown paths, unrelated mappings, omitted receipts, and extra receipts fail.
   No broad or full-suite fallback exists in the loop.
3. Permission allowlist removes per-command prompting in lanes.
4. Pipelined review overlaps QA with implementation.
5. Ledger + CLAUDE.md kill re-derivation (build traps, measured numbers, past decisions available at session start / one grep away).

## Error handling

Planner returns 0 lanes → workflow throws. Any mandatory lane BLOCKED/died or lacking
a fresh APPROVE → workflow releases known run-owned leases and returns FAIL before
integration. Merge conflict → verifier stops and names lanes/files. Gate FAIL →
reported with structured evidence, branches left for inspection, integration lease
released. Workflow death mid-run → `lease-worktree.ps1 -Status` for durable
owner/heartbeat state and run_id; investigate before explicit stale recovery or resume.

## Not in v1 (deliberate)

Stop-hook enforcement gates (documented convention first; hooks if drift shows), CI
runner integration (repo has none for C++), auto ledger compaction, and atx-engine
generalization. A custom asynchronous/headless runner is also deferred: prototype
the host's native background workflow controls and completion notification first;
do not replace `vol-sprint.js` or add a second polling/orchestration stack.
