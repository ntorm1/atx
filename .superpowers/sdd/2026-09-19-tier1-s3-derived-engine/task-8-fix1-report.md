# S3 T8 Important-review fixes

Addresses I1 and I2 in `task-8-review.md`. No core metric definition, source price loader, parent refresher, migration, registry or production integration changes are included.

## I1: same-day EV filing selection

The shared compatibility EV adapter now uses the genuine baseline's precedence: prefer complete filings available at trade-date midnight, then descending fiscal period, availability and component identity. It does not prefilter candidates at the market-cap clock. The yield grid applies the original market-cap visibility condition after the denominator winner is selected. This also preserves the less obvious case where all filings arrive during the rebalance day: a newer after-cutoff winner must be rejected rather than silently replaced by an older same-day filing.

The compact boundary evidence executes the genuine `valuation_multiples`, `enterprise_value` and `enterprise_yield` module bytes obtained directly from Git commit `5b11a272c90e9d51cf6fcad3cb3ac0ed6493f9c3`. The frozen JSON records exact SHA256 hashes of those bytes, the common source-input digest, complete factor output keys, selected component lineage and output digest. It does not regenerate expected values with compatibility code. Storage and shared cross-sectional infrastructure are the existing repository implementation; the three selector/formula implementations under comparison are the exact baseline Git blobs.

## I2: parent visibility before ranking

The general parent grid now filters both the publication timestamp and the declared decision timestamp at 22:00 before ranking. For the gross-profit and cash-flow enterprise-yield variants, a declarative flag defers parent ranking until the parent/EV combinations have survived the stricter market-cap visibility join. Eligible candidates retain deterministic availability, source-load and factor-ID ordering. A later invisible source can no longer remove an earlier eligible observation.

The generic parent regression now uses competing 21:00 and 23:00 observations for the same security/date and verifies the selected earlier parent ID, plus an all-ineligible security. The compact EV fixture supplies parent observations at 20:00, 21:30 and 23:00 against a 21:00 market clock, and a security with only the two ineligible observations. Both lineaged EV variants must retain the 20:00 parent and record its exact identity.

## Evidence domain and reproduction

The boundary fixture contains 24 securities on 2022-03-31 and 882 common input records: 596 statement facts, 72 TTM records, 24 share observations, 24 bars, 24 membership windows and 142 competing parent observations. TTM values are sums of four distinct quarterly statements; annual gross profit also sums those quarters. Parent observations are explicit input fixtures referencing those actual statement/TTM IDs, allowing different publication sources and clocks to compete without altering the underlying financial facts.

The genuine baseline emitted 90 unique factor/security/date rows across all four EV factor IDs and 24 denominator rows. The regression compares complete keys, raw values and standardized values, and separately compares all selected denominator component identities and values. It checks both all-ineligible exclusions and confirms production compatibility does not materialize the deprecated EV table.

To regenerate the compact artifact explicitly, from `atx-db`:

```text
.venv/Scripts/python.exe -c "from tests.test_derived_compatibility import _freeze_ev_boundary_evidence; _freeze_ev_boundary_evidence()"
```

Normal pytest execution only reads the frozen artifact and does not require removed source files or Git history. Regeneration and the boundary test use one DuckDB thread and a 256 MB limit. Controller coordination serializes all DB work with other agents.

## Validation

The explicit baseline freeze completed successfully on its first execution. Then the following package passed **3/3**, on its first execution, in about four seconds:

```text
.venv/Scripts/python.exe -m pytest tests/test_derived_compatibility.py -n 0 -q -k "ev_boundary_selection or legacy_loader_rejects_parent or compatibility_selectors_bind" --junitxml=.pytest_cache/s3-t8-fix1-boundaries.xml
```

This verifies the 90-row baseline comparison, the strengthened same-key parent-cutoff regression, and SQL binding for all 23 compatibility expressions. A final assertion audit found that `pytest.approx(abs=...)` retains its default relative tolerance; both numeric assertions were tightened to explicit 1e-12 relative and absolute tolerances. With the controller's renewed serial slot, only that single boundary case was rerun: **1 passed** in about three seconds, recorded in `.pytest_cache/s3-t8-fix1-strict-numeric.xml`. No old broad fixture or full suite was rerun. Ruff passed all three touched Python files; strict mypy passed both touched source files. The shared DB slot was released after each focused package.

Frozen input digest: `39ac52c50f03f83431fc73f46651d998e42e08479f0f454952178946f0b32a35`. Frozen output digest: `a7d666bfe5e939d165379b00056949207f618487e752712feb7e3256d004d78b`. Exact executed baseline SHA256 hashes:

| Git blob | SHA256 |
| --- | --- |
| `enterprise_value.py` | `cf0d7983e6871505e310344f3956cf771d320ce851d53f17f1fcf7aff02da0cb` |
| `enterprise_yield.py` | `42c7b237d5fbf66e901bfe86f52043b61603e5b6596906a5d127b7e829c14b1f` |
| `valuation_multiples.py` | `ae89e58c755fec6f555ebb6bbc69b007fd95f2679b686d04ab53431a4b2a2dce` |

Both Important findings are fixed. Per the controller's ruling, this issue-by-issue evidence closes the findings without independent re-review.
