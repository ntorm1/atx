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

## RQ-1 re-check (FLAGS-ROUTE), 2026-10-16

- Reviewer: a fresh scoped RQ-1 re-check reviewer (REV-FRESH; FLAGS-SEQ (3)); dispatch `r14-prompts/rev-flags-prompt.md`;
  the sections above were read first and stand as written. Only RQ-1 is re-checked.
- Heads (verified by `git log`): pool-2 HEAD `ef849874`; spec commit `f8d852f3` = code head; root docs `5648284f`,
  `1a14a24a`, `510a768a`, `5ec208b4`; PRE commits `bc034c4a` (SG-1), `cb051169` (SG-2), `ef849874` (part B3);
  `p9-prereg.md` at HEAD and in the working tree = `ef849874`. The re-pin of record stays `2078f877`.
- Blind: no 2020-2023 return / IC / Sharpe / turnover / NAV statistic read or written. Records, the prereg and the
  runbook were read through a decimal mask. NAV outputs were compared by SHA-256 and JSON path only. No build, no
  real-data run. One pytest file was run (check 2; own basetemp, deleted by name). Nothing of root's or the PRE agent's
  was edited. Trial ledger: 133 lines, sha256 `27e40f9f...`. `research-store/` and `index.sqlite` absent.

| # | claimed | evidence | checked | holds |
|---|---|---|---|---|
| 1 | Spec diff: one file; `/nav/flags` += the two tokens only; SHA-256 `3631679c` -> `a4ef6c04`; no other code moved; exes untouched | `git show f8d852f3 --stat`; `json.load` of the file at `f8d852f3^` and `f8d852f3`; `sha256sum`; `git diff 2078f877 HEAD -- . ':(exclude).superpowers' ':(exclude)scripts/specs/v8/lib-v8ysb-gm.json'` | 1 file, +3 / -1 (blob `9dd63a00` -> `f0c3acd2`). JSON path diff: one path, `/nav/flags`, 25 -> 27 tokens. new[:-2] == old; the tail is `--ann-mean-gross`, `--info-clock-print`; equal outside `/nav/flags`; key order unchanged. `f8d852f3^` bytes = `3631679c...edea`. Disk = blob at `f8d852f3` = HEAD = `a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a`. Excluded diff: 0 bytes. Release NAV `255b58ac...3565`, IC `ccd54db6...d705`, eval `e2b48c46...962c` = `handoff-root-gate.md:39-41` | yes |
| 2 | The route: X-10 / Y-1 inherit `nav.flags`; templates have `parent` null and `set` only; the driver builds the argv from `nav["flags"]`; the lane test shows the flag once | the four spec files parsed; `scripts/research_spec.py`; `scripts/cycle/phases.py`; `scripts/tests/test_info_clock_rule.py` | X-10: no `nav` key; `change` = `set` {`nav.leverage`, `nav.output`}. Y-1: `change.set` plus `change.flags.nav` {`--vol-target`, `--risk-model`, `--risk-model-sha256`}, no `unset`. `resolve` deep-copies the parent and `apply_flags` appends absent options (`research_spec.py:176-206`, `:128-163`), so neither replaces the list. Templates `info-clock` / `mom-volman` / `mv-aim` / `theme-cov-hedge`: `parent` null, `change` keys `[set]` only, no `nav`; SHA-256 `95d4104a` / `7e9b26c1` / `83391f2c` / `7c6778bb` = the prereg's pins. `phases.py:824` `flags = [... for f in nav["flags"]]`; `:829-832` argv `..., "--rule", nav["rule"], *flags, *label`. Test `:217`, `:222` `count("--info-clock-print") == (1 if parent_flags else 0)`. Re-run: 5 passed (explicit path, `-p no:cacheprovider`, `PYTHONDONTWRITEBYTECODE=1`; no build or bounded run of ours in flight; the pool-3 build and the pool-34 benches are foreign, HOST-6a). `flags-plan.txt:31-32`: X-10 and Y-1 plan each token x1 | yes |
| 3 | `verify` and the fixture: only the expected lines differ; X-10 / Y-1 HOLDS; the FIELDS-RISK REFUSED line unchanged; fixture `a50fdcf9` twice, `--pins 0`, identity 12 / 0 | my raw `diff repin-verify-3.txt flags-verify.txt` (72 / 72 lines); `fixture-chain-7.txt` vs `-6.txt` | 5 lines differ. :1 is the header (head, timestamp, the edit). :4 and :33 are the X-10 / Y-1 spec digest over the template chain (`ea5c4578...`, `41360ef2...`); their file SHA-256 (`46e9d6c2`, `f4186573`) is unchanged. :27 and :57 are the planned NAV argv sha256 (`40ff035d...`, `9dd016ff...` = `flags-plan.txt:31-32`, the two tokens). The dispatch says "three specs", but the parent alone prints no digest: it refuses first (:63 REFUSED, FIELDS-RISK, unchanged; :72 `tool exit 1` unchanged). :30 X-10 HOLDS, :60 Y-1 HOLDS. Fixture: `a50fdcf9c9c83bdac0d41ff368666da31578ebb4407bbc4498175a63014f100c` twice (= `fixture-chain-6.txt`), files 28 / 25 / 0 / 4, `verify --pins 0`, identity "total checked 12 mismatches 0" | yes |
| 4 | The identity: the argv differs from the reference only at the output and `--combined` path tokens (same `--combined-sha256`); a bounded run; outputs identical outside `/producer/*`; the reference untouched | `gate-logs/receipt-p9-fr-yf0-nav.json` (= the live `build-equity/p9-fr-yf0-nav-run/receipt.json`) vs `receipt-p9-b0g-yf0-nav.json`; `start.json` keys; my own SHA-256 re-hash of both dirs, file by file; a path-only JSON diff of the two `summary.json` | 49 / 49 tokens. By option: the same 26 option / value pairs, with values differing only at `--combined` (path) and `--output`. `--combined-sha256` `e13fbc4d...` on both, and both `train_combined.json` re-hash to it. Positionally, [43]-[48] are the same six tokens in another order (the label pair after `*flags`): ruled FLAGS-ORDER, ACCEPTED. Receipt: schema `atx.bounded-research-run/v1` (`run_bounded_research.py:480`), limits `seconds` 600, `max_rss_mib` 2048, `exit_code` 0, outcome completed, exe `255b58ac`. No `store` key in the receipt or `start.json`; no cache / cache-index token. `argv_sha256` `c0d90758...` = `research_tree.argv_sha256(command[1:])` = `flags-plan.txt:7`. Re-hash: 27 / 27 files byte-identical, `summary.json` `826d5ec6...8f38` on both. The dispatch expected 26 / 27: both sides are the same Release exe p9-3f, so no `/producer/*` leaf differs. Path diff of the summaries: 0 differing paths, so 0 outside `/producer/*`. Leaf paths counted, values not read: 3 `/producer/*`, 5 `ann_mean_gross`, 70 `info_clock` on each side. The reference listing (`e205d6ec...f797`) = the committed `b0-files-p9-b0g-yf0-nav.txt` byte for byte, and the new dir's listing equals it | yes |
| 5 | The prereg names the route beside each standing line (PRE-40 form); SG-1 and SG-2 applied; the header lists the edits; the new SHA sits beside every `3631679c` | `p9-prereg.md` @ `ef849874` (masked); `git diff a40db317 ef849874`; `p9-od3-runbook.md` | Brackets: B.0 PRE-7a `:594-605`; B.0 exes `:567-570` (new SHA beside `3631679c`); B.0 Void (2) `:660-669` (FLAGS-ROUTE-2 (a) / (b), FLAGS-ORDER); B.1 `:699-703`, `:706-708`; B.4 `:937-938`; B.7 `:1289-1294`; B.8 `:1364-1368`; record `:2562-2569`. Header `:188-225`, status `:4-5`. Diff: +142 / -14 (`bc034c4a` +3 / -1, `ef849874` +139 / -13). Each of the 14 removed lines survives as a re-wrap: 9 as verbatim word sequences, 1 re-punctuated (old `:2081`), and 4 with a bracket inserted between their words (old `:504`, `:582`, `:614`, `:843`, each read). No standing line is deleted. SG-1: F.1 `:1895-1897`. SG-2: runbook `:15` ("F.1 governs") and the note at `:230-234` (`cb051169`, +9 / -0). `3631679c` citations, with the new SHA: `:192` (`:193`), `:399` (`:402`), `:569` (`:568`), `:706` (`:707`), `:1688` (same line), `:2193` (`:2194`), `:2301` (`:2302`), and `:2470` (a table row; the new SHA is in the bracket beneath it, `:2475`). `:204`, `:210` and `:215` describe the edits and are not citations. None lacks it | yes (SG-4) |
| 6 | No 2020-2023 statistic in root's FLAGS section, the handoff amendment, the B3 edits or the runbook notes | a decimal and keyword-number scan of the added lines (values masked) | B3 + SG-1 (142 added lines): 0 decimals; the 2 keyword hits are "exit 0" and "27 / 27 files". Runbook (9): 0. Root's records (`root-gate-report.md:1138-1342`, `handoff-root-gate.md:166-200`, the `integration-log.md` row; 248 added lines): the decimals are host RAM / disk, wall time, MB, the `ucrtbase` version, and spec configuration constants (leverage, trade fraction, dust multiple, turnover caps, exit rate). Root's new gate-log texts (plan, NavSpec prints, runner log, ucrt) hold the same kinds. No statistic | yes |

**Required.** None.

**Suggested.**

- **SG-4.** The text says every P9-B0 reference carries both flags at `p9-prereg.md:598` ("the P9-B0 references carry
  both by hand on their exe-direct argv"), at `:2565-2566` ("the references carry both by hand") and at
  `p9-od3-runbook.md:233-234`. In fact only `p9-b0g-yf0-nav` carries `--info-clock-print`: the receipts' `command`
  of `p9-b0-r2-nav`, `p9-b0g-x10-nav` and `p9-b0g-y1-nav` names `--ann-mean-gross` only. The governing text is exact
  (Void (2) `:660-669`, B.7 `:1291-1292`, B.8 `:1366-1367`), so no cell's void status moves. Fix at the registration
  commit, PRE-40 form: "the Y-F0 reference carries both; R-2 / X-10 / Y-1 carry `--ann-mean-gross` only" (the precision
  of GATE-REV S-5).

**Noted (not findings).** The identity is 27 / 27, not the 26 / 27 the dispatch expected. Root disclosed the token
order difference and the PM ruled it (FLAGS-ORDER). Root states that the X-10 / Y-1 reference specs now plan
`--info-clock-print` while their exe-direct dirs do not (`root-gate-report.md:1177-1180`); FLAGS-ROUTE-2 (b) covers this.
No permission refusal.

RQ-1: CLOSED (APPROVE)
