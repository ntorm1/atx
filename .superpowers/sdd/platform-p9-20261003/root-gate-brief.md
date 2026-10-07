# Root brief: the single wave-2 + wave-3 gate (ruling GATE-1; PM draft 2026-10-06, finalised at dispatch)

Root = `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. One fresh root agent (or a chain of them, each from the
previous one's `handoff-root-gate-<n>.md`). Read first: `resume4-dispatch.md`, `resume6-addendum.md`,
`resume7-addendum.md`, `resume8-addendum.md`, `r7-root-merge.md`, the newest `handoff-root-*.md`, plan section 4.2
(`docs/plans/2026-10-03-p9-sprint-plan.md:850-861`) and 4.4, `p9-prereg.md` sections A.4, B.0, C (the standing lines),
Appendix IV (what the gate delivers to round 2b-ii), and by grep in `progress.md`: GATE-1, EXE-FREEZE, EXE-CUR, W1-I2,
PRE-22, PRE-29, PRE-30, PRE-34, B2-13, M2c-RED-2, M2A-4, ALCLOCK-1, PRE-7a, C3-6, C3-9, ROOT-1, ENV-1, PERM-1, RUN-1,
RUN-2, SEAL-ALLOW-3, CRT-2, COV-14, OD5-1, OD5-6. The PM's dispatch prompt names the gate head (the head with every
merge in), the lanes merged, the stretch lanes that did NOT make it (P10 whole; nothing of them is in the head), and
the next free build tag. 0 trials throughout; the trial ledger stays 133 lines `27e40f9f` until the PM starts a cell.

## 0. Start state

Head = the PM's gate head; clean tree (only the PM's files and the four PNGs); `git log --format=%s -40` shows every
named merge; ledger 133 / `27e40f9f`; disk and RAM noted; `ucrtbase.dll` version + SHA-256 recorded (M2c-RED-2 (c)).

## 1. Exes of record (EXE-CUR, EXE-FREEZE)

One target-scoped Release build (`equity-rel`, `-Canary`) and one Debug build (`equity-dev`) of the full exe set at the
gate head (NAV `atx-equity-strategy-targets`, IC `atx-equity-strategy-ic`, `atx-equity-strategy`, `-risk`, `-mine`,
`atx-research-eval`, `atx-research-composition`, `atx-research-store`, `atx-impl`, every test target the suites need),
0 warnings; receipts with tag, SHA-256 and source commit per exe (the table of `handoff-root-m2j-fin.md` as the form).
Release is the producer of record (OD5-2). From the re-pin (step 6) on, these exes are NOT rebuilt until the freeze
gate (EXE-FREEZE); if a rebuild becomes unavoidable, P9-B0 and the re-pin are redone first and no cell runs between.

## 2. Suites (plan 4.2 "suites"; 0 failed, named skips only)

Debug and Release: `atx-impl-strategy-ic-tests`, `atx-impl-strategy-target-tests`, `atx-impl-tests`, the engine alpha /
factory / book groups, `atx-engine-research-fields-tests`, every research test target (`ctest -L atx_research`,
`-L atx_equity_strategy` through the wrapper), `atx-engine-risk-tests` (`RiskCovContainer.*`, `Risk*` minus the
31-minute oracle), the `Cov*` suites; `atx-equity-strategy-risk cov-diff --legacy` on the store of record exits 0.
Pytest, separate invocations: `scripts/tests` under PYTHONHASHSEED 0 and 1, `atx-engine/tools`
(`test_stage_chain.py` on its own), `atx-impl/tools`, `atx-impl/strategies`. Known-red set EMPTY; the Release
`AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` named skip stays (OD5-2). ENV-1 for exit 3221225794.

## 3. Identity and canaries (plan 4.2 "identity")

Golden `0x889874a3b9b29c55` at 1 and 4 `--book-workers`; the tiny-world canaries on Debug and Release goldens
(C3-14 for a wall-time-only miss). Flag-absent identity of the current parents (X-5 reference `p9-c1-s2-x5-nav`, M2J-2;
Y-F0, R-2) with every new flag absent: u / fit / w / NAV byte-identical outside `/producer` on the Release exes of
step 1, by SHA and JSON path only (bounded runner `--seconds` <= 600, `--max-rss-mib 2048`; compare tools in
`m2g-logs/tools/nav_cmp.py`, `m2j-logs/tools/optin_cmp.py`); any difference is attributed by merge commit (ROOT-1 at
every merge gives the per-lane attribution) and stops the gate (report BLOCKED; the PM returns the lane).

## 4. P9-B0 (ONE run; PRE-22, PRE-29, PRE-30, PRE-34)

Substitution list written and committed FIRST (which reference dirs are replaced, by which runs, and why). Then, on
the Release exes: R-2 re-based (NAV), Y-F0 re-based (u, w, NAV), Y-1 re-based on the Release v1.1 container store
`build-equity/p9-m2i-dcov-v11` (PRE-29; P9-X's cell of record, PRE-30 / PRE-34); `--ann-mean-gross` on every reference
(PRE-7a, C3-9); `--info-clock-print` on Y-F0 and its pair iff AL-CLOCK is merged and APPROVE (ALCLOCK-1); every run
through `scripts/run_bounded_research.py`, synthetic nothing, `holdout_gate.py` never, open no data dated 2024-01-01
or later. Record per reference: dir, SHA-256 of every output file, the exe table, `ucrtbase.dll` (C.6), the
identity-bridge flag line (OD5-1). READ NO STATISTIC (the PM's reads come later through the registered prints); the
comparison of the re-based Y-1 against v8's ledgered Y-1 (PRE-34's "provided they agree with the ledger" clause) is
done by SHA / JSON-path diff of the verdict blocks and reported as "agree / differ at <paths>", not as numbers.

## 5. rho of the P9 pairs (B2-13, C.11)

rho of the P9-X pair (re-based Y-1 vs re-based Y-F0) and the P9-V pair references, and of the leverage-only pairs
measured at M2g if their references changed; printed to `root-gate-report.md` as the value and the "1 - rho < 1e-6"
flag only (a correlation is not a 2020-2023 return statistic under B2-13; no Sharpe / return is read).

## 6. The re-pin (W1-I2 (b); one commit)

The lineage parent specs re-pinned to the P9-B0 references, incl. `exes.eval` and every exe's SHA-256 of step 1
(B2-10 (b), B2-11); `verify` on the pinned lineage; the fixture chain digest; `git diff` of the re-pin commit lists
only pins. EXE-FREEZE starts here.

## 7. Store step (M2A-4), scoreboard, timings, G-P ticks, CRT record

`atx-research-store catalog` on the gate head as a bounded root run (catalog digest; `verify --pins` counts by state;
render-identity mismatch count; a second from-scratch catalog reproduces the digest; the X-5 opt-in cache identity);
`research_cycle.py scoreboard` re-reads every ledger line and reproduces the accepted lineage; `scoreboard --timings`
appended to `integration-log.md`; the G-P rows (G-P1 .. G-P10) ticked with evidence rows; the CRT record (CRT-2: the two
pinned D-COV stores' receipts and exe hashes, never rebuilt).

## 8. Whole-wave review (GATE-1), split by area among fresh reviewers (the PM dispatches them on your report)

Your report names the wave-2 + wave-3 diff range (`<wave-1 gate head>..<gate head>`) and the areas: NAV / book C++
(C3, AL-CLOCK, COV-MV, B2's NAV side), IC / composition C++ (D3W, ABS, part B), Python wave / cycle (B2, C3b, D3, E2
follow-ups, SQL3, OD3P), store / catalog (SQL3), fields (A4, T2). The PM dispatches the reviewers; I / M findings are
fixed by root as slips or returned to lanes before any cell.

## 9. Records and hand-back

`root-gate-report.md` (every item with its evidence path), `integration-log.md` rows, the round-2b-ii facts for the
prereg by Appendix IV item number (1-6, 8, 9, 12, 15, 20, 22, 23, 25, 26 and whichever the lanes' "For PRE" blocks
leave to root), `handoff-root-gate.md` (code head, docs head, the exe table, the reference dirs, known-red set, next
free tag, what the cells agent must know). Code and docs in separate commits; trailer as the contract. Final message
under 200 words: STATUS, head SHAs, build tags, per step holds / fails, any permission refusal, open questions.
