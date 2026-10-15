# v8 OD-3 protocol: the history read (2013-2019)

Ruled on 2026-10-02 as PM7-29, which adopts all 12 choices of section 4 and adds one condition to choice 11. This is the
lane XPRE text and was written before any read. It uses documents, source, manifests and receipts only. No history
byte, return, IC or Sharpe has been opened, and root was not changed. No line of this file changes before the read. The
tooling the read needs (T1-T5) is built in commit `f68058e2`. Section 5 is the integrator's runbook.

## 1. What the registration says (quoted)

- **Plan, OD table:** "OD-3 | Backward history extension to 2013-2019, read once on the frozen book | open | not run
  in v8 without a ruling. Track H builds the tooling only". **D16:** "10 years cut standard errors by nearly half".
- **Plan, H-1:** "Roles E1 2014-2016 and E2 2017-2019, each with a 399-session warm-up and today's geometry. The 4-year
  role is E3." "E3 stays byte-identical." Step 3: "data audit without returns: coverage of every field by year
  2014-2019, share of delisted names present, first valid date per signal. The audit opens no return." "**If OD-3 is
  granted:** one pre-registered read of frozen V8-F and of B0c on E1 + E2, with the registered expectation that Sharpe
  is lower than on 2020-2023. The read costs 1 trial."
- **Plan, V8-F step 3** (also `task-V8-F-brief.md`): "if the DSR gate is unmet, the scorecard says so and names OD-3
  (history) as the lever." **Risk table:** "The freeze gate stays unmet | high | no frozen book | stated in advance;
  OD-3 is the lever".
- **Plan, power table 12.2, "about 10 years":** SE of annual Sharpe .30; paired dSR SE at rho .98 .061; "smallest
  detectable paired gain, one-sided 5%" .10.
- **`v8-prereg.md`** (no OD-3 line).
  - Rule 2: "Re-runs of ledgered cells on the longer window add 0."
  - Rule 4: "Studentized circular block bootstrap, block 21 sessions, seed 20260929, 4,999 resamples".
  - Rule 7: "A rerun decided without seeing returns replaces it with no new trial. A rerun decided because the returns
    looked wrong is a new trial."
  - Rule 9: "Freeze gate: S2 net >= 1.0 AND mechanics AND cumulative paired dSR (V8-F vs B0c) > 0 with bootstrap
    p < .10 AND cell-count DSR >= .95". Plan V8-F step 2 places its Sharpe leg "on 2020-2023".
- **E-2:** "OD-3 (history read 2013-2019) and OD-7 (mining campaign) keep the plan default "tooling only, no read, no
  campaign" ... a read cannot be undone".
- **E-17:** ""frozen book" for a history read means frozen rules with pooled re-admission (Python tooling as
  designed); no runner change in v8; ... rebinding E3 weights to another role would defeat the role binding -- cost if
  wrong: the owner wants frozen weights and a small runner change follows."
- **E-34:** "the freeze gate's "bootstrap p < .10" is one-sided ... the scorecard prints both p values."
- **E-35:** "the pooled (era) fit must support `ew-theme-std-v1` and `ew-theme-std-aim-v1` before any OD-3 history
  read".
- **E-41:** "an OD-3 history read, one era or pooled, adds 0 to the construction N ... and 1 to a separate "history
  reads" count printed in the Appendix A block ...; the task-H brief's "costs 1 trial" is read as this count".
- **PM7-27:** "OD-3 is now open, under a protocol written and ruled before the read (PM7-28)"; "any read of the hidden
  block (2024-01-01 or later) stays closed, and nothing under `atx-db/` is written".
- **PM7-28:** "the history is read once, after the last X cell, on books fixed before the read: B0c, R-2 and the
  accepted X book (if any). No cell, signal, weight or constant is chosen, changed or re-fitted after the read; the
  history is a test window, not a tuning window. It answers the gate's two statistics on the longer sample and nothing
  else."
- **PM7-29 (this protocol, ruled):**
  - "TRAIN admission, signs and weights are carried back unchanged; no re-admission, no re-fit on history (overrides
    E-17's pooled re-admission for this read)".
  - "Data builds, coverage and mechanics checks on the history may run while X runs; no return, IC or NAV number of
    the history is opened until the last X cell is ledgered."
  - "The deflated-Sharpe part of the gate is expected to stay unmet (power note above); the read can settle the
    p-value part only. Whatever it shows is printed; a history result that is worse than TRAIN is the result".
- **Other records:**
  - progress.md H1: "Whether B0c's era run is ledgered is a PM decision at OD-3 time."
  - H-1 report: "Seal: every end is 2020-01-01 or earlier".
  - `v8x-prereg.md` section 10: "only on a frozen book (V8-F or X-F) after the X gate, never to choose among X items".
- **Code:**
  - `holdout_gate.py` reads only 2024 and later (OD-1).
  - `research_window.json` has no history key.
  - `era_pool.check_windows` refuses any era past the seal.
  - `history_read_lines` implements E-41.

## 2. Feasibility (from manifests, source and receipts; nothing built or read)

| input (TRAIN lo3 pin) | first usable | consequence |
|---|---|---|
| TickerHistory3 `0ed96b26` (prices, vendor shares, IV, earnFlag) | 2012-03-26 | price and base roles; warm-up from mid-2013 |
| fundamental-events-v3, `EMIT_FROM = dt.date(2014, 6, 1)` (FSDS 2009q2+ only updates the state) | ~2014-08 | 30 issuer fields are NaN before |
| atx-db `sic_events` `9f9b2f85` | "2014 on" | names drop out of lo3 as `no_visible_sic`; grp_* NaN |
| identity-bridge-v2-pit `09aac28f` | unverified | lo3 link test (check P3) |
| delisting stage `1b1166b6` | "2018 on" | no lo3 termination marks and no delisting returns before 2018 |
| FINRA SI; CNMS short volume (2018-08-01); ftd; short_volume_ext | 2018 | short-interest theme NaN; sv_ratio126 from ~2018-11 |
| 13F / earnings_calendar, sec_filings / insider | 2013q2 / 2009 / 2015q1 | inst_* from ~2013-08; ea_* fine; `ins_opportunistic_net` "NaN ... before 2018" |

**Roster.** The v8.0 library has 52 members; R-2's admitted subset is in its admission.json, which this lane did not
open. Members by the date they become usable:
- From the warm-up (14): issuance_vendor, ear, res_mom_12_1, mom_12_1, high_52w, bac, smax, smax5,
  seasonality_same_month, iv_rv_spread, fip_id, ea_overdue, ear_mom_12m, inst_best_ideas.
- From the first visible SIC in 2014 (4): ind_mom_12_1, within_ind_mom, ind_adj_rev_5, res_mom_ind.
- From about 2014-08, plus each member's decay window (26 issuer members): 9 value, 10 profitability and 4 investment
  members, plus qmj_safety, nincr and earn_surprise_comp.
- From 2018 (8): si_ratio, dtc, si_change, dtc_slow, si_low_io, sv_flow, ftd_fail, ins_opp.

Each member needs its K1 `required_lookback` after its first source date. That lookback fits the 399-session geometry,
which TRAIN already meets. Check P4 prints each candidate's first valid date.

**NAV.**
- The S2 cost (`modeled-1bn-stale5-v1`) reads TH3 only.
- `swap-fin-v1` tiers need `si_shares`, and `borrow_predictors` marks a non-finite value "warm, counted as missing".
  Before 2018 every name is therefore warm, and `--locate-in-aim` never blocks a short.
- `price-risk-v1` reads prices only. aim-partial-v5 reads no risk store.

**Members missing.** No fallback is registered. The present-member rule applies as coded
(`strategy_ic_composition.cpp`: "a cell with a present member adds W_theme times the rank"; NaN where none is present).
- Before 2018 the short-interest theme drops out.
- ownership_flow runs on inst_best_ideas alone.
- The NAV scales the book to L.
- B0c (v7.1) holds the same short-interest members, so the comparison is like for like.

**2013 cannot be scored.** The 399-session warm-up from TH3's first day ends about 2013-10, and lo3 has no SIC and no
fundamentals before 2014.

**Tooling** (built in `f68058e2`: Python only, synthetic tests, every flag absent byte-identical):
- **T1 `fit.carry`:** `research_roles.py` plus `atx-impl/tools/carry_weights.py`.
- **T2 `nav_summ --pool-one-sided`:** the pooled one-sided p and its verdict.
- **T3 `nav_summ --dsr-total` with `--pool`:** a history pool adds no trial.
- **T4 `roles[].label_dir`:** per-era label roles. A spec-level `inputs.label_role` is refused with two or more roles.
- **T5:** a pool whose E3 is already a ledgered TRAIN cell adds nothing.

**Power, stated before the read.** TRAIN figures are scaled by sqrt(1006/T), with pooled T = 2,264.
- The paired SE is about .139. A one-sided p < .10 needs a pooled dSR of at least .178, which means a history dSR of
  at least .22 (TRAIN: +.123).
- DSR >= .95 at N 50 and V 1.297e-3 needs a pooled annual Sharpe of at least 1.85, which means a history Sharpe of at
  least 2.33. That is out of reach.

## 3. The protocol

**3.1 Books.** Each book is frozen by its pins. Root writes them into progress.md and the R0 line before any era u pass.

| book | spec | library | admission / composition | L | NAV |
|---|---|---|---|---|---|
| B0c | `base-b0c.json` (parent `base-lo3.json`) | v7.1 `787c802e` | B0b `29357ece` / ew-theme-v1 | 1.247 | dlret labels, warm start 60 |
| R-2 | `lib-v80.json` | v8.0 `68ce8539`, recipe `fb09b740` | v4-prior-v1 / ew-theme-std-v1 | 1.1474 | aim-partial-v5 |
| X-F | the last accepted X cell when X-10 is ledgered (none if none) | own | own | own | own |

The pins also include each book's TRAIN admission.json and composition_weights.json SHAs, the build tag, the exe SHAs
and this file's SHA. Admission, signs and weights are carried back (4.1). ew-theme(-std) weights are 1/(T n_theme) on
the admitted roster, so the roster is carried too.

**3.2 Roles.**
- E1: warm-up from about 2013-06, scored [2015-01-01, 2017-01-01), 504 sessions.
- E2: warm-up from 2015-06, scored [2017-01-01, 2020-01-01), 754 sessions.
- E3: `train-2020-2023-lo3` as built, 1,006 sessions, with each book's TRAIN fields dir.
- Universe: lo3 on the TRAIN stage pins. Each era has its own lo3 label role built with `--delisting-returns`.

**3.3 Checks before any return is read** (manifests, receipts and the audit only; section 5 gives the commands):
- P1: the stage and FE pins equal the TRAIN lo3 manifest's input pins.
- P2: R-2 and B0c reproduce their TRAIN NAVs byte for byte on the read build (PM7-25).
- P3: each era has score_begin 399 and ends no later than 2020-01-01; `score_member_counts` >= 1,000 on every scored
  session; `dropped_by_reason` reported per year.
- P4: the audit's coverage by field and year, delisted share, mask exits and first valid date per candidate. The
  members that are NaN on each era's first scored session go into progress.md.
- P5: the T1-T5 tests pass in root.
- P6: K1 plan-only fits within 2,560 / 3,072 MiB.
- P7: the plan lists exactly E1, E2 and E3, with the carry as the fit.

**3.4 Statistics.**
- **S1 (gate):** paired S2 net dSR, R-2 minus B0c, on the pooled E1+E2+E3 return rows (2015-01-02 to 2023-12-29).
  - Studentized circular block bootstrap: block 21, seed 20260929, 4,999 resamples (`--protocol v8`).
  - Reported: Memmel SE, one-sided p (E-34, `--pool-one-sided`), two-sided p.
- **S2 (gate):** DSR_tot of R-2's pooled series (`--dsr-total` with `--pool`).
  - N = N_tot at the read; history lines add 0.
  - V = `dsr_variance` of the construction lines on research-window-v2, with history lines excluded.
  - Printed beside it, gating nothing: the N_c row, and the DSR with V scaled by 1006/T (R5).
- **S3 (report only):**
  - S1 and S2 on E1+E2 alone (R4).
  - Per-era and per-year rows.
  - Whether "Sharpe is lower than on 2020-2023" held.
  - Per era: gross, net annual return, turnover, cost per traded dollar, net Sharpe at 4x NAV, and the tier census.
- **S4 (X-F only):** the X gate (PM7-8, PM7-26) on the pooled series, meaning the one-sided p against R-2 and DSR_tot.
  X-F against B0c is reported as in S3.

**3.5 Gate on the longer sample.**
- R-2 passes the freeze gate if S1 dSR > 0 with one-sided p < .10 AND S2 DSR_tot >= .95.
- X-F passes the X gate if S4 meets the same two thresholds.
- The other two legs keep their registered 2020-2023 values (S2 net 1.256 >= 1.0; mechanics met).
- History mechanics decide whether the read is valid (3.7). They are not a gate leg.

**3.6 Use.**
- The result may be used to:
  - state the verdicts;
  - report S1-S4;
  - freeze a passing book exactly as it was read. OD-1 then becomes eligible for an owner ruling.
- The result may not be used to:
  - choose, change, re-fit or re-weight any cell, member, theme, sign, L, universe, cost rule or financing rule;
  - rank the books by their history results;
  - open the era u-pass IC files or any per-member history statistic;
  - read the history a second time in v8.
- Any later construction that uses 2015-2019 is a search on a new window and is ledgered there.

**3.7 Ledger, defects and voids.**
- **R0:** one protocol line, which adds 0.
- **R3, for each book:**
  - E1 and E2 era lines, which add 0;
  - the E3 era line, which is skipped as the TRAIN cell's own line (T5);
  - one pooled line (window POOL, from 2015). It adds 0 to N and 1 to history reads (E-41).
- Appendix A then shows "history reads 2 (3 with X-F) (OD-3, one read)".
- **A failed pre-read check:** fix and re-check; nothing is counted.
- **A mechanical fault at R2,** found before any return is read: `research_cycle.py ledger-defect`, then a blind
  re-run (rule 7, adds 0). The fix goes in code, never in a book constant.
- **After R3:** the result stands and nothing is re-run. A defect found then is printed beside the result, and the PM
  rules whether the read is void. A void means no verdict and no second read in v8.

## 4. Ruled choices (PM7-29)

1. **Weights.** TRAIN admission, signs and weights are carried back unchanged (`fit.carry`). There is no
   re-admission and no re-fit, which overrides E-17 for this read. The anchor E3 reads the TRAIN file byte for byte;
   E1 and E2 read it rebound with `carried_from`. If wrong, the read speaks for the book and not for the method. A
   pooled re-fit is a separate, counted program after this read.
2. **Window.** Scoring covers 2015-01-01 to 2019-12-31. The read stops before any return if a scored E1 session has
   fewer than 1,000 lo3 members. H-1's 2014 start is dropped because of the source dates in section 2.
3. **Missing members.** They drop out by the coded present-member rule, and B0c carries the same gaps.
4. **Universe.** lo3 on the TRAIN pins, not H-1's lo1.
5. **Delisting.** Each era has its own dlret label role (`roles[].label_dir`). Before 2018 the stage is empty, so names
   exit at their last price, and P4 reports it.
6. **Leverage.** Each book's L is carried. There is no gross calibration on history; gross is printed.
7. **Borrow before 2018.** As coded: every name is warm and locate never binds. The tier census is printed.
8. **Gate sample.** Pooled 2015-2023. The history alone is printed beside it.
9. **DSR.** N_tot and the TRAIN-window V. The scaled-V DSR is printed and gates nothing.
10. **Ledger.** One history line per book read (B0c, R-2, X-F), so history reads go from 0 to 2 or 3.
11. **Timing.** Data builds, coverage and mechanics checks (B1-B8, P1-P7) may run during X. No era u, w or NAV pass
    runs before the last X cell is ledgered, because each one writes an IC or return number of the history.
12. **References.** X-F's gate is against R-2, and R-2's against B0c.

## 5. Runbook (integrator, pool-2 root, Git Bash)

```bash
# variables: w0-2-runbook.md section 2 (PY sha V1 TH FINRA SVRAW V2PIT EC INS SECF F13 FTD REGSHO SECM SVX SIC DL),
# plus FE=build-equity/fundamental-events-v3 FEV=304d2945d6226c0ca9b56d1fb6e8309ae1f42da5dfcd9652aee1a83dc29be87b
# F70="$F63,ret_overnight,ret_intraday,ceq_iss_5y,coskew_60m,vol_126,xrd0_ttm,grp_ff12f49"   (fields v10, in order)
# P1: each of V2PIT SIC DL FEV equals the input pin in build-equity/train-2020-2023-lo3/manifest.json and the v10
#     fields manifest; re-hash every stage manifest now. STOP on any difference.
# B1 projection
"$PY" scripts/run_bounded_research.py --seconds 300 --max-rss-mib 1100 --min-free-mib 512 --output build-equity/od3-projection-run --bind atx-engine/tools/prepare_recent_research.py -- "$PY" atx-engine/tools/prepare_recent_research.py project --source $TH --out C:/atx-wt/pool-2/build-equity/od3-projection --start 2013-06-01 --end 2020-01-01 --memory-mib 768 --disk-mib 12288 --max-seconds 270
for E in "E1 2013-06-01 2015-01-01 2017-01-01" "E2 2015-06-01 2017-01-01 2020-01-01"; do set -- $E
# B2 base role (STOP if score_begin != 399 after one --start move)
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1024 --min-free-mib 768 --output build-equity/od3-$1-base-v1-run --bind atx-engine/tools/prepare_recent_research.py --bind build-equity/od3-projection/manifest.json -- "$PY" -B atx-engine/tools/prepare_recent_research.py role --cache build-equity/od3-projection --out build-equity/od3-$1-base-v1 --start $2 --score-start $3 --end $4 --top-n 3000 --max-union 8000 --memory-mib 768 --disk-mib 1024 --max-output-mib 512 --max-seconds 120
# B3 factor-break scan; root reviews the listed sessions, then repairs with exactly that list (STOP if unreviewed)
"$PY" scripts/run_bounded_research.py --seconds 120 --max-rss-mib 1536 --min-free-mib 512 --output build-equity/od3-$1-scan-run --bind atx-impl/tools/repair_role_factor_breaks.py --bind build-equity/od3-$1-base-v1/manifest.json -- "$PY" -B atx-impl/tools/repair_role_factor_breaks.py --scan-only --role build-equity/od3-$1-base-v1 --role-sha256 $(sha build-equity/od3-$1-base-v1/manifest.json)
"$PY" scripts/run_bounded_research.py --seconds 120 --max-rss-mib 1536 --min-free-mib 512 --output build-equity/od3-$1-base-run --bind atx-impl/tools/repair_role_factor_breaks.py --bind build-equity/od3-$1-base-v1/manifest.json -- "$PY" -B atx-impl/tools/repair_role_factor_breaks.py --role build-equity/od3-$1-base-v1 --role-sha256 $(sha build-equity/od3-$1-base-v1/manifest.json) --out build-equity/od3-$1-base --expect-sessions <the reviewed list>
BASE=$(sha build-equity/od3-$1-base/manifest.json)
# B4 lo3 role, B5 its label role (lo3 + delisting returns)
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 --output build-equity/od3-$1-lo3-run --bind build-equity/od3-$1-base/manifest.json --bind $V1/export/identity-bridge-v2-pit/manifest.json --bind $V1/fundamentals/manifest.json --bind $V1/delisting/manifest.json -- "$PY" atx-engine/tools/prepare_recent_research.py role --universe linked-operating-v3 --out build-equity/od3-$1-lo3 --base-role build-equity/od3-$1-base --base-role-sha256 $BASE --identity-bridge $V1/export/identity-bridge-v2-pit --identity-bridge-sha256 $V2PIT --sic-events $V1/fundamentals --sic-events-sha256 $SIC --delisting $V1/delisting --delisting-sha256 $DL --memory-mib 1024 --max-seconds 170
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 --output build-equity/od3-$1-lo3-dlret-run --bind build-equity/od3-$1-base/manifest.json --bind $V1/export/identity-bridge-v2-pit/manifest.json --bind $V1/fundamentals/manifest.json --bind $V1/delisting/manifest.json -- "$PY" atx-engine/tools/prepare_recent_research.py role --universe linked-operating-v3 --out build-equity/od3-$1-lo3-dlret --base-role build-equity/od3-$1-base --base-role-sha256 $BASE --identity-bridge $V1/export/identity-bridge-v2-pit --identity-bridge-sha256 $V2PIT --sic-events $V1/fundamentals --sic-events-sha256 $SIC --delisting $V1/delisting --delisting-sha256 $DL --delisting-returns $V1/delisting --memory-mib 1024 --max-seconds 170
LO3=$(sha build-equity/od3-$1-lo3/manifest.json)
# B7 fields v10 on the era: the TRAIN v10 receipt argv except role, role SHA, outputs, role bind and no --reuse*
#     (STOP if the diff against train-2020-2023-lo3-fields-v10-run's recorded argv shows anything else)
"$PY" scripts/run_bounded_research.py --seconds 600 --max-rss-mib 2560 --min-free-mib 512 --output build-equity/od3-$1-lo3-fields-v10-run --bind atx-engine/tools/prepare_research_fields.py --bind atx-engine/tools/research_fields_sec.py --bind atx-engine/tools/research_fields_holdings.py --bind atx-engine/tools/research_fields_price.py --bind atx-engine/tools/research_fields_v8.py --bind build-equity/od3-$1-lo3/manifest.json -- "$PY" atx-engine/tools/prepare_research_fields.py --role build-equity/od3-$1-lo3 --role-sha256 $LO3 --output build-equity/od3-$1-lo3-fields-v10 --fields $F70 --finra $FINRA --tickerhistory $TH --finra-short-volume $SVRAW --price-source $TH --identity-bridge $V1/export/identity-bridge-v2-pit --identity-bridge-sha256 $V2PIT --fund-events $FE --fund-events-sha256 $FEV --fund-lag-sessions 1 --sic-events $V1/fundamentals --sic-events-sha256 $SIC --sec-stages $V1 --sec-identity-bridge $V1/export/identity-bridge-v2-pit --sec-identity-bridge-sha256 $V2PIT --earnings-calendar-sha256 $EC --insider-sha256 $INS --sec-filings-sha256 $SECF --thirteenf $V1/thirteenf --thirteenf-sha256 $F13 --ftd $V1/ftd --ftd-sha256 $FTD --regsho-threshold $V1/regsho_threshold --regsho-threshold-sha256 $REGSHO --security-master $V1/security_master --security-master-sha256 $SECM --short-volume-ext $V1/short_volume_ext --short-volume-ext-sha256 $SVX --max-rss-mib 2048 --max-seconds 580
done
```

**B6 specs** (`scripts/specs/v8/od3-{b0c,r2,x}.json`). Each is written from the book's resolved cycle spec (a
template such as `base-b0c.json` is first resolved by research_spec.py) as follows:
- **`name`:** `od3-<book>`.
- **`inputs`:** library and recipe only. Drop `role`, `label_role`, `baseline_*` and `reference_*`.
- **`roles`:** three entries.
  - E1: `{"id": "E1", "dir": "build-equity/od3-E1-lo3", "begin": "2015-01-01", "end": "2017-01-01", "universe": "linked-operating-v3", "fields_dir": "build-equity/od3-E1-lo3-fields-v10", "label_dir": "build-equity/od3-E1-lo3-dlret"}`
  - E2: the same with E2 and the dates `2017-01-01` to `2020-01-01`.
  - E3: `{"id": "E3", "dir": "build-equity/train-2020-2023-lo3", "begin": "2020-01-01", "end": "2024-01-01", "universe": "linked-operating-v3", "fields_dir": <the book's TRAIN fields dir: v9 for B0c, v10 for R-2>, "label_dir": "build-equity/train-2020-2023-lo3-dlret"}`
- **`fields`:** `{"output": "build-equity/od3-fields-unused", "manifest_sha256": null}`, with no list. Every era is as built.
- **`fit`:** the book's own section, plus `"carry": {"weights": {"path": <the TRAIN fit's composition_weights.json>, "sha256": null}, "admission": {"path": <its admission.json>, "sha256": null}}`.
- **`ic.u_output`, `ic.w_output` and `nav.output`:** renamed `od3-u-<book>`, `od3-w-<book>` and `od3-nav-<book>` under `build-equity/`. The NAV rule, L and flags stay unchanged.
- **Dropped:** `static_check`, `ref`, `card`, `marginal`, `monitor`, `compare`, `gate`, `verdict` and `summ.cells_from_ledger`.
- **`summ.extra`:**
  - B0c: `--protocol v8 --psr --dsr-total build-equity/trials.jsonl`.
  - R-2: the same, plus `--pool-reference build-equity/od3-nav-b0c-E1 build-equity/od3-nav-b0c-E2 build-equity/od3-nav-b0c-E3 --pool-one-sided`.
  - X-F: the same, plus `--pool-reference` with R-2's three dirs.
- **`summ.ledger`:** `build-equity/trials.jsonl`, with each book's TRAIN `origin`.
- **Risk model:** R-2 and B0c read none. If X-F's NAV flags carry `--risk-model`, STOP: the roles loop has no per-era
  risk store, so this is a new tooling gap for the PM. Otherwise each era's store would be built with integration-log
  line 4392's argv on its lo3 role and v10 fields.

```bash
"$PY" scripts/research_cycle.py lock scripts/specs/v8/od3-r2.json --write   # also od3-b0c, od3-x; fills role, fields, label, carry pins
"$PY" scripts/research_cycle.py plan scripts/specs/v8/od3-r2.json           # P7: fields:E1..E3 pinned, u:E1..E3, fit = carry, w:E1..E3, nav:E1..E3, summ
# P6: K1 plan-only per era (the w pass argv of the plan with --plan-only) under the caps; P3/P4 from the manifests and audit
# B8 audit after P6 (its --plan is P6's JSON) (opens no return), once per book library (v71, v80, X-F's)
"$PY" atx-impl/tools/era_data_audit.py --era E1 build-equity/od3-E1-lo3/manifest.json <sha> build-equity/od3-E1-lo3-fields-v10/manifest.json <sha> --era E2 build-equity/od3-E2-lo3/manifest.json <sha> build-equity/od3-E2-lo3-fields-v10/manifest.json <sha> --library <lib> --library-sha256 <sha> --plan <K1 plan-only JSON> --json build-equity/od3-audit-<book>.json
# ---- the last X cell is ledgered (PM7-29 choice 11); X-F named and pinned; then:
"$PY" scripts/research_cycle.py ledger-protocol --ledger build-equity/trials.jsonl --owner-ruling "PM7-29 OD-3 read, v8-od3-protocol.md sha256 <S>, books B0c R-2 [X-F]" --date <D>   # R0
"$PY" scripts/research_cycle.py run scripts/specs/v8/od3-b0c.json --stop-after nav   # R1, then od3-r2, then od3-x
# R2 mechanics only (no return column): per NAV dir, exit 0 and these keys; STOP on a refusal or a pin mismatch
"$PY" -c "import csv,json,sys,statistics as s; d=sys.argv[1]; m=json.load(open(d+'/summary.json')); r=list(csv.DictReader(open(d+'/daily_'+m['primary_scenario']+'.csv'))); g=[float(x['gross_leverage']) for x in r[63:]]; print(json.dumps({'status':m['status'],'rule':m['rule'],'role':m['role_sha256'],'weights':m['composition_weights_sha256'],'rows':len(r),'gross_post_ramp':s.mean(g),'held_min':min(int(x['held_names']) for x in r[63:])}))" build-equity/od3-nav-r2-E1
sha build-equity/od3-nav-r2-E3/daily_modeled-1bn-stale5-v1+swap-fin-v1.csv   # must equal R-2's TRAIN ledger series sha256 (same for B0c, X-F)
"$PY" scripts/research_cycle.py run scripts/specs/v8/od3-b0c.json   # R3: summ (ledgered pooled line), then od3-r2, then od3-x
"$PY" atx-impl/tools/nav_summ.py --pool build-equity/od3-nav-r2-E1 build-equity/od3-nav-r2-E2 --pool-ids E1,E2 --pool-reference build-equity/od3-nav-b0c-E1 build-equity/od3-nav-b0c-E2 --pool-one-sided --protocol v8 --dsr-total build-equity/trials.jsonl --json build-equity/od3-hist-r2.json   # R4, no --ledger (X-F: against R-2, then B0c)
"$PY" -c "import sys,json; sys.path.insert(0,'atx-impl/tools'); import dsr_total as DT; r=json.load(open(sys.argv[1]))[-1]; t,m=r['deflated_total'],r['net_moments']; print(DT.dsr_at(m,t['tot']['n'],t['variance_sr']*1006/m['sessions']))" <the summ --json of od3-r2>   # R5 scaled-V DSR (gates nothing)
```

**Stop conditions.** Each one stops the read before any return is opened, and the PM rules:
- a P1 pin difference;
- a P2 identity failure;
- score_begin not equal to 399;
- an era past 2020-01-01;
- unreviewed factor-break sessions;
- an E1 scored session under 1,000 lo3 members;
- a fields argv diff beyond the allowed keys;
- a plan with other eras, or a carry pin or anchor refusal;
- K1 over its caps;
- an X-F with `--risk-model`;
- the last X cell not yet ledgered;
- at R2: a non-zero exit, status not `complete`, a role or weights pin that differs from the era or carry files, or an
  E3 daily CSV SHA that differs from the TRAIN series.

After R3 nothing stops the read: the result is printed as it is.
