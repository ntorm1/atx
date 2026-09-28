# Task V6-W: composition `ew-theme-v6` and PIT-linked operating-company universe option

**Pool:** C:/atx-wt/pool-4. First: `git status` clean, then `git checkout -B feat/mega-alpha-v6-w-20260927 e0dfb8c7`.
Work ONLY in pool-4. Never build, never run real data, never spawn subagents, never touch C:/atx or other pools.
Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
**Model:** Opus 5.5. **Report:** `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-V6W-report.md`.
Reply < 15 lines.

**Read first:** `.superpowers/sdd/mega-alpha-20260926/v6-code-review-signal.md` (findings I1, I2, I4, I5; levers L1-L3)
and `v6-literature.md` sections 1 (levers 1, 4, 5) and 5; the "## v6 revision" section of
`.superpowers/sdd/mega-alpha-20260926/v4-prereg.md` (items V6-U and V6-W are this task -- the rule text there is binding
and must be implemented literally). Code: `atx-impl/tools/fit_composition_weights.py` (+ `tests/test_fit_composition_weights.py`
or `atx-impl/tools/test_fit_composition_weights.py`), `atx-impl/src/strategy_ic_composition.cpp:141-156` (coverage handling,
read-only unless the rule (d) needs it), `atx-engine/tools/prepare_recent_research.py:~348` (universe/member construction),
`studies/v51_train.sh` (phases u / fit / w / nav) and the admission JSON `build-equity/mega-weights-v51-ew/admission.json`
(theme, standalone tau per member; read-only TRAIN metadata is allowed).

**Deliverables:**
1. `fit_composition_weights.py --composition ew-theme-v6` implementing exactly:
   (a) drop theme `low_risk`; (b) merge `options_implied` into `short_interest` (one theme, equal weight within);
   (c) fast-sleeve shrink: any admitted member with standalone daily tau >= .08 (from admission.json, which you must cite
   by key) gets within-theme weight x 1/3, the freed mass reallocated pro rata within the same theme; (d) equal weight
   across the resulting themes; coverage redistribution: when a member is missing on a day, its mass stays inside its
   theme (spread over present members) instead of the current behaviour. If (d) requires a C++ change in
   strategy_ic_composition.cpp, implement the smallest one behind a composition-id check and prove ew-theme-v1 bytes are
   unchanged; otherwise implement it in the weights file semantics and document how.
   Provenance block in the weights JSON: rule id, tau threshold, list of shrunk members, dropped/merged themes, input SHAs.
2. Universe option in `prepare_recent_research.py`: `--universe linked-operating-v1` (default = current) that keeps only
   members with a PIT issuer link AND common-stock share class (drop ETF/ETN/SPAC/ADR/unit/warrant). Use the fields
   and link tables that already exist in the u pass (cite the manifest keys: the signal review cites fields-v6
   manifest.json:4582-4584 for the unlinked share). The role/membership output must record the universe id and the
   dropped share per day in its manifest. No look-ahead: the classification at date d may use only information published
   <= d.
3. `studies/v6_train.sh` is being written by lane V6-C1 in pool-10; do NOT create it. Instead write
   `studies/v6_w.env.example` listing the env knobs your changes need (`COMPOSITION=ew-theme-v6`, `UNIVERSE=...`) and
   the exact fit / u commands for the root, so the controller can merge them into v6_train.sh.
4. Tests: extend the composition fitter tests with synthetic fixtures for (a)-(d) (weights sum, theme mass, shrink math,
   provenance) and a test that `ew-theme-v1` output is byte-identical to before for the same inputs. Add a small pure-python
   test for the universe classifier. Run both test files (pure Python; no build, no real data).

**Constraints:** do not read any 2023+ data or per-candidate VAL statistic. Do not edit the library JSON / generators,
NAV C++, nav_summ.py or v5_train.sh / v51_train.sh. Report: changes (file:line), how ew-theme-v1 bytes are proven unchanged,
the exact root command lines for fit / u / w, and concerns.
