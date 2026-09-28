# Task V6.1: FINRA long-horizon shorting-flow field + library v6.1 member + root run script

**Pool:** C:/atx-wt/pool-10. First `git status` clean, then `git checkout -B feat/mega-alpha-v61-svflow-20260928 17a12949`
(17a12949 = pool-2 HEAD with the pre-registration). Work only in pool-10. Never build C++, never run the pipeline or any
IC/NAV pass, never spawn subagents, never write to C:/atx (READ-ONLY there: you may open a few raw FINRA files in
C:/atx/atx-db/data/raw/finra_short_volume to learn the format and symbology; never relate them to returns).
Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report:
C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-V61-report.md. Reply < 12 lines.

**Binding spec:** section "## v6.1 sub-alpha" at the end of `.superpowers/sdd/mega-alpha-20260926/v4-prereg.md`. Implement it
literally: field `sv_ratio126`, candidate `sv_flow`, prior sign negative, theme short_interest, FF12-demeaned, one variant.

## Deliverables
1. `atx-engine/tools/prepare_research_fields.py`: new optional field `sv_ratio126` in the builder's field registry, sourced
   from a new `--finra-short-volume <dir>` argument (CNMSshvolYYYYMMDD.txt.gz files). Exactly the prereg formula: at
   session d, sum ShortVolume / sum TotalVolume over sessions d-126..d-1 on which the instrument's PIT ticker appears;
   NaN if < 63 such sessions or zero total. Lag 1 session (the file dated d is never visible at d). Ticker -> instrument:
   REUSE the builder's existing PIT FINRA symbol mapping used for short interest (read how `si_shares` maps FINRA symbols
   through TickerHistory3; FINRA symbology such as class-share suffixes must be handled the same way). Record in the
   fields manifest: source dir, count and sha256 of a sorted (name, size, sha256) list of the files read, date range, the
   formula id `finra-cnms-ratio126-lag1-v1`, and per-year coverage of finite cells on member rows. Memory: stream files one
   day at a time (the builder's caps are 700 MiB / 1800 s); do not load all 2,045 files at once. Existing fields'
   outputs must be byte-identical when `sv_ratio126` is not requested (prove it: a test that builds a synthetic role
   with and without the new field and compares the other fields' bytes).
2. Library v6.1: `atx-impl/strategies/generate_fund_ic_v61.py` -> `fund_industry_ic_v61.json` (+ `.recipe.json`) =
   library v6 (5ee66d13, byte-identical candidate entries, same order) + `sv_flow` appended in theme short_interest,
   prior sign -1, tier B-, citation Wang-Yan-Zheng 2020 JFE. DSL: the FF12 group-demean of `sv_ratio126` using the SAME
   group op and grp field idiom the existing industry-adjusted members use (read `ind_adj_rev_5` / `within_ind_mom` in
   fund_industry_ic_v6.json and the evaluator in atx-engine vm.hpp / ts_ops.hpp for NaN semantics; no time-series op on
   top: the field is already the 126-session aggregate). Deterministic canonical JSON like v6; record sha256s. Update or
   extend `check_fund_ic_v6.py` so it validates v6.1 against a manifest that includes `sv_ratio126`.
3. `studies/v61_train.sh` (in `.superpowers/sdd/mega-alpha-20260926/studies/`, commit with `git add -f`): derived from
   `studies/v6u_train.sh` (read it; same conventions, pins, never-overwrite, `set -uo pipefail`, DRY=1 prints commands).
   Phases: `fields` (restricted role build-equity/recent-fast-train-2020-2022-v2-lo1, the 40 fields of
   lo1-fields-v6b PLUS sv_ratio126 -> build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7), `u` (library v6.1, own
   cache mega-candidate-cache-v61), `p1` (print the sv_flow admission row from the fit output: status, runner sign vs prior,
   HAC t, tau, max |rho| and with whom; exit 10 if not admitted -> stop), `fit` (ew-theme-v1), `w`, `nav` (the final
   construction flags with LEV=1.247 fixed; output dir mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247),
   `summ` (nav_summ paired vs build-equity/mega-nav-v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247, --dsr-n 29), `all`.
   Note fit runs before p1 in practice (admission is computed by the fitter): order phases so p1 reads the fit's
   admission.json and the script stops before `w` when P1 fails.
4. Tests (pure Python, synthetic): the ratio formula incl. lag, min-count and zero-total rules; symbol mapping reuse;
   byte-identity of other fields; generator determinism and v6 entries unchanged; checker. Run them with
   "C:/Program Files/Python312/python.exe" -m pytest and report counts.

Report: changes with file:line, how PIT/lag is guaranteed, expected RSS/time for the fields phase on the lo1 role
(5,627 instruments x ~1,155 dates), the exact root command lines, and concerns (e.g. symbology gaps, days missing from
FINRA, market-maker contamination).
