# SEC submissions verified resume implementation report

Implemented in `sec_submissions.py`, `activation.py`, and the new focused
`test_sec_submissions_resume.py`. No migration, registry change, dependency,
memory escalation, network activity, or production write was performed by the
implementer.

`--submissions-resume-from-run-id DATASET_UUID` explicitly selects a failed
bulk attempt. The loader verifies compatible all-form/all-CIK/history/batch/path
parameters, terminal ordered ancestry, a matching archive SHA256 receipt from
before the first attempt, and complete nonempty-CIK batch boundaries. It then
compares every sorted archive member through the candidate boundary, including
empty main members and historical filings, against exact retained accession,
security ID, stripped form, and source keys. Counts/max CIK alone never authorize
skipping. The remaining archive retains the full main-member denominator,
existing progress, and bounded connection lifecycle. Current attempt rows written
remain separate from verified prior rows and total covered rows.

The verification query fetches 4,096 rows at a time; Python holds only that
batch plus one CIK's source keys beyond the loader's existing ZIP directory
structures. The database sort uses the caller's existing memory/spill settings.
Verification adds archive hashing, sorting, and prefix JSON parsing. Its live
cost is unmeasured; no performance advantage over replay is claimed yet.

## Independent review and repairs

`submissions-verified-resume-review.md` found no Critical issues and two Important
issues. Both were repaired without expanding source scope:

- I1: full `scope_complete` now additionally requires unrestricted forms and
  enabled history. Form-filtered, empty-filter, recent-only and restricted-CIK
  calls preserve their useful results but publish `scope_complete=false`, also
  in the saved source metadata.
- I2: direct activation and shared CLI entrypoints validate the selected plan
  before database opening or governed-migration inspection. A resume UUID plus
  `sec_bulk_download` is refused with explicit `--only submissions_load` /
  `--start-stage submissions_load` guidance. This prevents cache-hit receipt
  replacement from destroying the pre-attempt evidence. It does not relabel old
  receipts, skip requested refreshes, or change downloader semantics.

Focused regressions verify both refusal entrypoints, untouched receipts and
successful later resume using the same evidence, acceptance of the two offline
stage selectors, and four incomplete-scope cases. Existing changed-archive
refusal remains in place. Per the agreed one-pass process, Important repairs are
accepted on this report; no Critical rereview was requested.

## Verification

Root executed all focused runtime checks (54 cases total):

- Resume tests: 25 cases; existing bulk tests: 10 cases. All 35 passed, exit 0;
  measured process peak 0.674938 GiB.
- Production activation tests: 11 passed, exit 0; peak 0.678215 GiB.
- Review-repair regressions: all 8 passed, exit 0; peak 0.651371 GiB.
- Implementer static Ruff on all three touched files passed, including after
  the review repairs. This was explicitly authorized by root.

The three test sets have committed log/memory receipts named
`submissions-resume-tests`, `submissions-resume-activation-tests`, and
`submissions-resume-fix-tests` in this directory. Root accepted both Important
repairs on the concrete report and regression results; no rereview is required.

No implementer imports, tests, live database queries, probes, or jobs were run.
Root authorized the exact-path implementation/evidence commit after verification.
Production resume verification still requires a separately guarded live run.
