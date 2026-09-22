# Reported-quarter EPS source: Critical-only static rereview

**Disposition: reject the current completion artifact.** This review inspected
`reported-quarter-eps-draft/integration-final.patch` and its owned source and
test mirrors against `reported-eps-source-review.md`. It did not apply either
patch, import code, run tests, access a database or network, or change live
source. Line numbers below refer to the draft mirrors unless marked as patch
lines.

## Remaining Critical

1. **The Chevron-shaped quarterly diluted row still cannot be extracted.**
   `_label_lineage` clears all inherited labels when the preceding row has a
   numeric cell (`src/atx_db/press_release.py:516-519`). The new structural
   fixture has a `Net income ... per share:` parent, then numeric `- Basic`,
   then numeric `- Diluted` (`tests/test_reported_quarter_eps_source.py:110-115`).
   At the diluted row, clearing on Basic leaves only `- Diluted`; the required
   `per share` check at `press_release.py:605-608` fails. The fixture's asserted
   1.39 at test lines 119-124 therefore contradicts the implementation.
   The original production Chevron shape likewise has a numeric Basic sibling
   before `- Diluted`. Preserve the applicable section/parent label across
   numeric siblings while keeping Basic, adjusted, continuing and annual/YTD
   siblings outside the selected diluted lineage. Statically trace the revised
   row hierarchy through both the fixture and actual EX-99 structure, then run
   the focused parser test once the guarded loader is terminal.

2. **The advertised standalone patch does not install the 0320 migration.**
   `integration-final.patch` has six `diff --git` sections (patch lines 1, 938,
   1045, 1113, 1219, 1234), but no `migrations/registry.py` delta. The owned
   registry mirror registers `bodies_0320` at lines 146 and 290, while the
   patched candidate query requires its `acceptance_datetime_raw` column
   (`src/atx_db/sec_submissions.py:79`) and the loader requires its receipt
   table (`src/atx_db/press_release.py:790-809`). Applying this patch alone to
   the stated baseline leaves migration 0320 unapplied; the source path cannot
   run. Assemble the standalone patch with the registry delta and other claimed
   owned files, then repeat the static apply check against that final artifact.
   Its missing test, source CLI and readiness deltas also make the fix report's
   "complete owned mirrors" claim inaccurate; those omissions should be
   reconciled before integration.

## Original Critical dispositions

- **Chevron table and row hierarchy: open.** The span-expanded grid retains
  `rowspan`/`colspan` (`press_release.py:246-336`) and the header selector binds
  the current-year leaf to its quarterly parent and excludes `Year Ended`
  (`462-498`). The inherited-label reset above still prevents the target
  diluted fact. The added fixture has the relevant duplicate quarter/annual
  2025 leaves and adjusted/basic siblings, but its acceptance assertion is
  currently unsupported.
- **Atomic accepted receipt and fact: statically closed.** Accepted insertion
  and fact insertion share one outer transaction (`press_release.py:994-1006`);
  the writer can avoid nesting (`1473-1502`). The terminal query requires an
  accepted receipt's id and document SHA on a matching fact (`790-811`), and
  excludes `fetch_failed` from terminal outcomes (`793-799`). The injected
  fact-write-failure and resume fixture covers rollback and retry at test lines
  160-199, subject to deferred execution. This disposition does not assert a
  runtime pass.
- **13/14-week boundary qualification: statically closed, with coverage gap.**
  The header must provide 13/14 weeks and the report-date end (`435-459`,
  `462-498`); a document fiscal-quarter label is required (`523-565`,
  `922-929`). The source records the exact inclusive start, week count, duration
  and fiscal evidence (`539-547`, `981-986`) without calendar-quarter mapping.
  A 13-week positive and 14-week missing-fiscal negative fixture exist at test
  lines 128-157. The requested 14-week positive and 53-week issuer fixtures
  are still absent, so the repair report should not claim those are verified.

The cache and bulk metadata additions address the earlier Important findings
in source, but the final patch omits their tests. Focused runtime verification
remains deferred under the active loader guard.
