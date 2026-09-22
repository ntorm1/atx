# Reported-quarter EPS source: Critical repair report

This is a draft-only 0320 repair. Production source, imports, tests, the database,
and SEC network were untouched while the guarded bulk loader was active. The
original `integration.patch` is preserved. `integration-final.patch` is the
standalone patch from the current live baseline.

## Critical repairs

1. The span-expanded grid binds a current-year leaf to its quarterly duration
   group, excluding the duplicate annual/YTD leaf. The row lineage treats
   numeric `- Basic` and `- Diluted` rows as siblings under the same per-share
   heading. A new adjusted per-share heading supersedes the older GAAP parent.
   The CVX-shaped fixture includes separate net-income and per-share headings,
   Basic, adjusted 1.52, annual 6.63, and prior-quarter 1.84 cells; the
   selected current quarter is 1.39.
2. The SEC 8-K `reportDate` is retained as an event date, not used as the
   earnings period end. The parser combines an explicit quarterly duration
   header's month/day with its own year leaf and selects the latest such end
   on or before filing. Fiscal-label selection uses a unique HTML title/heading
   or centered bold SEC results title; arbitrary prose `results` wording is
   not a title. A sole unanchored quarter label is accepted, while competing
   unanchored labels are rejected. It makes no calendar-year assumption about
   fiscal identity. A prior-year comparative can appear first in prose without
   relabeling the current EPS. The source fixture uses event date 2026-01-30
   and EX-99 quarter end 2025-12-31, and asserts the event date stays in raw
   provenance.
   A second fixture has comparative FY2025 prose and a current fiscal 2026
   first-quarter HTML heading, a 13-week end of 2025-04-27, and a 2025-05-05
   event date. It asserts fiscal `(2026, Q1)` and the exact 2025-01-27 start.
   An inverted comparative sentence with `FY2025 results` and `FY2026` but
   no qualified title is rejected.
3. Accepted receipt and fact writes share one outer transaction. A terminal
   accepted skip proves the fact's receipt id and document SHA; `fetch_failed`
   remains retryable. The injected fact-write failure fixture covers rollback
   and a successful cache-backed resume.
4. Week-based rows require an explicit 13/14-week duration, an exact headed
   end, and a document fiscal-quarter label. Their inclusive start is derived
   from the stated week count, not a calendar-quarter map. Fixtures cover a
   13-week pass, a 14-week 53-week-issuer pass, missing fiscal evidence, and
   mismatched 13/14-week headed ends.

## Governed source and supporting evidence

The patch includes migration 0320 and its registry entry, the raw SEC
acceptance-string column, bounded SEC source loader, source CLI, dataset/job,
activation stage, activation CLI, readiness stage list, and focused source and
bulk tests. The source stage runs after `submissions_load` and before statement
materialization; cache, history/scope, timeout, byte caps, and project User
Agent controls are ledgered. The source fact is CIK-owned evidence, not a
certified market-security or standardized EPS result.

The prior patch SHA-256 values
`2569830B872138B8DB240000F16A293BD0DC68C68D6BAA0B7A058832602C94E5`
and `30F1DE0D621CEA54FDFCE50940535C295D1590D63E2E799EFB46F22CAC435AB7`
and `49AE4E22C97D5824F0D105143147087401BCF8A0244B11B9A10D587991D46A7E`
are superseded. Static inspection of the locally retained official EX-99
Attachment 1 showed that each year leaf spans currency, value, and spacer
cells. The parser now recognizes one contiguous headed leaf group and selects
its one numeric EPS cell; the prior-year comparable comes from its separate
headed group. A compact generic fixture reproduces those spans and a
comparative-first fiscal phrase, asserting current 1.39, prior 1.84, and
rejection of annual 6.63 and adjusted 1.52 alternatives. No full exhibit
prose or issuer-specific parser rule was added.

Cache writes use temporary files, fsync, atomic replacement, SHA/byte-count
sidecars, and receipt-SHA checks on hits. The new fixture proves an incomplete
cache entry is refetched after a `fetch_failed` receipt. The bulk fixture
asserts raw offset strings, retained prefix rows, CIK order, archive SHA, and
scope metadata across a failed all-form prefix resume. These assertions are
present but have not been executed under the loader guard.

`integration-final.patch` has full `diff --git a/atx-db/... b/atx-db/...`
sections for every changed owned mirror. The unchanged `tests/test_press_release.py`
mirror needs no delta. `git apply --check --whitespace=error` passes against
the current live baseline. Its final SHA-256 is
`BAC6C8DE5808B5FCF2897E2AB3DADD2F962F1D808762309F07A787F900C0B170`.
Root must serialize the 0320 registry entry before
the separate 0321 bridge registry entry. The original `integration.patch` must
not be applied in addition to the final patch.

After the active loader is terminal, run the focused parser/source, bulk,
press-release, migration, and schema-contract tests once before Critical-only
rereview. No runtime pass is claimed here.

## Post-integration follow-up

Root applied the preceding full patch and reported a guarded focused run with
28 passes and three fixture failures. Two clock fixtures expected midnight on
2026-02-01; filing date 2026-01-30 plus 46 hours is 2026-01-31 22:00. The
simple two-year EPS fixture expected a prior-year comparative even though its
quarterly duration heading covered only the current column. The small
`source-fixture-followup.patch` corrects those fixture inputs and assertions;
it does not relax the source's prior-year header proof.

The same small follow-up repairs a production discovery defect found against
the locally frozen official SEC references. Accession directory `index.json`
reports MIME labels (`text.gif`), not SEC exhibit Type, and the official EX-99
filename need not begin with `ex99`. The loader now reads the accession's
`<accession>-index.html` Document Format Files table in its existing bounded
index request, selects only a row whose Type cell is EX-99, and validates the
linked flat filename inside the same accession directory. Tests cover a MIME
directory negative, typed non-prefix positive, wrong Type, wrong accession,
and multiple typed exhibits. The source also fixes the two narrow lint issues
reported after integration (unused header reason and temporary-file context).

Follow-up patch SHA-256:
`5B6332C1AF3EE7E5BA6115F64010B5DECBE89DF8035D707BEFC3430BC4663A23`.
It has two full `diff --git` sections against the integrated live files and
passes `git apply --check --whitespace=error`. It has not been applied or run
by this draft implementer.
