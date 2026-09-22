# S4 Task 3 fix1 report

Baseline: `e3f52a9ea180f1ee81fdf7f9addd74985fbdbcb2` and `task-3-review.md`.
Branch: `feat/tier1-parity`. Implemented with Codex; no Claude execution or LLM API calls.
Status: complete. The report is included in the same pathspec-scoped fix commit.

## Review findings resolved

- **F1 (High): newest bankruptcy snapshot.** Select one current directory revision per
  evidence row, ordered by snapshot date descending with deterministic directory/status
  tie-breaks, before inspecting the `Q` flag. A newer normal or null status clears an
  older bankruptcy flag; a snapshot after the event date cannot affect that event.
- **F2 (High): source isolation.** Add the bound evidence source to the deterministic
  evidence ID. Refreshing one source no longer replaces rows belonging to another.
  Existing rows for the refreshed source are replaced by the normal scoped rebuild.
- **F3 (Medium): real historical cutoff.** `as_of_date` now bounds event dates and input
  availability at the program's 22:00 end of day. The bound applies to SEC Form 25/15,
  merger evidence, Nasdaq deletes, directory snapshots, and both archive sessions and
  per-security last bars. The fold independently filters dates and availability when
  consuming previously materialized evidence. `None` retains an unbounded rebuild;
  individual evidence `as_of_date` values remain the event date. Details record the
  requested cutoff.
- **F4 (Low): latest revisions.** Filter `is_latest_revision = true` for archive bars
  (both the session grid and last-bar aggregation), Nasdaq deletes, directory snapshots,
  and evidence entering the fold.

The cutoff work also enforces the standing `available_at = max(inputs)` ruling.
Merger classifications carry the later filing input timestamp. Bankruptcy assessments
carry the selected directory timestamp even when a normal/null snapshot clears the
bankruptcy flag. Archive-gap evidence carries the availability of its visible archive
horizon, conservatively avoiding backdating an inferred gap to the final trade itself.
Nasdaq availability uses file creation time, then its canonical availability, then the
snapshot date at 22:00 when neither timestamp is present.

## Focused verification

Regression coverage includes normal/null bankruptcy clears, per-event snapshot selection,
source coexistence and independent rebuilds, inclusive 22:00 cutoffs for each explicit
stream, future archive sessions and resumed trades, superseded source rows, delayed
merger/directory availability, and a separately bounded evidence fold.

- `python -m ruff check src/atx_db/delisting_evidence.py tests/test_delisting_evidence.py`:
  passed.
- `python -m ruff format --check src/atx_db/delisting_evidence.py tests/test_delisting_evidence.py`:
  passed.
- `python -m mypy --strict src/atx_db/delisting_evidence.py`: passed.
- `python -m pytest tests/test_delisting_evidence.py -n 0 -q`: **32 passed**, exit 0.
- `git diff --check` on both owned code files: passed.

The initial focused run collected 30 cases before two final regressions were added:
23 passed and seven fixture assertions failed. Direct SQL inserts in the existing and
new Nasdaq fixtures omitted `is_latest_revision`; unlike `delisting_evidence`, those
source tables have a nullable flag without a default. Production `warehouse.insert_frame`
explicitly supplies `true` for these inputs. Updated fixtures to mark current Nasdaq
rows explicitly and reran the focused file, including both late-added normal/null
cleared-bankruptcy timestamp cases. All 32 passed. No source filter was relaxed and
no migration changed. The unrelated in-flight migration bootstrap concern did not
cause a setup failure in this task's run.

All commands use `C:/atx/atx-db/.venv/Scripts/python.exe` from `C:/atx/atx-db`.
No full suite, live warehouse changes, network operations, secrets/auth reads, or external
transmission of user email. Only the evidence module, its test file, and this report are
owned by this fix. No shared registry, activation, jobs, delisting, migration, or progress
file was edited. No Critical finding was discovered; reviewed fixes are accepted on the
implementer's report under the program's speed ruling.

F5 remains informational and outside this fix: unresolved SEC identities can still be
excluded without a dropped-row diagnostic. Source rebuilds retain only current evidence;
historical runs must refresh with their requested cutoff rather than expect the fold to
reconstruct older classifications from a newer materialization.
