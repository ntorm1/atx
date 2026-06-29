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

## Row Template

| Tranche | Status | Start SHA | End SHA | Domains | Verification | Live DB Notes | Caveats / Next |
|---|---|---|---|---|---|---|---|
| `Sx-name` | `in_progress` / `committed` / `blocked` | `<sha>` | `<sha>` | `<tables/modules>` | `<tests/smokes>` | `<row counts/migrations>` | `<known caveats and next step>` |
