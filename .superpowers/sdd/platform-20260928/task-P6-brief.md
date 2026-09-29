# Task P6: pitch iteration 3 config for the current best version (v7.1), render-ready

**Pool:** C:/atx-wt/pool-11. `git checkout -B feat/platform-v7-p6-pitchcfg-20260929 <BASE>`, BASE = `git -C C:/atx-wt/pool-2
rev-parse HEAD` (it contains your P5 work and every v7 result). Same rules as P5: never build, never run real data (IC /
NAV / fit / risk / decide), never spawn subagents, never touch C:/atx, never read validation / 2023+ statistics beyond
what the existing config already prints, no pushes, do NOT write into C:/atx-wt/pool-2 (read-only inputs). Smoke
renders go to C:/atx-wt/pool-11/build-equity-p5/. Trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
Report: task-P6-report.md in YOUR pool's .superpowers/sdd/platform-20260928/, committed with `git add -f`.

**Read first:** C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/progress.md, the five entries dated 2026-09-29 (top of
the file), and v7-prereg.md sections "Library v7.0", "Library v7.1", "Universe trial U-lo3", "Construction trial: spo-v2".
Every number below is from that ledger; the config must take numbers from the files, never from this brief.

**Deliverable:** NEW files (the v6 configs and HTML stay untouched):
`docs/plans/mega-alpha-v7-pitch.config.json` -> root renders `docs/plans/2026-09-29-mega-alpha-v7-pitch.html`.
Start from mega-alpha-v6-pitch.config.json and change:
1. The book being pitched = the current best accepted cell: library v7.1 on role lo1,
   `build-equity/mega-nav-v71u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` (S2 net SR +1.405, gross 1.809, cost 13.49
   bps/$, tau .0379). Scoring input `build-equity/mega-nav-v71-summ-n37.json` (dsr_n 37), PBO
   `build-equity/mega-nav-v71-pbo-n37.json`; update every literal "nav_summ n29" source label in `facts`. Library file
   atx-impl/strategies/fund_industry_ic_v71.json; weights / admission build-equity/mega-weights-v71-ew; u pass
   build-equity/mega-v71-train-u-1; combined build-equity/mega-v71w-train-ew-1.
2. Cell table and verdict badges, in ledger order: v6.1 baseline (reference), C1-C3 REJECTED, spo-v1 REJECTED (defect),
   v7.0 ACCEPTED (wave 1, +1.313), U-lo3 ACCEPTED (v7.0 on role lo3, +1.332;
   `build-equity/mega-nav-v70-lo3-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`), spo-v2 REJECTED (research result, +0.538;
   `build-equity/mega-nav-v70-lo3-spo-v2-G1.0`), v7.1 ACCEPTED (wave 2, +1.405). State plainly that V7-F (v7.1 on role
   lo3) is pre-registered, both its conditions are met, and it has NOT been run.
3. Sections: capacity curve and cost stresses from `build-equity/v7-71-nav-stress` (the v7.1 cell with --cost-v2
   --capacity-curve; its S2 daily CSV is byte-identical to the cell); risk model bias from `build-equity/v7-f3-risk`
   (atx-risk-v1.1; say that v1 was corrupt at the source and why, from the ledger); report cards sets v6.1, v7.0
   (`mega-cards-v70`), v7.1 (`mega-cards-v71`); monitor baseline `build-equity/mega-monitor-v71` (keep the v6.1 one if
   the schema differs); ops loop (decide / orders / reconcile) stays on the v7-w4-* evidence, labelled as measured on
   the v6.1 book; integrity runs n32, n33, n34 (`mega-nav-v70-summ-n34.json`), n35
   (`mega-nav-v70-lo3-summ-n35.json`), n36 (`mega-nav-v70-lo3-spo-v2-summ-n36.json`), n37 primary; trial ledger
   `build-equity/trials.jsonl` (37 lines) with every verdict; validation 2 of 3 spent, 2025+ reserved.
4. Honesty items the pitch must carry in prose, each with its number: (a) freeze gate UNMET: cell-count DSR .8247 < .95
   at N 37 (effective-N DSR .9282 shown beside it, never substituted); (b) each accepted step is a sign result at or
   inside about one SE (dSR +.073 / +.019 / +.093); (c) wave 2's four members have weak or wrong-signed standalone
   TRAIN ICs (ins_opp is admitted with its prior sign against a negative TRAIN IC), so the gain reads as
   diversification from a tenth theme; (d) the handoff's "DSR .9032 at N 33" was the N 32 figure; (e) everything is
   TRAIN 2020-2022; 2023-2024 was read twice and a third read needs the owner (U1).
5. Title / dates / version strings say v7.1 and 2026-09-29. Expect `unavailable blocks 0`.

**Tests:** existing mega_report tests green; add one test that the v7 config parses and every configured path key is
either present in a fixture root or reported unavailable (no crash).

**Report** (<= 30 lines): config keys changed, inputs with sha256, smoke-render result (unavailable blocks, n/a marker
count, size), anything you could not source from a file, the exact root render command. Reply to the parent in < 12
lines with the commit sha.
