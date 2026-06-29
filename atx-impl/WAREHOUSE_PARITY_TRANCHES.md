# Warehouse Parity Tranche Ledger

Append one row per coherent tranche. Keep this file boring and factual: what changed, which commit landed it, how it was verified, and what the next agent should do next.

## Rules

- Append-only by default; only edit prior rows to correct factual errors.
- Include full 40-character commit SHAs when available.
- Record test commands and live DB smoke checks, even when they expose caveats.
- Link the tranche to the current north-star goal in `WAREHOUSE_PARITY_NEXT_AGENT_README.md`.
- Keep implementation notes short; deeper details belong in code, tests, `PARITY_GAP.md`, or `.superpowers/sdd/progress.md`.

## Ledger

| Tranche | Status | Start SHA | End SHA | Domains | Verification | Live DB Notes | Caveats / Next |
|---|---|---|---|---|---|---|---|
| S3-S5b warehouse parity spine | committed | pre-existing branch worktree | `9784a218293a1d55678afd5e7e4ec4e524f10290` | S3 insider/blockholder ownership; S4 fundamentals depth, overlays, four-date period model, XBRL validation; S5a share-count history; S5b corporate-action type dim and adjustment-factor history | `python -m pytest atx-impl\db\tests -q` passed before commit; live CLI smoke for shares and adjustment factors ran | Default DuckDB migrated through `0011`; live counts: `shares_outstanding_history=342`, `corporate_actions=142`, `corp_action_type_dim=5`, `adjustment_factor_history=142` | Fix S5b split-artifact classification before daily cumulative factors. KO smoke showed a `0.5` factor currently classified as `CASH_DIV`; add heuristic/quarantine plus regression test. |
| S5b split-artifact classification fix | committed | `e59fadb9f9b0950b788251d034b5ed5c29f3bb7a` | `c2d22f949f6bf1f505b585da3e91509205afdb40` | `adjustment_factor_history`, `corp_action_type_dim`, as-of adjustment-factor surface, quality checks, migration `0012` | `python -m pytest atx-impl\db\tests\test_adjustment_factors.py atx-impl\db\tests\test_migrations.py -q` passed; `python -m pytest atx-impl\db\tests -q` passed before the final column-order polish; focused tests reran after polish | Default DuckDB migrated through `0012`; `adjustment_factor_history` refreshed 142 rows with run `dc93f804-b4da-422a-a273-09cd3dd2399d`; KO `2012-08-13` now classifies as `SPLIT` with `factor_price=0.5`, `factor_shares=2.0`, `classification_reason=split_like_inferred_cash_artifact`; live counts by event type are `CASH_DIV=140`, `SPLIT=2`; watermarks refreshed and focused adjustment-factor quality checks passed | Daily cumulative factor materialization is now unblocked. Next tranche should build split-only price/share factors and total-return dividend factors while keeping raw close, split-adjusted close, and total-return adjusted close separate; delisting proxy work follows after that. |

## Row Template

| Tranche | Status | Start SHA | End SHA | Domains | Verification | Live DB Notes | Caveats / Next |
|---|---|---|---|---|---|---|---|
| `Sx-name` | `in_progress` / `committed` / `blocked` | `<sha>` | `<sha>` | `<tables/modules>` | `<tests/smokes>` | `<row counts/migrations>` | `<known caveats and next step>` |
