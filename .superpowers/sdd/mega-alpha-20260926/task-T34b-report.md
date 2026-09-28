# T34b report: library v5.1 = frozen v4 + `opex_at`

Status: **DONE**. Implementer (Opus 5.5), worktree `C:/atx-wt/pool-8`, branch `feat/mega-alpha-v5-lib51-20260927`,
base c8192c46. Pure Python: nothing was built, no IC runner was run, and no real data was read.

## Commits

- `6bfd9858` feat(strategies): library v5.1 = frozen v4 + opex_at (T34b). It adds `generate_fund_ic_v5.py`,
  `fund_industry_ic_v5.json`, `fund_industry_ic_v5.recipe.json` and `test_generate_fund_ic_v5.py`.
- (next) docs(mega-alpha): T34b report. It adds this file with `git add -f`.

## Artifacts and SHA-256

| file | SHA-256 | bytes |
|---|---|---|
| `atx-impl/strategies/fund_industry_ic_v5.json` (library, the IC runner input) | **`9e5ea08cb3c9a802f72e23dd069499b59294fba5708e0873d9dfc57f8a7458e0`** | 28,912 |
| `atx-impl/strategies/fund_industry_ic_v5.recipe.json` (lineage and provenance; optional `--recipe` for the fitter) | `a26670b0f7b6d1681a4ea8a4758da83841163ceaea8af5a4256ee718ad7c3aea` | 90,828 |

- The committed blobs hash to the same values; both JSON files are `eol=lf`.
- The library bytes do not depend on the generator source.
- The recipe embeds the LF-normalized generator SHA-256, so it changes only if the generator changes.

## Design

`generate_fund_ic_v5.py` follows the v4.2 pattern (`generate_fund_ic_v42.py:66-84, 96-160, 179-291`):

1. **Frozen v4 copy.** `_pinned_v4()` loads an untouched copy of `generate_fund_ic_v4.py` and runs its `documents()`.
   It then asserts:
   - library SHA-256 `daa9663e…` and recipe SHA-256 `62b510f1…`, the same pins as v4.2;
   - each committed v4 file equals the generator output.

   Every v4 row is re-validated with the v4 validator: `dsl_sha256`, `native_prior_bars` and `fields` must match
   the lineage.
2. **Validator.** It is the pinned v4 module, **unmodified**. `opex_at` uses only decay_linear, group_rank, log and
   `+ - * /`, which are all v4 registry rows, so v5.1 adds no registry row.
   - `registry_crosscheck` is v4's.
   - Result: `registry cross-check ok (14 operators vs registry.cpp/typecheck); grp_ group typing checked`.
3. **One Spec (ruling R-a, verbatim).**
   - `Spec('opex_at', 'profitability_quality', 'B', positive(div(sub(sale_ttm, oi_ttm), at), at), 'within_industry_grp_ff12', …)`
   - raw_prior_direction +1, smoothing 21, formula `(sale_ttm - oi_ttm) / at`.
   - domain `non-positive total assets -> NaN (house guard)`, the same string as gpa and roa.
   - The deviation text discloses: COGS + SG&A (incl. R&D) + D&A + other operating items vs. the paper's COGS + XSGA;
     no opex_ttm, xsga_ttm or cogs_ttm in fields-v6; OperatingIncomeLoss is single-concept; FF12 Money revenue
     includes interest.
   - Citation (verbatim R-a): `Novy-Marx (2011, RF) operating leverage; proxy opex = sale_ttm - oi_ttm (includes D&A) because fields-v6 has no opex_ttm`.
   - The rendered DSL is asserted **byte-equal** to the pre-registered string `PREREG_DSL['opex_at']`:
     `decay_linear(group_rank((((sale_ttm - oi_ttm) / at) + (0 * log(at))), grp_ff12), 21)`.
4. **Asserts in `documents()`.**
   - **Kept from v4.2:**
     - theme ∈ THEME_IDS; tier ∈ TIER_RANK; direction ±1; smoothing default or a reason; base lookback re-parse.
     - total == **38** (was 40 in v4.2); unique ids and DSLs; `candidates[:37]` and `lineage[:37]` equal v4's.
     - **≤ 5 extras** for every candidate.
     - `referenced ⊆ declared` fields, with the v4 field list copied unchanged.
   - **R-b replacement:** v4.2's `theme not in WITHIN_INDUSTRY_THEMES and ranking == 'cross_section'` is replaced by
     three asserts:
     - v4's own rule: `ranking.startswith('within_industry') == (theme in WITHIN_INDUSTRY_THEMES and id not in exceptions)`;
     - `theme == 'profitability_quality'` and `ranking == 'within_industry_grp_ff12'`;
     - the parsed tree is `TsDecayLinear(CsRankG(base, field grp_ff12), 21)`.
   - **Added:**
     - ≤ **7** estimated slots per candidate (`MAX_SLOTS_PER_CANDIDATE`, the v4 library maximum);
     - the added ids are exactly `['opex_at']`;
     - byte-level check: v4's `candidates` array, as encoded in its file, is a prefix of v5.1's, followed by `,\n`.
5. **Library JSON.** It differs from `fund_industry_ic_v4.json` in exactly three places (checked by `diff`):
   - `id` is `fund_industry_ic_v5`;
   - the `profitability_quality` family description adds "and operating leverage (operating costs to assets)… high
     operating leverage", following the v4.2 precedent of updating the descriptions of themes that gain members;
   - the appended `opex_at` row.

   `fields`, the family ids and all 37 v4 rows are byte-identical.
6. **Recipe.** It is a deep copy of the v4 recipe with these changes:
   - id, library pin, preregistration (v5.1 R1'', commit c8192c46);
   - `generation`: rule `frozen-v4-37-plus-opex_at-v51`, the frozen_v4 pins, `new` with `ranking_rule` R-b, and the
     generator SHA-256;
   - themes: `opex_at` appended to profitability_quality, `expected_turnover_v51_additions`;
   - `within_industry.members` +opex_at (20);
   - `v51_additions`, `family_fixing` (hygiene statement, including the gpa-redundancy disclosure), templates and
     lineage;
   - admission `v4-prior-v1`, trials 38; composition note (ew-theme-v1 and ew-theme-aim-v1 re-fit on 38);
   - trials `v51_family` (admission 38, composition +2, construction +1, reference cell only);
   - `static_validation` (limits 5/7), `qualification`, and `data.operating_costs`.
7. **CLI.** `--check` compares both emitted files byte for byte and exits `fixed artifact differs: <name>` on a
   mismatch. It also prints the summary and the registry cross-check, the same as v4.2.

## Decisions (small ambiguities, recorded)

- **The recipe is also emitted and committed.** R-c names only the library JSON. The v4.2 pattern emits a recipe,
  and the fitter's `--recipe` path (`load_priors`, source `library+recipe`) and the provenance (frozen pins,
  lineage, templates) depend on it. The library alone still carries theme, tier, tier_rank, prior_sign and
  citation, which is enough for the fitter without `--recipe`.
- **Library id `fund_industry_ic_v5`** matches the file stem, as v4.2's `fund_industry_ic_v42` does. The IC runner
  only records the library id: the cache key is candidate id + DSL SHA + role/fields manifest
  (`strategy_ic_runner.cpp:903-925, 972-990`). So the 37 v4 entries still hit.
- **≤ 7 slots is asserted for every candidate**, not only opex_at. v4's maximum is 7, so this pins
  `max_compiled_slots` at 7.
- **No registry extension**, unlike v4.2. The pinned v4 module is used as is, so the v5.1 validator is exactly v4's.
- **`generate_fund_ic_v42.py`, all v4 and v4.2 files, and the fitter (`V4_THEMES`) are untouched.**
  `generate_fund_ic_v4.py --check` and `generate_fund_ic_v42.py --check` still pass.

## Static numbers (Python estimate; native `--plan-only` is authoritative)

| candidate | fields | extras | est. slots | dag nodes | prior bars |
|---|---|---|---|---|---|
| opex_at | at, grp_ff12, oi_ttm, sale_ttm | 4 (≤ 5) | 4 (≤ 7) | 13 | 20 |
| library v5.1 | 35 declared extras (unchanged) | capacity 5 (= v4) | max 7 (= v4) | max 23 | max 272 |

## Tests

New file `atx-impl/strategies/test_generate_fund_ic_v5.py` (10 tests; this follows the v4.2 precedent of one test
file per generator):

| test | checks |
|---|---|
| `test_documents_are_deterministic_and_committed` | deterministic output, committed bytes, recipe library pin |
| `test_total_is_38_and_v4_members_are_byte_identical` | v4 pins; `frozen_v4()` equals the disk bytes; 38 total; byte-level prefix of v4's candidates array; per-row, lineage, templates and fields identical; only the profitability_quality description changed |
| `test_opex_at_spec_fields` | the exact library row (id, family, DSL, horizons, sign_policy, theme, tier B, tier_rank 4, prior_sign 1, verbatim citation); lineage (roster 38, direction +1, within_industry_grp_ff12, s21, prior bars 20, fields, dsl_sha256); template; theme and within_industry membership; R-b tree shape; admission v4-prior-v1 with 38 trials |
| `test_memory_budget_extras_and_slots` | every candidate ≤ 5 extras and ≤ 7 slots; opex_at 4 slots and 4 extras; library max slots 7 and capacity 5 equal v4's; field users |
| `test_check_mode_accepts_committed_bytes` | the `--check` subprocess exits 0 and prints the library SHA |
| `test_check_mode_rejects_tampered_bytes` | a copy in a tmp dir with opex_at's prior_sign flipped makes `--check` fail with `fixed artifact differs` |
| `test_fitter_reads_the_v51_labels` | `fit_composition_weights.load_priors`: themes ⊆ V4_THEMES, prior_signs all +1, opex_at grade index 4 (B), source library+recipe |
| `test_opex_at_is_operating_costs_to_assets_with_the_asset_guard` | on the v4 numpy mirror: base = (sale - oi)/at; NaN iff at ≤ 0; lower OI gives a higher score |
| `test_opex_at_ranks_within_ff12_and_smooths_over_21_sessions` | group rank within FF12; NaN label excluded; 21-session linear decay equals a direct computation |
| `test_opex_at_is_causal_and_lookback_is_sufficient` | future scramble has no effect; prior_bars suffices |

Output:

```
$ "C:/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider atx-impl/strategies/test_generate_fund_ic_v5.py -q
..........                                                               [100%]
10 passed in 0.80s
```

Regression: the v4, v4.2 and v5 test files run together give `56 passed in 8.19s`.

`--check` output (abridged):

```
$ "C:/Program Files/Python312/python.exe" -B atx-impl/strategies/generate_fund_ic_v5.py --check
fund_industry_ic_v5.json 9e5ea08cb3c9a802f72e23dd069499b59294fba5708e0873d9dfc57f8a7458e0 28912 bytes
fund_industry_ic_v5.recipe.json a26670b0f7b6d1681a4ea8a4758da83841163ceaea8af5a4256ee718ad7c3aea 90828 bytes
candidates 38 (frozen v4 37 byte-identical, new 1); themes 9; max prior bars 272; max dag nodes 23; max peak slots 7 (limit 7); extra-field capacity 5 (limit 5)
  2 profitability_quality: gpa, opbe, cfoa, roe_q, roa, accruals, fscore, opex_at
registry cross-check ok (14 operators vs registry.cpp/typecheck); grp_ group typing checked: typecheck.hpp is_group_field accepts grp_<suffix> (T22)
```

## Root: build targets, tests and the `--plan-only` command

- **Build targets: none.** This change is pure Python and needs no CMake registration. The IC runner binary is the
  existing `build-equity/bin/atx-equity-strategy-ic.exe`.
- Root pytest (after the cherry-pick into pool-2):
  `"C:/Program Files/Python312/python.exe" -m pytest -p no:cacheprovider atx-impl/strategies/test_generate_fund_ic_v5.py -q`
- Plus `"C:/Program Files/Python312/python.exe" -B atx-impl/strategies/generate_fund_ic_v5.py --check`.

The `--plan-only` command is from T34a §5 and is unchanged. The guard needs a clean tree, so commit first.

```bash
cd C:/atx-wt/pool-2
export PATH="/c/atx-cache/vcpkg_installed/x64-windows/debug/bin:/c/atx-cache/vcpkg_installed/x64-windows/bin:$PATH"
L=atx-impl/strategies/fund_industry_ic_v5.json; LS=$(sha256sum $L | cut -c1-64)   # expect 9e5ea08cb3c9a802f72e23dd069499b59294fba5708e0873d9dfc57f8a7458e0
R2=build-equity/recent-fast-train-2020-2022-v2/manifest.json
R2S=210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de
FD=build-equity/recent-fast-train-2020-2022-v2-fields-v6
FS=32565c3212a0b06a4a0a1185aabf07aea2fc043906ff767e8489233a0ddfd7a8
IC=build-equity/bin/atx-equity-strategy-ic.exe
"C:/Program Files/Python312/python.exe" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 \
  --min-free-mib 512 --output build-equity/mega-v51-plan-run --bind $IC --bind $L --bind $R2 --bind $FD/manifest.json \
  -- $IC --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S --train-fields $FD \
  --train-fields-sha256 $FS --max-memory-mib 1536 --min-names 1000 --workers 4 \
  --candidate-cache build-equity/mega-candidate-cache --plan-only
```

**Expected (T34a §4-5).** Accept iff all of the following hold:

- train `required_bytes` **1,449,071,914** (= v4's 1,449,071,402 + 512 B for one composition row; 1,381.9 MiB;
  154.1 MiB headroom) ≤ 1,610,612,736;
- `candidates` 38;
- `max_compiled_slots` 7;
- `research_fields.resident_capacity` 5;
- `required_lookback` 272;
- candidate cache **37 hits + 1 miss (opex_at)**, because the fields SHA 32565c32 is unchanged.

`planned_loads` grows by at most 2, and `loaded` stays at the 35 v4 extras. Expected runtime is ≤ 30 s: the v4.2 u
pass with 37 hits and 3 misses took 30 s / 964 MiB.

If `required_bytes` exceeds 1,536 MiB, T35 opens (brief acceptance).

## Concerns

1. **Redundancy with gpa (disclosed in the recipe).** opex/at = sale/at − oi/at and gpa = sale/at − cogs/at share
   asset turnover. Expect a high rank correlation, so the fitter's redundancy screen may drop opex_at (gpa is tier A
   and ranks ahead). It still counts as one disclosed trial in the v5.1 family.
2. **Proxy definition.** `sale_ttm − oi_ttm` includes D&A and other operating items. Coverage is bounded by oi_ttm
   (.463 of member cells, T34a).
3. **The cache-hit count** assumes `build-equity/mega-candidate-cache` still holds the 37 v4 entries under fields
   32565c32 and the current VM identity. If the binary's VM identity changed since those entries were written, the
   cache goes cold: 38 misses, about 86 s / 1,012 MiB per the ledger's cold pass. That is still within 180 s, and
   `required_bytes` is unaffected.
