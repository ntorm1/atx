# Task YPRE report: pre-registration of expansion Y (lane YPRE, 2026-10-02)

Worktree `C:/atx-wt/pool-13`, branch `feat/platform-v8-ypre-20261002`, base `798d3b23`. Documentation only: no C++, no
build, no real data, no subagent. **Status: DONE_WITH_CONCERNS** (concerns: YP-10 and three rulings cited by lanes but
not given to this lane; see below).

## What was written

`.superpowers/sdd/platform-v8-20260929/v8y-prereg.md`, in the section order of `v8x-prereg.md` as the brief fixes it:
1 books (X-F0 = Y-S's parent, Y-F0 = claims book after Y-5, Y-F = levered book); 2 windows (unchanged); 3 trial
counting from X-F0's ledger state and the 15 hand-written admission trials (theme, tier, sign, horizon class, half-life,
Y-5 bucket, DSL SHA-256, source, argv line SHA-256); 4 DSR_tot / DSR_hand / DSR_v8 with the counts now including X-7,
the campaign, X-9 and Y; 5 the Y budget (6 cells incl. X-10, +15 admission, roster 96); 6 order and acceptance per
PM8-10 (e) with registered and printed-only criteria, PM6-6 per cell, PM7-35, one change per member, the undefined
cases; 7 the adoption print (v8x section 7 as PM7-34 (2) amended it, once after Y-1); 8 X-10 and Y-1; 9 fields v15 (two
processes, `conn_ret63` alone with `--reuse`); 10 `merger_arbitrage` and the iv_vol_of_vol clock repair; 11 the
half-life table (both printed, YCOMB's recommended); 12 voids; 13 preconditions P1-P14 in PM8-9's merge order; 14 the Y
list in v8x section 14's format. Appendix A (report block), B (open choices YP-1..YP-15), C (cross-lane checks).

## How root verifies

- Every SHA-256 in the file (36 distinct) was recomputed with Python from the lane reports read by `git show` at the
  heads named in section 14: 15 / 15 DSL strings equal the lanes' values; YSIG's 10 argv line prefixes and 9 table
  prefixes reproduce; the 3 carried argv lines equal `task-LIB2-report.md` up to the four placeholders; the 4 YCOMB
  templates hash as pinned (`git cat-file blob <commit>:scripts/specs/v8/y-*.json | sha256sum`); the two proposed
  iv_vol_of_vol repair strings hash as printed. **No SHA mismatch between lanes.** To re-check one:
  `git show feat/platform-v8-ysig-20261002:.superpowers/sdd/platform-v8-20260929/task-YSIG-report.md` and hash the
  `--dsl` value of the add-alpha line (UTF-8, no newline).
- Identity: documentation only; no code path changed (`git diff --stat 798d3b23..HEAD` lists the two files).

## Open choices for the PM (Appendix B; each with its cost if wrong)

YP-1 book names; YP-2 DSR_hand in Y (N_tot - M - mined-wave lines, n/a if X-9 is in the lineage); YP-3 X-10 counted
once inside the +6; YP-4 YCOMB's half-life table + `merger_arbitrage` 126 (PM8-13, cited by YCOMB, may already pin it);
YP-5 iv_vol_of_vol repair as `delay(iv_atm_21d, 1)` (`4d42a72b...`); YP-6 citation checks correct at 0 trials; YP-7
driver for Y-S/Y-3/Y-2/Y-5, hand for X-10/Y-1, PM6-6's second correction by hand; YP-8 Y-1 runs after a rejected
X-10; YP-9 Y-1's tau/net limits at X-10's scaled limits; **YP-10 add `merger_arbitrage` to the two-speed table before
the build** (else Y-5 is refused whenever `deal_target` is kept); YP-11 OD-3 reads Y-F0, never Y-1; YP-12 Y-S IC
memory re-probe (about 3,200 MiB by linear scaling) and marginal cap 720 s; YP-13 roster order; YP-14 extra prints;
YP-15 accept YCOMB's Y-5 / Y-1 composition (`3ff73201`).

## Inconsistencies found (Appendix C)

PM8-8 (6) points the repair at `iv_rv_spread_xe`'s convention, but that string also reads `iv_atm_21d` at lag 0; the
two-speed C++ table lacks the 13th theme; YSIG's Y-1..Y-10 member numbering collides with YCOMB's cell names; class
definitions and five theme half-lives differ between YSIG and YCOMB (no bucket differs); YDATA registered no member
horizons and no `deal_pending` L2 row; the wave driver allows one PM6-6 correction (PM6-6 allows two) and has no
"4x not lower" criterion; the Y-1 template text says parent X-F0; YOPS wrote `group_sum` though PM8-6 listed it as an
ask only; PM8-12 and PM8-13 are cited by YCOMB but were not in the draft given to this lane. Lane heads moved while
this ran (YCOMB `4ffd0eb0` -> `97eb5d0f`, YINFRA `dde31df3` -> `2697c3e1`); the file pins the latest heads read.

## Hygiene

Read: lane-rules.md; `v8x-prereg.md` (all); progress.md PM sessions 6-7 (public cell and X batch 1 numbers);
`pm8-rulings-draft.md`; the four lane reports and the YINFRA plan doc by `git show`; YCOMB templates and greps of its
two-speed / NAV refusal code; YINFRA `wave_rules.py`; `dsr_total.py`; registry rows for `iv_atm_21d` and
`iv_rv_spread_xe`; two lines of `task-XDATA-report.md` (IV clock); grep of `task-XIMP-report.md`; LIB2 argv lines (by
script). Nothing under `build-equity/` opened; no data payload; nothing dated 2024-01-01 or later; no 2020-2023 return,
IC or NAV output beyond progress.md's public lines; `atx-db/` not touched. Cross-lane edits: none.
