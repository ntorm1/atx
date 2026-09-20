# CF5 Important findings: fix 1

Both Important findings in `companyfacts-archive3-repair-review.md` are addressed
in the actual CF5 files. No runtime, test collection/execution, Python imports,
database connections, probes, production commands or commits were run by this
implementer. Root remains the sole workload owner. No new independent review
was requested; root's accepted process calls for this report and focused root
execution unless a Critical finding emerges.

## Exact fix paths

- `C:/atx/atx-db/src/atx_db/_companyfacts_resume.py`
- `C:/atx/atx-db/src/atx_db/fundamentals.py`
- `C:/atx/atx-db/src/atx_db/activation.py` (CLI help text only in this fix)
- `C:/atx/atx-db/tests/test_companyfacts_archive_repair.py`
- `C:/atx/atx-db/tests/test_companyfacts_activation_resume.py`
- This report.

The root-authored environment isolation change in
`C:/atx/atx-db/tests/test_activation_stages_b.py` is preserved, with no additional
edits by this implementer. It remains an additional CF5 commit path.
No generic Dataset, warehouse helper, migration, registry, jobs or primary-key
changes were made. The original integration patch remains an audit of the first
activation integration; it must not be reapplied.

## I1: foreign/NULL-owned extra points

The SEC fact and point fingerprint scans now include all run owners. Shared-row
digests still include `run_id`, including NULL, so ownership remains part of the
evidence. Count and four SHA-256 limb sums are compared by security identity for
every security needed by a candidate skipped issuer. A point-only extra under
a foreign or NULL run now changes the retained multiplicity/fingerprint and
rejects the skip before any issuer/candidate mutation.

The fact query groups only by `(cik, security_id)`, returning min/max owner,
non-NULL owner count and min/max source URL along with the fingerprint. Those
bounded aggregates preserve the exact receipt CIK/owner/source checks without
adding buffers proportional to the number of run owners. The point query groups
only by security ID. All-owner fact counts still guard legacy spellings and
extra facts. Archive-owned points must still have NULL symbols; legitimately
paired foreign-owned legacy points may retain their legacy symbols.

There is one aggregate fingerprint scan of each SEC surface, not one scan per
issuer, and no full-history fact/point join or sort. Fetch batches remain 256;
SQL/Python state scales with issuer/security identities. The fixed ten-commit
reopen interval and the reopen between proof and issuer processing are unchanged.

Regressions now inject an exact filing-key duplicate point with a foreign owner
and with a NULL owner. Both must fail before mutation. A positive shared-security
fixture also proves that consistent foreign-owned fact/point pairs are included
and preserved instead of being rejected merely for having an outside owner.

## I2: resumable source-incomplete normal returns

When member errors remain, `load()` now writes a `data_quality_checks` failure
named `source_completeness` before returning. It records the actual dataset UUID,
positive failed-member count and the archive/allowlist fingerprints. This
append-only run observation survives later overwrites of per-member error
receipts.

Lineage still accepts terminal failed runs. A terminal `succeeded` run is now
accepted only when that exact UUID has a positive `source_completeness` failure
recorded inside its start/finish interval. Missing, malformed, zero-count,
wrong-run and out-of-window markers do not authorize a resume. Ordinary successful
runs and nonterminal runs remain ineligible. All existing archive, options,
receipt and retained-row proof checks continue to apply after eligibility.

This preserves the existing generic Dataset contract: a normal return is
ledgered as `succeeded`, while activation continues reporting source incompleteness
as failure. The durable per-run source outcome supplies the additional lineage
eligibility rather than rewriting generic dataset ledgers after the fact.

The mixed fixture now resumes twice more from each newest returned dataset UUID.
It verifies that the newly completed CIK 6 tail is skipped, the malformed member
is retried, empty/unavailable members retain their distinct behavior, and all
completed facts/owners stay unchanged. The additional iteration proves that an
ancestor's source-incomplete eligibility survives its error receipt being
overwritten. An activation regression also verifies the failed-stage/newest-UUID
path directly.

## Exact changed regression selectors

- `tests/test_companyfacts_archive_repair.py::test_contradictory_completion_proof_fails_before_issuer_mutation`
  adds `point_foreign_owner_extra` and `point_null_owner_extra` cases.
- `tests/test_companyfacts_archive_repair.py::test_shared_security_proof_accepts_matching_foreign_owner_pairs`
- `tests/test_companyfacts_archive_repair.py::test_verified_resume_skips_loaded_recovers_candidates_and_retries_observations`
  now covers repeated newest-UUID source-incomplete resumes.
- `tests/test_companyfacts_archive_repair.py::test_source_incomplete_success_requires_its_durable_positive_marker`
  covers five invalid-marker cases.
- `tests/test_companyfacts_archive_repair.py::test_ordinary_successful_run_does_not_authorize_resume`
- `tests/test_companyfacts_activation_resume.py::test_source_incomplete_activation_resumes_its_newest_dataset_uuid`

The two CF5-specific files now enumerate 52 parameterized cases by source
inspection, not runtime collection. Pending root command from `C:/atx/atx-db`,
under the existing sole workload slot and process-tree guard:

```powershell
$env:ATX_SEC_USER_AGENT = 'atx-db/0.2 atx-research@example.com'
python -m pytest -n 0 tests/test_companyfacts_archive_repair.py tests/test_companyfacts_activation_resume.py tests/test_activation_stages_b.py::test_download_stage_requires_a_sec_user_agent
```

The original receipt/candidate rollback, fixed-interval reopen, settings replay,
proof-phase reopen, missing-CIK, source digest and scope tests remain in that
focused command. Root may reuse its existing Ruff/strict-mypy invocation for the
modified sources; neither check was run by this implementer for this fix.

## Limitations and changed cost

- The all-owner scans may hash more SEC rows than the former lineage-only scans.
  Aggregate memory remains identity-bounded, but full-archive latency and peak
  memory still require root measurement under the existing budget.
- A contradictory pair elsewhere in a shared security's retained SEC group
  conservatively rejects that security's skip proof. Matching foreign-owned
  pairs are accepted. This trades some resume permissiveness for complete
  all-owner evidence without millions of filing-key buffers.
- The SHA-256 multiset collision assumption and the largest-single-issuer
  transaction limitation from the first report are unchanged.
- Old normally returned source-incomplete runs written before this marker was
  introduced remain ineligible if they lack the run-bound marker. Archive3's
  terminal failed UUID remains eligible through the existing failed-run branch.
  Marker deletion or corruption fails closed; no source completeness is inferred
  from a maximum CIK, mutable error receipt or successful execution status alone.
- This fix does not claim a complete source when malformed members remain.
  Those errors continue to be retried and activation continues to fail visibly.

This report supersedes the first handoff's lineage-only aggregate description
and failed-status-only eligibility description. The initial independent review
reported no Critical findings. No additional independent review was performed.
