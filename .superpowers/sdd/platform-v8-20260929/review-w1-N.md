# Review W1 part 2, area N (files part 1 never read)

Snapshot `C:/atx-wt/pool-2` at 81abfa12 (code 237486fe). Read-only. Nothing built, run or edited, and no data read.
Method and finding format follow `review-w1-B.md`. Severity: **I** important, **M** medium, **m** minor. "Verified" means
the claim follows from code I read. Anything else is marked unverified, with what would confirm it.

No I finding. No read past the seal was found in any file of this area (see "Checked, no finding").

---

## Findings

### N-1 (M) Holdings reuse is blind to the seal, and three holdings payloads depend on it even for a role that ends before it

- **Where:**
  - `atx-engine/tools/research_fields_holdings.py:52-53`: `SEAL = rw.SEAL`, `SEAL_NS`.
  - FTD, `:777-782`: rows available at or after the seal are dropped before each settlement date's clock is taken. `:796-801`: `vis_d`, `prefix_vis`. `:821`: `S*(t)`.
  - Reg SHO, `:854` and `:883-889`: only unsealed list and on_list rows are kept, so only an unsealed late row can delay a list.
  - Short volume ext, `:982` and `:989`: `avail[s]` is the maximum over unsealed rows only. `:1020`: the session enters the window.
  - Reuse, `:1083-1091`: `reuse_inputs` and `entry_inputs` are the stage-manifest SHAs only.
  - Builder, `prepare_research_fields.py:2916-2931`: `load_prior` binds the role manifest, sessions, ids and member, not the seal. `:670-672`: `Role` checks only `days < SEAL`.
- **What is wrong:** B-11 argues that for one role the seal cannot change a payload. That does not hold for `ftd_shares_ratio21`, `regsho_threshold_days63` or `sv_offexchange_share126`.
  - Each of them sets a date's or a session's visibility clock to the latest `available_at` of its rows (a re-posted file carries a later HTTP Last-Modified).
  - The seal decides which late rows exist at all:
    - A re-post with `available_at` between two seals is dropped under the earlier seal, so the date is visible from its original rows (or is removed from the FTD settlement calendar).
    - Under the later seal the same re-post is kept and blocks the date for the rest of the role (FTD: `S*(t)` stops, then NaN after 60 days).
  - The seal reaches the holdings module only through the AST of `SEAL = rw.SEAL`, never as a value, so a `research_window.json` edit changes no fingerprint.
  - No holdings clock or definition text names the seal. Contrast `SEC_CLOCK` (`research_fields_sec.py:84-87`), which embeds `rw.SEAL_DATE` and so recomputes on a seal move.
  - `rw.partition_is_sealed` (`:263`, `:489`; SEC `:620`) is an imported body, outside every fingerprint (the B-1 class).
- **Failing scenario:**
  1. The seal moves by editing `research_window.json` (the next re-base) with no code change.
  2. An existing role dir is reused. Its days lie before both seals, so `Role` accepts it.
  3. `--reuse` of a fields dir built under the old seal then copies the three payloads. The new manifest's `seal` block states the new seal over bytes computed under the old one.
  - v8 avoided this only because W0-1 changed the `SEAL` statement text, a one-time change (integration 3 identity c: "reuse copied 0 of 63").
- **What it blocks:** byte identity of the next re-based fields build, and the W0-a overlap reading of these three fields.
- **Fix:** add `rw.SEAL_DATE` (or `WINDOW_ID`) to the holdings `reuse_inputs` / `entry_inputs` and record it on each entry. Alternatively, `load_prior` refuses a prior whose `seal.exclusive_end` differs from the current seal, which also closes B-11 for every module.
- **Verified** by reading.
  - **Unverified:** whether a real 2020-2023 row carries `available_at` between two candidate seals.
  - Identity c found 62 of 63 payloads equal between the seal-2025 and seal-2024 builds of the 3-year role. It attributed the one regsho difference to the stage republish, so the practical effect on FTD and svx was nil for that role.
  - To confirm on the next re-base, compare the three payloads of a cold build and a `--reuse` build of the same role.

### N-2 (M) spo-v3 cannot produce the 4x report that plan section 9 and E-29 require of every cell, and R-9 cannot run on it

- **Where:** `atx-impl/src/strategy_nav_v7.cpp:718-727`. Inside `if (o.spo_v1)`, which covers spo-v1, spo-v2 and spo-v3, `if (o.capacity) throw "spo-v1 does not run the capacity curve"`. The spo-v3 horizon is a registered constant: `strategy_spo_v3.cpp:121`, `strategy_spo.cpp:1227`.
- **What is wrong:**
  - The plan requires 4x of every cell. Plan section 9 (line 676): "Every cell also reports net Sharpe at 4x NAV". Ruling E-29: "every R cell reports 4x against its parent".
  - R-6 (spo-v3) is refused `--capacity-curve`, and the refusal is correct: the capacity pass holds the construction fixed and rescales impact and participation, but spo-v3's plan itself depends on the cost law and on the 1% ADV trade limit in NAV units.
  - The only other route is a separate run at `--initial-nav 4e9`. That is another book: the tracker plans against 4x costs and limits, and the ADV cap is set at 4x NAV, not at $1bn as in the parent's capacity row (E-15). It is not comparable with the parent's 4x row.
  - R-9 (three cells at theta .03/.04/.05 at 4x "on the final construction") has no effect on an spo-v3 book, because its horizon H = 20 does not read `--trade-fraction`.
- **What it blocks:**
  - The 4x report of R-6 and of every later cell if R-6 is accepted.
  - R-9 as registered.
  - The plan's "more at 4x" estimate for R-6.
- **Fix:** a ruling before R-6 runs. Either spo-v3 cells report 4x from a declared `--initial-nav` run with the ADV-cap difference stated, or 4x is waived for spo-v3 and R-9 is withdrawn or redefined when the final construction is spo-v3.
- **Verified** by reading (refusal, H constant, plan and ruling text).

### N-3 (M) The R-7 acceptance gates on K6 marginal IC, which pre-registration rule 8 says gates nothing; the K6 file the card copies is bound to nothing

- **Where:**
  - Plan R-7 acceptance (`docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md:822`): "at least half of the admitted new members have marginal IC HAC t above 0 (K6)". This is registered through prereg item 11.
  - `v8-prereg.md:17` (item 8) and the closing list: "the report-only columns ic_theta, f_theta and marginal IC gate nothing and select nothing".
  - `atx-impl/tools/alpha_report_card.py:361-363` and `:367-389`: `load_marginal_ic` accepts any K6 file. Its SHA pin is optional (`:1136`), and rows are matched by id only, with no role, window, pool, weights or library binding. `:933-934` copies the row into the card.
- **What is wrong:** the two registered texts contradict each other on the R-7 verdict, and no ruling in `progress.md` resolves it (grep of E-1..E-35 and PM3-*). No tool computes the R-7 criterion (grep for `marginal_hac_t` consumers finds only report printers). If it is read from cards, a K6 file of another role, window or pool is accepted silently.
- **Failing scenario:** R-7 passes dSR, mechanics and turnover, and 3 of 5 admitted new members have K6 t > 0. Under rule 8 the criterion does not apply. Under plan R-7 the wave passes. With 2 of 5 the two texts give opposite verdicts, and the choice would be made after the read.
- **What it blocks:** the R-7 verdict.
- **Fix:** a ruling before any R-7 read on which text governs. If K6 gates, bind `marginal_ic.json` (role manifest, window id, pool and weights SHA, library SHA), require its pin in the card and the cycle, and compute the criterion in `cycle_verdict`.
- **Verified** (texts, card code, consumer grep).

### N-4 (m) ew-theme-std-v1 weights read tiers and tier scores from the unpinned working-tree registry

- **Where:**
  - `atx-impl/tools/composition_rules.py:91-110` (`load_registry`), `:192-203`: the registry tier wins over the library/recipe tier (`resolve_tiers`), and `tier_scores` come from the registry.
  - Called from `fit_composition_weights.py:2153-2156`.
  - The slim recipe records the registry path and schema but no SHA (`generate_library.py:311`). No spec pins the registry (grep of `scripts/research_cycle.py`).
- **What is wrong:**
  - R-1's weights depend on a file that no pinned input covers. `provenance.std.registry_sha256` records it after the fact.
  - The admission order (`screen_v4`) uses the pinned library/recipe tier, while the weights use the registry tier. A disagreement between the two is not refused.
  - The C-1 report's statement "the registry is read unpinned for the admissible theme list and the order of appended themes only" is not true of this composition.
- **Scenario:** a hand edit of `tier_scores` or of an entry's tier (`register_alpha` forbids only the add-alpha route) lands between the R-1 lock and a rerun. The weights change while every pinned SHA stays the same. A resumed cycle then keeps the old output by its marker (C-13).
- **Fix:** pin the registry (`--registry-sha256` on the fitter, recorded in the spec), or refuse a registry tier that differs from the library tier.
- **Verified** by reading. No such edit exists today.

### N-5 (m) The pooled (era) fit already accepts ew-theme-std-v1, contrary to its own text and to the premise of E-35

- **Where:**
  - `fit_composition_weights.py:134`: the doc says "ew-theme-v1 / ew-theme-v6 only".
  - `:1807-1809`: the refusal text says the same, but the check only excludes `ew-theme-aim-v1`.
  - `:2153-2156`, `:2223-2230`: the std path runs under `pool`, and each per-era file copies `theme_standardise`.
- **What is wrong:** E-35 plans a lane because the pooled fit "ignores the std variant". The std weights take no data (tiers and themes of the admitted set), so the pooled std fit already produces a consistent file. The text misleads the E-35 lane, and no test pins the pooled std output.
- **Fix:** correct the text and add a pooled-std test; E-35's lane then needs only the aim variant.
- **Verified** for the fitter.
  - **Unverified:** that `era_pool` and pooled `nav_summ` accept a schema-v2 weights file (area P).

### N-6 (m) The slim recipe counts the eight `_f49` re-screens as admission trials, against R2-e

- **Where:** `atx-impl/strategies/generate_library.py:298` and `:318-319` (`admission_trials=len(new)`).
- **What is wrong:** R2-e rules that the 8 re-screens count 0. The v8.0 recipe will say 15 against the registered 7 (the draft notes the gap).
  - No tool reads the field today (grep), so no Appendix A figure is wrong yet.
  - A reader of the pinned recipe gets the other count.
- **Fix:** mark re-screens in the registry (`rescreen_of`) and exclude them from `admission_trials`, or record both counts.
- **Verified.**

### N-7 (m) aim-partial-v6 silently accepts `--hold-band` / `--adv-hold-q`

- **Where:** `strategy_nav_v7.cpp:645-657` rewrites `--rule aim-partial-v6` to `aim-partial-v5`, so the replay's "aim-partial-v5 only" validation of the band passes. Only spo-v1/v2 refuse the shaping (`:721-725`).
- **What is wrong:** an unregistered combination runs, and its rule id is relabelled `aim-partial-v6+...+hold-band-B`. No v8 cell uses it.
- **Fix:** refuse both flags with v6, as with spo-v1/v2.
- **Verified.**

### N-8 (m) decide on a hold-band book without state columns silently resets the band

- **Where:**
  - `atx-impl/src/strategy_live.cpp:1370-1378` (`decide_row`), `:1304-1306` (`state_in` "none"), `:542-547` (unset default), `:789-820` (`health_checks`: no hold-state check).
  - CSV pairs, `:535-539`: the CSV path does not enforce the both-NaN-or-both-finite rule that `strategy_holdings.cpp:201-207` enforces.
- **What is wrong:** with `nav.hold_band` > 0 and a broker positions file (no `rank_set,desired_prev`), every member is first-set. That is a different book from the replay's, yet health stays `ok`, and `decision.json` records only `names_set_in: 0`.
- **Fix:** a health Warn, or a refusal unless the book is declared first-deployment, when `hold_band_declared && !has_hold_state` on non-flat positions. Apply the pair rule in the CSV reader. v8 deploys nothing (E-16), so this does not block a cell.
- **Verified.**

---

## Checked, no finding

- **SEC fields, point in time:**
  - `usable_from` is the first session whose 22:00 UTC mark follows the acceptance, plus 1. `Latest` and `Windowed` need clock order, and `g` and `lexsort` supply it.
  - Rows at or after the last role mark or the seal are dropped.
  - The expected date uses only rows at or before A in (cik, available_at, accession) order.
  - Insider presence and windows use mark(t-1). Cohen-Malloy-Pomorski classification of year Y uses only trades filed before Y-01-01.
  - The 8-K accession clock is the latest of its items. `k8_item402_63` is a second mask on the same accession and adds no bytes to the other fields.
  - The calendar suffix is a rule calendar, not data.
  - Reuse: the clock embeds the seal date; `requires` shares_out is enforced (`prepare_research_fields.py:2837-2840`); entry and run pins are both lowercase SHAs.
- **v8 fields, point in time:**
  - `LatestRows` is the builder's issuer selection: `advance` to mark(t-L), staleness by `period_end`, primary lines, t < L NaN (`prepare_research_fields.py:1941-1965`).
  - `QuarterIndex` takes a quarter only from pairs first seen at or before r, and items from the last row at or before r in global clock order, so everything is visible at t.
  - `me_company` is read at t-1 (row 0 NaN); `grp_sic2` and `member` at t.
  - Checked against E-20/E-30: VARROA/VARSGR with 12 of 16 finite; the EPS denominator `(|EPS_q-4|+|EPS_q-8|)/2`; strict opposite signs; ties to the later `period_end`; an inclusive tercile; peer medians per measure; R&D missing = 0.
  - `load_events` does not filter rows by the requested items, so co-requesting both fiscal-quarter fields does not change either one's bytes.
  - Reuse pins bridge, events and lag, plus `requires`.
- **`grp_ff12f49`:** FF12 Money 11 and FF49 45-48 match the builder's numbering. The rule is cellwise, so the NaN pattern is `grp_ff12`'s.
- **Holdings, point in time:**
  - 13F `V(P)` is the latest acceptance of the quarter's filings by the deadline; aggregate rows later than `V(P)` are dropped.
  - FTD uses prefix visibility. The Reg SHO window ends at t-2 against mark(t-1). A svx session enters once visible.
  - shares_out is read at the quarter-end or window session, before t.
- **Fitter, seal:**
  - `RoleManifest` refuses any session or score end at or after the TRAIN end.
  - Every cache sidecar must name role `train` and this role's SHA.
  - Labels are r(d+2) with d < score_end - 2; era roles are refused past the TRAIN end.
  - Nothing past the seal is read.
- **Fitter stores:** the factor key is role, window, semantics tag, payload SHA, producer fingerprint and `horizon_fingerprint` (the imported `theta_book_returns` is covered). The aim key adds the TRAIN window.
- **ew-theme-std-v1, Python against C++:**
  - The C++ side (`strategy_ic_composition.cpp:180-187, 282-306, 325-331`) adds each member's `w_k s_k rank_k` into its theme plane, re-ranks over names with a present member, and adds `std_mass` (the sum of pinned weights > 0) times the rank.
  - This equals the Python block (themes listed for weights > 0) and the registered rule. Agrees with part 1 (B).
  - The member cap conserves mass and refuses when infeasible (members < 2T).
- **Card:**
  - `row_decay` GEMMs are exact: centred ranks are multiples of .5 and the masks are 0/1, so every partial sum is exact far below 2^53. A missing-only batch gives rows bit-identical to a full batch, so the C-1 "open risk" on batch height does not arise.
  - The store key covers `min_names`, both field pins, role, window and producer.
- **generate_library:** alpha id, DSL, theme, tier, prior sign and citation are immutable through `register_alpha`. K1 validation checks the DSL SHA, the budget or its exception, and that the token match equals the exe's extra fields.
- **strategy_live:**
  - The seal check comes before any load.
  - The NAV recipe pin includes `initial_nav`, so the ADV cap's NAV in decide equals the run's.
  - Hold state is read only with the band on, and b = 0 is identical to no band.
  - The cadence phase is anchored at `decision_begin`, as in the replay's extended phase.
  - Optional nav keys enter the pin only when on.
- **strategy_holdings:** the v1 layout bytes are unchanged without a declared band. The reader accepts either layout by column names. Session tiling, SHA and exact-index checks are present, and JSON exceptions are caught.
- **strategy_nav_v7:**
  - Warm-up decisions leave no TC record (`observe`).
  - E-26 refusal applies to spo-v1/v2, and the registered spo-v3 constants are refused.
  - The void path writes no NAV file.

## Table of findings

| ID | sev | file:line | one line |
|---|---|---|---|
| N-1 | M | `research_fields_holdings.py:777`, `:883`, `:982`, `:1083` | FTD/regsho/svx bytes depend on the seal through late rows; the reuse key ignores the seal |
| N-2 | M | `strategy_nav_v7.cpp:726` | spo-v3 refuses the capacity curve: no 4x report (plan §9, E-29); R-9 undefined on spo-v3 |
| N-3 | M | `alpha_report_card.py:367`; plan :822 vs prereg item 8 | R-7 gates on K6 while rule 8 says K6 gates nothing; K6 file unbound |
| N-4 | m | `composition_rules.py:197`, `generate_library.py:311` | std weights from the unpinned registry; registry tier beats the library tier silently |
| N-5 | m | `fit_composition_weights.py:1807` | pooled fit already accepts std, against its text and E-35's premise |
| N-6 | m | `generate_library.py:319` | recipe counts 8 `_f49` re-screens as admission trials (R2-e: 0) |
| N-7 | m | `strategy_nav_v7.cpp:645` | aim-partial-v6 accepts `--hold-band` / `--adv-hold-q` |
| N-8 | m | `strategy_live.cpp:1370` | hold-band decide without state columns resets the band with health ok |

Counts: I 0, M 3, m 5.

## Coverage (what was read)

| file | lines read | result |
|---|---|---|
| `atx-engine/tools/research_fields_sec.py` | 1-996 (whole) | clean (B-1 class noted under N-1) |
| `atx-engine/tools/research_fields_holdings.py` | 1-1311 (whole) | N-1 |
| `atx-engine/tools/research_fields_v8.py` | 1-609 (whole) | clean |
| `atx-impl/tools/fit_composition_weights.py` | 1-2390 (whole) | N-4 (dispatch), N-5 |
| `atx-impl/tools/composition_rules.py` | 1-300 (whole; area B also read it) | N-4 |
| `atx-impl/tools/alpha_report_card.py` | 1-1155 (whole) | N-3 |
| `atx-impl/strategies/generate_library.py` | 1-456 (whole) | N-4, N-6 |
| `atx-impl/src/strategy_live.hpp` | 1-145 (whole) | clean |
| `atx-impl/src/strategy_live.cpp` | 1-1512 (whole) + sprint diff | N-8 |
| `atx-impl/src/strategy_holdings.hpp` | 1-133 (whole) | clean |
| `atx-impl/src/strategy_holdings.cpp` | 1-381 (whole) | clean |
| `atx-impl/src/strategy_nav_v7.hpp` | 1-185 (whole) | clean |
| `atx-impl/src/strategy_nav_v7.cpp` | 1-836 (whole) + sprint diff | N-2, N-7 |
| `atx-engine/tools/prepare_research_fields.py` | 638-680, 1663-1732 (returns), 1733-2010, 2455-2612, 2612-2660, 2734-2935, 3165-3200 (context) | context for N-1; no own finding |
| `atx-impl/src/strategy_ic_composition.cpp` | 120-345 (context) | clean (std C++ side) |
| `atx-impl/src/strategy_nav_replay.cpp` | 1500-1530 (recipe), grep of cadence / initial_nav (context) | context |
| `atx-impl/src/strategy_spo.cpp` 1220-1235, `strategy_spo_v3.cpp` 118-130 | context | context for N-2 |
| sprint docs | plan §3-4, 441-535, 668-848; `progress.md` rulings; `v8-prereg.md`; `library-v8-draft.md` R2-8, R7-4..R7-8, §6, §8; reports C-1, C-2, C-3, A-1, R-4; `.agents/cpp/agent.md` §0-10 | context |

Left to area T: `atx-impl/tests/strategy_live_test.cpp` (matches `strategy_live*`) and every `test_*` file next to the files above.
