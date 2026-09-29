# Task L8: library v7.1 = v7.0 + wave 2, research-cycle spec v71 on fields-v9 (no runs)

**Pool:** C:/atx-wt/pool-10. `git status` clean, then `git checkout -B feat/platform-v7-l8-libv71-20260929 <BASE>`,
BASE = `git -C C:/atx-wt/pool-2 rev-parse HEAD` at the time you start (it contains library v7.0 and spec v70).
**Rules (binding):** never build C++; never run IC / NAV / fit on real data or any returns-conditioned statistic
(synthetic fixtures and `research_cycle.py plan` dry-run via an overlay root are fine); never spawn subagents; never
touch C:/atx; never read validation / VAL / 2023+ statistics; no pushes. Do NOT read the v7.0 per-candidate results to
change anything about wave 2 (the wave-2 text is frozen). Python "C:/Program Files/Python312/python.exe". Trailer
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Write files with the Write tool.
**Report location:** task-L8-report.md in YOUR pool at C:/atx-wt/pool-10/.superpowers/sdd/platform-20260928/, committed
with `git add -f`. Do NOT write into C:/atx-wt/pool-2 (real-data runs there are voided by any write).

**Binding spec:** C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/v7-prereg.md section "Library v7.1 = parent + wave
2" (rulings W2-a..e) and library-v7-wave2-prereg.md sections 1-5. Wave 1 was ACCEPTED, so by ruling W2-a the parent is
library v7.0 (all 44 entries byte-identical and in order, including its non-admitted members) and the reference cell is
`build-equity/mega-nav-v70u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`. Implement the four DSLs literally:
ins_opp `rank(ins_opportunistic_net)`; inst_best_ideas `rank(decay_linear(inst_best_ideas, 21))`; ftd_fail
`rank((-1 * ftd_shares_ratio21))`; ea_overdue `rank((-1 * max(sign((-1 * ea_days_to_expected)), 0)))`; themes, tiers,
prior signs and citations as in the referenced text; family `ownership_flow` declared in v7.1.

**Deliverables**
1. atx-impl/strategies/generate_fund_ic_v71.py -> fund_industry_ic_v71.json + .recipe.json (roster 48 <= 56; v7.0
   prefix identity test; deterministic; shas recorded). Checker accepts v7.1 against the fields-v9 manifest.
2. scripts/specs/v71.json, modelled on v70.json: fields = build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9
   as built (pinned manifest sha 8fd00e9f44b475116f483e133c03fe031618390060b8374180278f1cd7b8769b; phase done, no
   rebuild); an identity phase BEFORE the u pass per ruling W2-e and section 5 "Identity before the cell": (iii) the
   IC pass on fields-v9 reproduces the parent's orientation / train_daily_ic member rows byte for byte (the candidate
   cache is content-keyed by DSL sha + field payload sha and the 55 v8 payloads are byte-identical in v9, so seed
   `mega-candidate-cache-v71` by hard links from `mega-candidate-cache-v70` and expect 44 hits + 4 evaluations), (iv)
   the reference construction re-run with `--fields` = fields-v9 reproduces the reference cell's S2 daily CSV bit for
   bit (a `nav` phase `ref-v9` into `build-equity/mega-nav-v70u-ref-fields-v9`, compared by a check step; mismatch =
   hard stop, no trial). If research_cycle.py lacks a generic "compare files" check phase, add a small one with tests.
   Then u, fit (ew-theme-v1 / v4-prior-v1, 10 themes when an ownership_flow member is admitted), card, gate (the four
   wave-2 rows; require any), w, nav (output `mega-nav-v71u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`, aim-partial-v5,
   L 1.247, `--fields` fields-v9), monitor, summ (34 ledgered cells + this one; `--dsr-n 35`; reference = the v7.0 cell;
   bare --pbo; ledger). Note: if another construction cell is ledgered before this one runs, root changes dsr_n; make
   it one obvious key.
3. Tests: generator identity / determinism, checker on v7.1, spec parse + plan dry-run (overlay root), any new cycle
   phase.

**Report** (<= 40 lines): files, shas, test counts, the exact root command lines (cache seed, plan, run, the JSON
scoring rerun), expected u-pass memory with 63 resident-capable fields, anything untested. Reply to the parent in < 15
lines with the commit sha.
