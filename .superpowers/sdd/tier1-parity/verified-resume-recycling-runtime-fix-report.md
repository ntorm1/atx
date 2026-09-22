# VR1 focused-runtime fixture repair

## Scope

The reviewed final patch was applied by root after archive7's terminal guard
stop. Under the guarded focused run, five selectors passed and only
`test_verified_resume_uses_independent_bounded_recycling_and_preserves_owned_rows`
failed at its stale `rows_loaded == 1` expectation. Its four close-state
assertions and `connection_reopens == 4` had already passed.

This repair changes only
`atx-db/tests/test_companyfacts_archive_repair.py`. Production
`atx-db/src/atx_db/fundamentals.py` was not changed.

## Corrected fixture oracles

The fixture deliberately removes receipt admission for CIKs 10, 20, …, 100.
Those ten issuers therefore replay through `_replace_facts`, alongside the new
loaded CIK 201; CIK 202 is an empty replacement.

| Oracle | Correct value | Reason |
| --- | ---: | --- |
| `rows_loaded` | 11 | Ten replayed loaded issuers plus new loaded CIK 201 each return one fact row. This is replacement output, not net-new retained rows. |
| `previously_completed_targets` / `resumed_loaded_targets` | 190 | The 200 committed precursor issuers less the ten deliberately removed receipts remain verified. |
| `replayed_targets` | 12 | Ten receipt-forced raw replays, CIK 201, and empty CIK 202. |
| `loaded_targets` | 201 | 190 verified loaded members plus 11 raw loaded replacements. |
| `completed_targets` | 202 | All 200 original targets plus raw CIK 201 and empty CIK 202 completed; the empty member counts as committed replacement work. |
| `empty_target_count` | 1 | Only CIK 202 has the unsupported/empty taxonomy payload. |

The independent cadence, configured-session, close-state, candidate count,
verified fact/point/receipt ownership, and rollback assertions are unchanged.

## Root verification command

```powershell
pytest atx-db/tests/test_companyfacts_archive_repair.py::test_verified_resume_uses_independent_bounded_recycling_and_preserves_owned_rows
```

No runtime command was run for this repair. Root owns the one affected-selector
rerun; this is an Important fixture-oracle correction and does not require a
broad re-review.
