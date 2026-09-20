# AF1 root verification and acceptance

Root accepted the single independent review's two Important findings on the
implementer's fix3 report and focused execution. No Critical finding required
another review. Production coverage remains unmeasured.

- Initial six-file run: 76/79 passed. The three failures were forbidden NULL
  standardized-source fixture inserts and an outdated frame wrapper signature.
  Fix2 preserved the NOT NULL source schema, canonical invalidation assertions
  and frame/scope checks. All three affected selectors then passed, peak job
  memory 0.7906875610351562 GiB.
- After review fixes, both counterexample regressions and four relevant
  preservation cases passed (6/6), peak 0.7892837524414062 GiB. This verifies
  independent complete-quarter retention, annual weighted-share coherence,
  invalidation/zero handling, exact annual composition and endpoint protection.
- Ruff passed all eleven AF1 paths; after the final mechanical type fix,
  derived_metrics.py passed again. Strict mypy passed _derived_annual.py,
  _derived_pit.py, migrations/bodies_0316.py and derived_metrics.py together.
  The expanded check exposed six un-narrowed COUNT result lookups (four P1,
  two AF1). Fix4 adds explicit non-None assertions without changing SQL,
  query counts, candidate/frame limits or publication behavior.
- Tests used the project Python with -n0 under a 2.5 GiB process-tree guard.
  Static checks had individual 1.5 GiB caps. No production writer ran during
  verification, and the production policy remains 1 GB/one DuckDB thread
  within a 3 GiB guard.

Evidence: annual-fallback-fix2-focused-memory.json, fix3-focused-memory.json,
their logs, annual-fallback-final-ruff-memory.json, fix4-ruff-memory.json and
fix4-mypy-memory.json. The earlier initial-run and fix1 evidence remain under
their original names. Review/fix reports distinguish root execution from
agent-only static work.

Commit the eleven AF1 source/test paths and migration 0316 together. The
separate root documentation/snapshot refresh and committed-HEAD checks must
complete before production resumes. No full-universe derived result, annual
coverage threshold or significant custom signal is claimed by this acceptance.
