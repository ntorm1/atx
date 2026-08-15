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
- Branch: lane/<id> off <base>
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
## Files changed
<list>
## Evidence
Commands actually run, with pass/fail counts pasted — build target, ctest -R output.
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
## Findings
path:line | severity (blocker/major/minor) | problem | required fix
Correctness-scoped: UB, lifetime, bounds, error paths, test gaps, contract breaks.
NOT style, NOT formatting (clang-format owns that), NOT scope expansion.
## Checked
- [ ] .agents/cpp/agent.md §10 checklist applied to the diff
- [ ] Diff stays inside brief's files-in-scope
- [ ] Evidence in report matches claims (re-ran spot checks where cheap)
```
