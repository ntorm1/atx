# Resume contract (owner instruction, 2026-10-03)

Read with IMPLEMENTER.md / REVIEWER.md. Owner explicitly selects gpt-6.1-sol implementers. No child agents. Work only in C:/atx on feat/tier1-v3-warehouse, no worktrees, no branch switching. At most three lanes globally. Every real-data command, including diagnostic SQL/Python and network fetch/build scripts, must use run_memory_guarded.py with <=1 GiB. Offline fixture tests need no guard. Do not kill any process you did not start or delete git lock files.

Never edit/stage foreign-dirty-files.txt paths. Explicit-pathspec commits only. Follow GC6 trailer as the existing plan requires; report actual agent model truthfully. Reports and controller scratch artifacts belong to .superpowers/sdd/tier1-v4. Implementers never modify progress.md, HANDOFF.md, shared contracts or another task's files.

No return statistics on dates >=2023-01-01. Predictive evaluation must additionally require the entire future label window to end before 2023; do not tune using held-out data. Value/coverage checks after 2022 remain authorized.

Prefer codebase graph tools for code discovery if available. The controller found no callable graph tools in this runtime; targeted rg / source reads are an authorized fallback for insufficient tools. Use UTF-8 for file reads and writes. Do not paste diffs or full reports back to controller. Report status, commit SHAs, test summary, gate counts, concerns, artifact path in <=15 lines.

Reviewers are read-only except writing their assigned findings Markdown file (no staging/committing it). Do not run full suites/builds. Named-risk focused checks only. Keep full findings in the artifact and return a short summary with both spec and quality verdicts.
