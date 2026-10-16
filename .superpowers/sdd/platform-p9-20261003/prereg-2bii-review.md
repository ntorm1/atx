# prereg-2bii-review.md — GATE-REV part 2 (round 2b-ii review of `p9-prereg.md`)

- Reviewer: GATE-REV (fresh; PM subagent). Date 2026-10-16. Written at OWNER STOP 13; the Part 2 checks below were done
  before the stop (DONE, not partial).
- Object: `p9-prereg.md` @ `a40db317` (2441 lines; parts A `7890883e`, B1 `aef2e033`, B2a `1e2dbf3c`, B2b `a40db317`).
- Blind: no 2020-2023 return / IC / Sharpe / turnover / NAV statistic read or written; the prereg was read through a
  number mask. No build, no run, no test re-run. Nothing of root's edited.
- Scope: `r12-post-gate.md` "GATE-REV" Part 2, plus the RESUME 13 ruling list in the PM's part-2 message.

**Verdict: CHANGES_REQUIRED** (one Required finding, RQ-1).

## 1. Placeholder audit (`[ROUND 2b-ii: ...]`)

Every round-2b-ii placeholder outside the open-by-design set is filled, and each value names its source. Checked by
path / SHA at pool-2 HEAD:

| Value | Prereg | Source checked | Holds |
|---|---|---|---|
| Template SHA-256: info-clock `95d4104a`, mom-volman `7e9b26c1`, mv-aim `83391f2c`, theme-cov-hedge `7c6778bb` | B.4-B.8 | `scripts/specs/p9/templates/*.json`, re-hashed | yes |
| Rule SHA-256: info-clock-rate-v1 `7801c198` (`51b8b754` at `858a4fca`), mom-volman-v1 `8a7222fa`, mv-aim-v1 `4b4af6e2` (also at `5c811b56`), theme-cov-hedge-v1 `55f25f2f` | B.4-B.8 | `scripts/specs/p9/rules/*.json`, re-hashed (and at the named merges) | yes |
| kinds/walk-forward `d865201a`, od3 flagged_members `9aa6fb59`, q5 diagnostic `721889d1` | B.6, F.1, C | re-hashed | yes |
| Generators: `wave_kinds.py` blob `d282586a` / sha256 `9268e222`; `wave_stage_kinds.py` `694b619e` / `800b4080` | A.4, B.0 | `git ls-tree` + re-hash | yes |
| Merges cited (f6444417, 9d518317, 08f04e07, 858a4fca, 1d003241, 5c811b56, 0a2ceb2b, ad913bd5, 338e6a7c, cdc753a8, 898bf120, 7f6f1c75, a2cf7e78, 430c29e8, b182dfdd, bba4c06f, 6d47b28d, 22caee08, d51d011e) | throughout | `git merge-base --is-ancestor` each to HEAD | all in HEAD |
| Release exes p9-3f: NAV `255b58ac`, IC `ccd54db6`, eval `e2b48c46`; re-pin of record `2078f877` (code head `f8a85ad4`, `3a1a4759` reverted) | B.0 :489-509 | gate records, REPIN-REV | yes |
| Store pins v11 (manifest `397366473b3a...`, root `b39a1b77...`, model `cd48d574...`) and covv1 (`ea58691f...`, `20919f7b...`, `0f90bf7c...`) | B.5, B.7, B.8 | `handoff-root-m2i.md` "Store pins" | equal |

Open by design (not findings): registration-commit placeholders 7, 10, 11, 13, 19, 21 and the registration parts of
16-18; cell-time items (each pair's rho under B2-13, the G-B7 P9 steps). Item 24 carries no part tag (noted; not a
finding).

A mechanical sweep of backticked path tokens finds bare file names (e.g. `summary.json`, `nav_summ.py`) that resolve
relative to a named directory or a `<sha>:path` form, not to the sprint dir; none is a broken source on reading.

## 2. Lapse table B.10 (`p9-prereg.md:1408-1442`)

B.10 equals LAPSE-TABLE and agrees with the gate report: P9-L, P9-H, P9-M, P9-C, P9-MV, P9-V run; P9-W LAPSED (0)
(D3-Q1a DIFFERENT, gate step 3); P9-X pending PRE-30 / PRE30-MVX (B.7). PRE-14, PRE-35, COMPOSE-1 (COMPOSE-1a ACCEPT),
PRE-MVX applied as ruled. Holds.

## 3. Argv final, per cell

| Cell | Parent named | Template / hand-spec SHA | `--seal-allow` (SEAL-REF) | `--ann-mean-gross` alike | `--info-clock-print` alike | No cache index flag (SQL-11) | `--risk-model` only P9-MV / X / V | Store pins = m2i |
|---|---|---|---|---|---|---|---|---|
| P9-L (hand v1, B.1) | yes | registration-commit (open) | yes | **stated, no route** (RQ-1) | n/a | yes | yes (none) | yes |
| P9-H (B.2) | yes | pinned | yes | **no route** (RQ-1) | n/a | yes | yes (none) | yes |
| P9-M (B.3) | yes | pinned | yes | **no route** (RQ-1) | n/a | yes | yes (none) | yes |
| P9-C (B.4) | yes | pinned `95d4104a` | yes | **no route** (RQ-1) | **claimed inherited, no route** (RQ-1) | yes | yes (none) | yes |
| P9-MV (B.5) | yes | pinned `83391f2c` | yes | **no route** (RQ-1) | n/a | yes | yes (allowed) | yes |
| P9-W (B.6) | lapsed (0) | - | - | - | - | - | - | - |
| P9-X (hand v1, B.7) | yes | registration-commit (open) | yes | stated, no route (RQ-1) | n/a | yes | yes (allowed) | yes |
| P9-V (hand v1, B.8) | yes | registration-commit (open) | yes | stated, no route (RQ-1) | n/a | yes | yes (allowed) | yes |

## 4. Rulings (RESUME 13)

| Ruling | Where | Holds |
|---|---|---|
| LAPSE-TABLE | B.10 | yes |
| FIELDS-GONE / FIELDS-SUB / FIELDS-SUB-A / FIELDS-X5 | A.4, F.1 | yes |
| PRE30-MVX, PRE34-WHO | B.7 | yes |
| MVX-F0H / MVX-PRE13 | A.4, B.7, B.8, E | yes |
| COVMV-PIN / COVMV-PIN-2 | B.5 | yes |
| B8A-PROD / B8A-PROD-2 | B.8 (a) | yes |
| PW-LAPSE-PRINT | B.6, F.1, E | yes in the prereg (see SG-2 for the runbook) |
| SEAL-REF | B.0 :510-518, B.1 | yes |
| REPIN-REV, FIELDS-RISK | A.4, C.15 :1571-1581, record | yes; F.1 :1783-1785 stale wording (SG-1) |
| F1-FIELDS | F.1 | yes |
| D3Q3-ARGV / D3W-TPL / D3W-V3READ | B.6 | yes (moot under the lapse, recorded) |
| COMPOSE-PRE / COMPOSE-1a | B.5 | yes |
| GP-TICKS | as ruled | yes |
| TEST-2 | B.0 "Lane suites" :556-576 | yes |
| PRE-2BI-REST | header, "Rulings applied" :2267-2308 | yes |

## 5. G-B7 step table (E, `p9-prereg.md:1685-1715`)

Complete: every step named with its input, its owner and its cell-time open items. The "Memmel SE" question of E,
answered from the v8 driver of record: the v8 verdict reads the SE the NAV summary computes, not a re-derivation.
`scripts/cycle_verdict.py` maps `"se": p.get("memmel_se")` (at `fa61c67f`:57, `fc802825`:65, `337ed421`:70,
`0aa411a9`:71, `e5c219d6`:76); `atx-impl/tools/nav_summ.py` @ `44a27d5d` defines `memmel_se(sr_a, sr_b, rho, t)` at
:329, writes it into the bundle at :669-670 and prints `Memmel SE` at :844. The prereg's answer agrees. Holds.

## 6. OD-3 runbook F.1 vs B.10

The prereg's F.1 (:1738-1877) is consistent with B.10: P9-F0h as registered; P9-W lapsed with its PW-LAPSE-PRINT
print in place of the dSR-based prediction. The separate `p9-od3-runbook.md` is not (SG-2).

## Required

- **RQ-1. No route puts `--ann-mean-gross` (every cell) or `--info-clock-print` (P9-C) onto a driver-built cell NAV
  argv; as registered every wave cell is VOID under B.0 void rule (2).**
  `p9-prereg.md:519-527` (PRE-7a: every cell and its reference carry `--ann-mean-gross` alike), `:581-582` (void (2)),
  `:606-607` (P9-L "carried alike"), `:842-845` (P9-C: "inherited from the parent's NAV flags"), `:2439-2441` (record:
  "every cell carries alike" with no route named). The driver builds the NAV argv only from the resolved spec's nav
  flags (`scripts/cycle/phases.py:824`). The parent spec `scripts/specs/v8/lib-v8ysb-gm.json` (sha256 `3631679c`)
  nav.flags carry neither flag; the four templates have `parent` null and `set`-only changes; the four rule
  manifest_fragment `rule_cell`s carry no `constants`. `info-clock-rate-v1.json:53` says the cell "inherits" the flag
  from root's Y-F0 reference, but that reference was an exe-direct run with the flag appended, not a spec flag. No
  file under `scripts/` or `atx-impl/tools/` names either flag in the driver path (git grep). The P9-B0 references
  carry both, so cell and reference differ. The P9-C parent that is an accepted P9 cell fails alike, and so do the
  OD-3 turnover-share books.
  Fix (PM's choice): (a) each manifest's `rule_cell.constants.flags` names `--ann-mean-gross` (and P9-C's
  `--info-clock-print`; both unowned, so allowed by the E2-8 guard, per `scripts/tests/test_info_clock_rule.py`), and
  the hand v1 specs (P9-L, P9-X, P9-V) name both in their nav flags; or (b) a ruled spec commit adding both to
  `lib-v8ysb-gm.json` nav.flags, with the re-pin re-verified. Then name the route in B.0 :519-527, B.4 :842-845 and
  the record :2439-2441.

## Suggested

- **SG-1.** `p9-prereg.md:1783-1785`: FIELDS-RISK "not ruled; stated for the PM" is now ruled (OD3-FIELDS-RISK,
  `progress.md:1008`: a refusal at F.1 B6 is an F.1 stop condition; moot if OD3-DB is silent). Cite it.
- **SG-2.** `p9-od3-runbook.md:220-226` (last commit `d99a12bb`, pre-gate) still states the P9-F0h prediction through
  P9-W's dSR and lacks the PW-LAPSE-PRINT note; :222's "with `--info-clock-print` on both books" inherits RQ-1. Note in
  the runbook that the prereg's F.1 governs, or update it.
- **SG-3.** Line citations into `65ce28f0`-era files are stale (disclosed in the prereg); refresh at the registration
  commit.

## Open questions (for the PM)

- RQ-1 route (a) or (b): (b) changes a re-pinned spec and needs a ruling plus re-verify; (a) keeps the parent fixed.
- Item 24 has no part tag: assign one or confirm it is untagged by design.
