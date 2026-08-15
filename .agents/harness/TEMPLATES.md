# Harness contracts — brief / report / review

Formalizes the existing `.superpowers/sdd/<sprint>/task-N-{brief,report,review}.md` convention. Copy the relevant section; keep every heading, delete nothing silently.

## Lane brief (planner → builder)

```markdown
# Lane <id>: <title>
## Goal
One paragraph. What exists after this lane that does not exist now.
## Scope
- Files in scope: <explicit list — the ONLY files this lane may create/modify>
- Files forbidden: <files adjacent lanes own — touching these is a lane failure>
- Branch: lane/<id> off <frozen-base-sha>
- Lease: pool-N acquired with <workflow-run-id>
## Ladder
- check targets: <the .cpp files to type-check while shaping>
- build target(s): <e.g. atx-vol-tests>
- suites: <gtest -R filters that must go green>
## Done criteria
Falsifiable. "Suite X passes, suite Y still passes, no new warnings" — never "improved".
## Out of scope
Named explicitly, so the builder does not helpfully drift.
```

## Lane report (builder → reviewer/verifier)

```markdown
# Lane <id> report
## Outcome
DONE | BLOCKED — one sentence.
## Branch / SHA
lane/<id> @ <sha> (all work committed; leased tree left clean)
## Frozen base / lease
base_sha=<40-char-sha>; worktree=<C:\atx-wt\pool-N>; lease_name=<pool-N>;
lease_run_id=<workflow-run-id>
## Files changed
<list>
## Evidence
For each command: exact command, integer exit code, and verbatim non-empty output.
Every brief check target, build target, and suite must occur in an evidence command.
Claims without pasted output are void.
## Deviations from brief
<none | list with reason>
## Ledger candidates
0-3 one-line facts worth remembering forever (trap found, number measured, decision made).
```

## Review (reviewer → verifier)

```markdown
# Lane <id> review
## Verdict
APPROVE | BLOCK
## Reviewed SHA
<exact current lane SHA; every Fix requires a new review>
## Evidence
At least one independently run command with exit code and pasted output.
## Findings
path:line | severity (blocker/major/minor) | problem | required fix
Correctness-scoped: UB, lifetime, bounds, error paths, test gaps, contract breaks.
NOT style, NOT formatting (clang-format owns that), NOT scope expansion.
## Checked
- [ ] .agents/cpp/agent.md §10 checklist applied to the diff
- [ ] Diff stays inside brief's files-in-scope
- [ ] Evidence in report matches claims (re-ran spot checks where cheap)
```
