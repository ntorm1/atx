# Task A-1 report: alpha registry and one generator (lane A, pool-11)

Branch `feat/platform-v8-a-20260929`, on top of A-3 (`fa61c67f`). Python and JSON only; nothing built or run on data.

## What was built

- `atx-impl/strategies/alphas/registry.json` (`atx.alpha-registry/v1`, 83 KB), seeded from `fund_industry_ic_v71.json`
  and its recipe: `alphas[]` (48 entries, `origin: "prior"`, keys exactly `{id, dsl, theme, tier, prior_sign, citation,
  prior_sign_source, form, origin, notes{formula, domain, deviation}, added_in}`); `fields{name: {formula_id, origin,
  producer, clock, basis}}` (the 43 v7.1 declarations; `basis` is needed to write the library byte for byte, `origin
  "base"` marks the role fields); `themes{name: text}` (the 10 v7.1 families, in library order); `tier_scores` with the
  brief's values verbatim (order = tier rank 1..6); `house_budget {max_extra_fields 5, max_slots 7, max_prior_bars 314,
  max_dsl_bytes 4096, max_roster 56}`; `candidate_defaults {horizons [5, 21, 63], sign_policy train-rank-ic21}`.
  `added_in` = the first legacy library holding the exact DSL (v4, v6, v61, v70, v71). `prior_sign_source`, `form`
  and `notes` are the v1 recipe's lineage and template text (legacy nulls kept as null).
- `atx-impl/strategies/libraries/v71.json`: `{id fund_industry_ic_v71, parent "v70", members (48, roster order),
  budget_exceptions [q5_eg max_extra_fields 6, qmj_safety max_slots 8, with their recorded bases], prereg (the v1
  recipe's preregistration text)}`.
- `atx-impl/strategies/generate_library.py --library NAME [--check] [--plan-json PATH]`: writes `<id>.json`
  (`atx.dsl-ic-library/v1`, the legacy encoder) and `<id>.recipe.v2.json` (`atx.dsl-ic-experiment/v2`: library pin,
  parent pin, registry ref, prereg, generation {new_members, house_budget, budget_exceptions}, lineage rows {id,
  roster_order, theme, tier, tier_rank, prior_sign, prior_sign_source, form, origin, added_in, dsl_sha256, fields},
  trials {new, unchanged, admission_trials, dsr_n_rule}, static_validation {source: K1}). Importable API used by A-2:
  `load_registry`, `validate_registry`, `load_library`, `build_library`, `build_recipe`, `documents`, `plan_rows`,
  `validate_plan`, `exe_plan`, `referenced_fields`, `extra_fields`, `encode`, `encode_data`.
- `atx-impl/strategies/fund_industry_ic_v71.recipe.v2.json` (25 KB, sha256 `69e95298...3017107`): the slim recipe of v7.1.
- `atx-impl/strategies/alphas/fixtures/v71_plan_k1.json`: the recorded K1 plan (hand-written from the contract; rows from
  the v1 recipe's lineage and per-candidate figures, placeholders 4 slots / 12 nodes for the 39 members whose recipe
  carries only maxima; flagged in its `_fixture` key).
- `atx-impl/strategies/test_generate_library.py` (9 tests).

Static validation (K1): `validate_plan` requires a row per member with `dsl_sha256 = sha256(dsl)`, `len(extra_fields)`
<= 5, `num_slots` <= 7, `required_lookback` <= 314 (or the member's recorded exception), extra fields that are registry
fields, and the exe's extra fields equal to the registry token match; rows of other ids and a plan made for another
library SHA are problems too. No Python parser: the library's field declarations are the base fields plus the registry
fields whose names occur as identifier tokens in the members' DSL, and that match is checked against the exe's rows.
Writing requires `--plan-json`; `--check` works without it (byte compare) and validates when it is given.
Generators v4..v71 and their outputs are untouched (frozen legacy; their tests still pass).

## How root verifies

```bash
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/strategies/test_generate_library.py
"C:/Program Files/Python312/python.exe" atx-impl/strategies/generate_library.py --library v71 --check
```

Results: 9 passed; all of `atx-impl/strategies` 162 passed (legacy generators included). `--check` prints
`fund_industry_ic_v71.json 787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259 39317 bytes`.

- `test_v71_library_byte_identical`: the generated library has SHA 787c802e... and equals the committed file; the slim
  recipe equals its committed file; `--check` exits 0 with and without the plan.
- `test_plan_rows_equal_static_validation`: the 48 plan rows equal the v1 recipe's DSL SHA, prior bars and extra fields
  of every member, the slots and nodes of the 10 members whose figures it records, and the library maxima (8 slots,
  37 nodes, 272 prior bars); the library validates against them. **With the real exe after B-3**: save the plan
  (`build-equity/bin/atx-equity-strategy-ic.exe --plan-only --library atx-impl/strategies/fund_industry_ic_v71.json
  --library-sha256 787c802e... --train <lo1 role manifest> --train-sha256 <pin> --train-fields <fields-v9 dir>
  --train-fields-sha256 8fd00e9f...`) and run the test with `ATX_V71_PLAN_JSON=<that file>`. Any difference between the
  C++ counts and the old Python estimates (slots, nodes) then shows by member.
- `test_unknown_theme_refused`, `test_origin_required` (K5: missing, `guess`, null, `Prior` refused; prior, grid,
  mined accepted).
- Also: `test_registry_seed_is_the_v71_library`, `test_slim_recipe_carries_what_the_fitter_and_ledger_read`,
  `test_fitter_reads_the_same_priors_from_the_slim_recipe` (acceptance (c) offline: `fit_composition_weights.
  load_priors` gives identical themes, tiers, tier ranks, prior signs and source from the v1 and the slim recipe; only
  `recipe_sha256` differs), `test_plan_problems_name_each_member`, `test_writing_needs_a_plan_and_a_new_library_round_trips`.

Brief step 4 (root): the fitter on the slim recipe: replace `--recipe .../fund_industry_ic_v71.recipe.json
--recipe-sha256 7f8a2643...` with `--recipe atx-impl/strategies/fund_industry_ic_v71.recipe.v2.json --recipe-sha256
69e95298470d1e4fc38b7b4e6d35809740b5a1479f6c5be1c34a8daac3017107` in the v7.1 fit command; admission.json must equal
`mega-weights-v71-ew/admission.json` once the recipe pin is dropped.

## Deviations from the brief (with reasons)

1. Registry `fields` entries also carry `basis` (the library's field declaration text, needed for byte identity), and
   the registry adds `house_budget` and `candidate_defaults` (data that the v4..v71 generators held as constants).
2. The slim recipe is a new file (`fund_industry_ic_v71.recipe.v2.json`); the 156 KB v1 recipe stays as the legacy
   file the v71 spec pins. The generator reproduces the v71 library (the IC runner format) byte for byte.
3. A parent without `libraries/<parent>.json` is a legacy library `fund_industry_ic_<parent>.json`: v7.1's parent v7.0
   is not re-derived (its theme texts differ from the registry's current ones), only pinned by SHA in the recipe.
4. The slim recipe records no generator SHA (a generator edit would otherwise invalidate every recipe pin) and no plan
   rows (they depend on the exe build; validation is a gate at generation time).

## Cross-lane edits

None. For lane C (C-1 "themes read from the registry"): `generate_library.load_registry(HERE)["themes"]` is the theme
table; for lane V (K5): every slim-recipe lineage row carries `origin`.

## Open risks

- The house budget's `max_slots` 7 was a Python estimate (`estimated_peak_slots`); if the exe's `num_slots` counts
  differently, v7.1 members may fail the budget under the real plan. The real-plan test run above shows it; the fix
  would be a registry `house_budget` change (data), not code.
- The token match declares a registry field whenever its name appears as an identifier in a DSL. A registry field named
  like an operator would be declared spuriously; the K1 cross-check catches it.
