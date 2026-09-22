# VR1 static draft report

## Scope and artifacts

This is a static draft only. The live runtime-owned files were not edited. The
two complete draft copies and the narrow integration patch are:

- `atx-db/src/atx_db/fundamentals.py`
- `atx-db/tests/test_companyfacts_archive_repair.py`
- `integration.patch`

The patch changes only `SecCompanyFactsDataset.load` recycling cadence and its
existing focused archive-repair test file. It does not touch registry, jobs,
activation, migrations, schema, source selection, archive verification, or
receipt logic.

## Baseline and artifact hashes

| Artifact | SHA-256 |
| --- | --- |
| Live baseline `atx-db/src/atx_db/fundamentals.py` | `A73F51625BF8933744256D8BA1A7BB4A2C67BAD33F2D86DBC5BD932BAA8CC868` |
| Live baseline `atx-db/tests/test_companyfacts_archive_repair.py` | `6DFB5880253584F323DF2F78462DD24F20F6B6EF895856D082A6C1CDBECA74FE` |
| Draft `atx-db/src/atx_db/fundamentals.py` | `31989CC785D403EDB9B828680EB1EA00C942C05D499680D25403EA8C613AA2D0` |
| Draft `atx-db/tests/test_companyfacts_archive_repair.py` | `329E24CC9E73E7CE03BE6B74CEAD5EC620EBD78379F266691CE99B2BEAE2A1BE` |
| `integration.patch` | `E998A2927AA5FD45F8E36B5E73A1D3B694101468773C9DDD90EE25F706DED80F` |

## Implementation and invariants

`_COMPANYFACTS_REOPEN_TARGETS` remains the raw fact/point replacement cap of
10. A new private `_COMPANYFACTS_VERIFIED_REOPEN_TARGETS` caps only committed
verified-member candidate reconciliations at 100. At the top of each target
iteration, either reached cap calls the existing `reopen_companyfacts_store`;
the successful reopen then resets both counters because both kinds of work use
the same connection state.

The verified counter advances only after its existing per-issuer candidate
transaction succeeds. The raw counter advances only after the existing
`_replace_facts` transaction succeeds, so empty replacements still count and
candidate/receipt/fact/point rollback failures do not count. Unavailable
placeholders and source failures still bypass both counters. The explicit
post-proof reopen is unchanged. The final reopen now occurs when either
counter has partial committed work.

No mutation path was removed or batched: verified members still reconcile
candidates issuer by issuer, raw and empty paths still call `_replace_facts`,
and the archive identity checks, receipt admission and lineage, multiset proof,
source scope, row ownership, clocks, session protections, and temporary
relation protections are untouched.

## Focused draft evidence

`test_verified_resume_uses_independent_bounded_recycling_and_preserves_owned_rows`
uses the existing offline archive and real resume proof. Its failed precursor
commits issuers 1–200, then fails on 201. It removes only receipts for raw
replays 10, 20, …, 100 and clears candidates as the existing recovery fixture
does. The resumed sequence has 190 verified members, ten raw replacements, a
new raw fact at 201, and an empty replacement at 202. It observes closes as:

`[(200, 0), (200, 100), (200, 200), (201, 201)]`.

Those states establish, in order: the preserved post-proof reopen; the ten raw
replay cap; reset of the verified counter when that raw-triggered reopen occurs;
the 100-member verified cap; and the final partial reopen after raw and empty
work. The test also verifies 201 recovered candidates and byte-for-byte
preservation of selected verified facts, points, receipt rows, and original
owners.

`test_verified_candidate_reconciliation_failure_rolls_back_without_counting_work`
injects an error after a real verified candidate insert. It asserts the data
snapshot is unchanged, no temporary relations remain, and the run fails. The
pre-existing raw cap/session/temp test and post-proof candidate-recovery test
remain in place.

## Root integration and verification

From `C:/atx`, first review the current live hashes above. Then run a patch
check and apply only when archive ownership permits:

```powershell
git apply --check .superpowers/sdd/tier1-parity/verified-resume-recycling-draft/integration.patch
git apply .superpowers/sdd/tier1-parity/verified-resume-recycling-draft/integration.patch
```

Run the focused selectors after integration:

```powershell
pytest atx-db/tests/test_companyfacts_archive_repair.py::test_verified_resume_uses_independent_bounded_recycling_and_preserves_owned_rows atx-db/tests/test_companyfacts_archive_repair.py::test_verified_candidate_reconciliation_failure_rolls_back_without_counting_work atx-db/tests/test_companyfacts_archive_repair.py::test_reopen_after_ten_commits_preserves_caps_and_leaves_no_relations atx-db/tests/test_companyfacts_archive_repair.py::test_resume_reopens_after_proof_before_candidate_recovery
```

No Python, import, test, lint, database, profiler, or live performance/memory
measurement was run while preparing this draft. It makes no wall-time or
capacity claim.
