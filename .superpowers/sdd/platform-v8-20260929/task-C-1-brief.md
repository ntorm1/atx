# Brief: task C-1

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task C-1: fit and card stores

**Files:** `atx-impl/tools/fit_composition_weights.py:184-192,242,926,1061`, `atx-impl/tools/alpha_report_card.py`, tests

**Interfaces:**
- Store root `build-equity/fit-work/<role_sha16>-<window_id>/`; record key = (signal payload SHA, producer fingerprint).
- `producer_fingerprint(funcs: tuple) -> str`: SHA-256 of the normalised AST of `factor_record`, `Context`, `PricePanel`,
  `neutralization_basis`, `centered_tied_ranks`, as `prepare_research_fields.py:2748-2780` does for field groups.
- Themes are read from the registry (A-1) when present, else from the in-file list.

- [ ] **Step 1:** tests `test_fit_store_reuses_across_libraries` (v7.0 then v7.1 prints `computed 4, reused 44`),
  `test_comment_edit_keeps_store`, `test_fit_store_keyed_by_role_and_window`, `test_card_invariant_block_reused`,
  `test_card_low_coverage_reported`.
- [ ] **Step 2:** implement the store key, the fingerprint and the card's invariant block (IC by year, decay curve, size and
  FF12 splits, coverage, book PnL); only the correlation block is recomputed.
- [ ] **Step 3:** root: admission.json byte-identical to `mega-weights-v71-ew`; cards byte-identical to `mega-cards-v71`.

**Target:** fit 21.9 -> about 8 s; card 19.5 -> about 5 s (est.).

