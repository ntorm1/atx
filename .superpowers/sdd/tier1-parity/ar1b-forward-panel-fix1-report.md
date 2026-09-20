# AR1b review repair: Important F1

Addresses the single Important finding in `ar1b-forward-panel-review.md`
(review commit9e602bc5). Implementation is confined to the forward section of
`delisting.py`, `quality/checks_survivorship.py` and issue-specific regressions.
No activation/jobs/registry/migration changes or live warehouse operations.

The observation cutoff is applied before terminal revision ranking. The latest
eligible revision and first event are then selected without excluding invalid
numerical values. Validity is retained as a flag, not an input filter. A selected
nonfinite terminal return or value below-1 therefore remains an event boundary:
it cannot revive an older valid revision, select a later valid event instead, or
permit formation/endpoint prices after the terminal to produce survivor labels.
Valid preterminal windows remain publishable. Historical cutoffs can still select
an older revision when the invalid correction was not yet available.

The existing registered critical quality check now adds one failure for each
selected invalid terminal event, independently of output/formation rows, then
counts missing stitches for valid selected terminals as before. This prevents an
empty panel, missing formation data or existing valid-looking output from masking
the invalid input. Its observed value is now invalid selected terminal inputs
plus missing valid-terminal stitches. Price revision/validity ordering is unchanged.

Seven focused regression cases cover below-100%, infinite andNaN corrections;
invalid-only evidence; historical cutoff selection; stale output masking; later
event fallback; post-terminal survivor prevention; preserved valid preterminal
returns; and selected invalid evidence with no formation bars.

Static validation: AST parsing passed and Ruff reports no new findings against
HEAD for the three touched files (new test additions remain clean). `git diff
--check` passed. All **seven issue-specific runtime cases passed**, one serial
run under the2GiB Windows job cap; the minimal fixtures use128MB DuckDB/one thread.
The guard reported native peak job memory0.603GiB and no pressure stop. This native
metric is not RSS. Receipt: `ar1b-fix1-focused-memory.json`.

```powershell
.\.venv\Scripts\python.exe ..\.superpowers\sdd\tier1-parity\run_memory_guarded.py --job-gb 2 --receipt ..\.superpowers\sdd\tier1-parity\ar1b-fix1-focused-memory.json -- .\.venv\Scripts\python.exe -m pytest -n 0 -q tests/test_survivorship_forward_sql.py -k 'invalid_terminal_correction or invalid_only_terminal or invalid_selected_terminal'
```

The source audit had finished before the root granted the DB slot. The slot was
released immediately after the successful4.75-second run, for activation
integration. No earlier tests/full suite/scale benchmark were rerun. F1 is fixed
with the requested issue-specific evidence; the process ruling accepts Important
repairs on this report without another review pass.
