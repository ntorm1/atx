# DL1 selected-lineage review

One-pass static review of the DL1-owned diff and new files on `feat/tier1-parity`, against `derived-selected-lineage-brief.md` and the current `program.md` ruling. No code was changed and no Python, database, network, or test command was run by this reviewer. The implementer's eight focused fixtures passed under the 1.5 GiB guard (reported peak 0.682 GiB); root owns the affected regression run.

## Important

1. **Metric-reference source and fiscal-span fields are not verified against the referenced row.** `_derived_pit.py:157-166` stores a dependency's `source`, `fiscal_period_start`, and `fiscal_period_end` in each direct ref. In `derived_lineage.py:268-282`, qualification compares the ref's code, bucket, input hash, definition hash, and clock to the child, but never compares those three stored fields. `_METRIC_COLUMNS` at `derived_lineage.py:39-43` does not fetch the fiscal span, so the reader cannot currently check it. A ref whose `source` or fiscal span differs from the selected child can still receive `qualified` if its JSON hash is recomputed. This falls short of the brief's requirement to verify referenced row source and clock/span fields. Fetch and compare the child fields, and add a mismatch fixture. The child row's own canonical source and leaf CIK checks do still protect issuer qualification from this particular mismatch.

## Minor

1. **Focused tests omit an explicit lag and weighted-share branch assertion.** `test_derived_selected_lineage.py` covers TTM, annual fallback and coalesce, but no direct `lag(...)` selected-ref offset or weighted annual denominator selection. These are explicit proof cases in the brief and distinct lowering paths at `derived_dsl.py:603-614` and `_derived_pit.py:237-251`. Add compact fixtures when repairing the Important finding; no repeated broad run is warranted for this review.

## Checks with no finding

The publisher preserves the prior `inputs_hash` candidate-frame expression and deterministic ID expression; the selected-ref hash participates in compression, so a same-value source transition survives. The staged row byte/count check precedes scope deletion. The resolver fails closed for legacy NULL refs, derives a unique actual selected leaf CIK when no expected CIK is supplied, returns verified CIK only with `invalid` status for invalid numeric states, and rejects child clocks later than the parent event. Its SQL preflight caps aggregate payload bytes before batch materialization. These observations are static; runtime regression results remain root-owned.
