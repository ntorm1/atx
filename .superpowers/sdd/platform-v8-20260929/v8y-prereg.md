# v8 expansion Y pre-registration (lane YPRE, 2026-10-02; for the PM's ruling before any Y measurement)

Basis: PM8-1..PM8-11 (draft `pm8-rulings-draft.md`, binding; root appends it verbatim to progress.md at the Y merge),
`v8x-prereg.md` (its sections and PM7-6..PM7-39 bind Y except where this file amends them by name), PM7-34, PM7-35,
PM6-6, E-45. PM8-1..PM8-4 as the draft records them: "in root's dispatch (acceptance stays PM7-34; Y opened; X-10 after
Y; root runs without return)". PM8-12 ("puts the rule in C++; Python only writes the spec") and PM8-13 ("half-life
table pinned") are cited by YCOMB's report; neither text is in the draft this lane was given. Lane reports (section
14): YSIG `5431831a`, YDATA `e1cf4135`, YCOMB `97eb5d0f` (Y-5 wiring `e2ac7d63` and Y-5 / Y-1 composition `3ff73201`
folded in), YINFRA `dde31df3` (code head `2697c3e1` after review fixes); YOPS `19d58b6d` (code only). Nothing here was
derived from a return, IC, Sharpe, turnover or NAV
output of 2020-2023 beyond the public lines of progress.md (X batch 1); nothing dated 2024-01-01 or later was opened. No
Y cell, screen or field build has run; no Y statistic exists. Choices left to the PM: Appendix B (YP-1..YP-15), each a
recommendation with its cost if wrong. Cross-lane checks (every SHA-256 recomputed; inconsistencies): Appendix C.

## 1. Baseline and named books

- **R-2** (V8-F): the deployable book until an adoption print passes (PM7-25, PM7-34 (2)).
- **X-F0** (v8x section 1, unchanged): last accepted after X-9, leverage excluded. It is the parent of Y-S (PM8-10 (e):
  "parent = the accepted book after X-9"). Root names it at run time, in the Y-S manifest and the log: spec path and
  digest, ledger trial_id, S2 daily net series SHA-256, L, fields dir and manifest (v14), and N_c, N_tot, M at its
  ledger state. Public state at this writing: last accepted X-5 (`x-theme-erc-gm.json`, L 1.1720, N 54); X-7, the
  campaign and X-9 have not run.
- **H-F** (v8x, unchanged): last accepted hand-written X cell; the book of v8x's DSR_hand.
- **Y-F0**: last accepted cell after Y-5, leverage excluded: the book of every claim of X and Y together, the parent of
  X-10 and the unlevered reference of Y-1 (amends v8x section 8's "parent X-F0": X-10 now runs after Y-5, PM8-3).
- **Y-F**: the levered book offered to the owner (PM7-3): Y-1 if accepted, else X-10 if accepted, else none. It takes
  the role of v8x's X-F, which is not formed separately.
- Every other Y cell's parent is the last accepted cell before it in section 6's order (v8x rule; YP-1).

## 2. Windows and seal (unchanged)

TRAIN [2020-01-01, 2024-01-01), seal 2024-01-01, from `atx-impl/strategies/research_window.json` (sha256 `62cf2cfa...`,
research-window-v2), never typed. Disclosure carried: 2023 and 2024 were read twice at book level before v8; every v8, X
and Y cell reads 2023; 2025+ never read. Every Y field is sealed at its build (section 9). Y-2's schedule starts at
decision 254 (about 2021): its paired series equals its parent's in 2020, so its dSR SE is printed with the window
2021-2023 stated (PM8-10 (c)).

## 3. Trial counting (continues from the ledger at X-F0; no reset; PM7-7)

Ledger of record: root `build-equity/trials.jsonl`, chain-verified. Code names as v8x section 3 (`N_c`, K_a, M,
**N_tot = sum(trial_counts(records)) + M**). Y starts at X-F0's ledger state, recorded by root as `expect.n_before`.
Public at X-6: N_c 55; admission lines 34 (v8 12, X hand-written 13, X-4 re-screens 9); ledger 98 lines, head
`a190f7ef`. Before Y: X-7 (1 cell, up to 12 admission lines; X hand-written 25, PM7-36), the campaign (B = 110 after the
lock: "10 fields, B = 110", X batch 2 note), X-9 (1 cell, up to 16). If X-7 and X-9 are both ledgered, N_c(X-F0) = 57.

| event | counts in | count | ledger evidence |
|---|---|---|---|
| Y construction cell scored (Y-S, Y-3, Y-2, Y-5, X-10, Y-1) | N_c | 1 | construction line; defect rule (`trial_counts`) |
| string listed in Y-S's `gate.admitted`, admitted or not (the 15 below) | K_a | 1 each | admission line per string, origin prior |
| Y-S with no cell (gate exit 10, or PM7-35 drops every string) | N_c | 0 (its admission lines stay) | gate line; ruling |
| b library of the kept strings (same trial ids) | - | 0 new lines | PM7-35 |
| iv_vol_of_vol clock repair (PM8-8 (6)); a sign or constant corrected from its citation before any read (YP-6) | - | 0 | integration log; string re-pinned |
| fields v15 build, theme registry change, roster cap, builds, identity runs, Release IC switch, K1 plan-only, memory probe, PM6-6 calibration runs, driver `plan` / `--dry-run` | - | 0 | integration log (W0-i) |
| candidate withdrawn before any read (field absent, K1 refusal not mechanically rewritable) | - | 0 | ruling |
| undefined cell (E-45, PM4-10); cell stopped before any return read | - | 0 | ruling, logged |
| void cell after a return read (section 12) | N_c | 1 (stays) | its line |
| rule-7 tool or data defect: blind re-run / returns re-run | - / N_c | 0 / 1 | `rerun_basis` |
| any 2020-2023 statistic read for a variant not on the Y list | N_tot | 1 per variant | disclosure; the variant never enters Y |
| adoption print; OD-3 history read | - | 0 | counted apart (history lines) |

What closes the search (v8x section 3, carried): (1) closed list: only section 14's items are measured, each one frozen
string or rule with its constants; after the first Y read nothing joins (an idea from a result is a v9 note); (2) a read
is a trial; (3) no retry, re-parameterisation or re-cast of a rejected cell or member (E-45); (4) voids keep their
count; (5) hard ceilings (section 5), unused budget lapses.

**Y hand-written admission trials: 15** (PM8-8 (4): "hand-written admission trials +14 for Y: 10 YSIG + 4 YDATA";
PM8-11: `conn_rev` "joins the Y-S wave as the 15th hand-written admission trial (Y total 15: YSIG 10 + YDATA 5)").
Roster order = table order (YP-13). Every sign is +1 with the literature sign inside the string; origin `prior`.

| # | id | theme | tier | sign | horizon class | half-life (sessions) | Y-5 bucket (theme) | DSL SHA-256 | source | argv line SHA-256 (16) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `peer_mom_1m` | price_momentum | B- | +1 | medium | 21 | slow | `67d4ff4a86c439d7734f7cf6ceff56318137ee1565a3729430a736caada2de77` | YSIG 4 (Y-1), argv 6 | `89e07be1e33db181` |
| 2 | `so_wang_rev` | reversal_seasonality | B- | +1 | fast | 2 | fast | `c145cd2a7c1f85ef88a9bed87662ee8e3af8c71becb1d2b3f742aacd10cf59c8` | YSIG 4 (Y-2), argv 6 | `d7830943610fc079` |
| 3 | `iv_vol_of_vol` | options_implied | B- | +1 | medium | 21 | slow | `c5ecec15fbdb480700d8db39b5f8042338ce1a49fd6d86de3a2f7c3a7fc9fcc7` (re-pinned by the repair, section 10) | YSIG 5, argv 6 | `06ef2910b06dcc1e` |
| 4 | `day_rev_freq` | reversal_seasonality | B- | +1 | medium | 21 | fast | `a5416c4ea13d4422592b378716d2a11b493d0152d06c2a48afc3b5937e42c5a7` | YSIG 5, argv 6 | `f57d3a76a9df22a1` |
| 5 | `mom_turn` | price_momentum | C+ | +1 | slow | 126 | slow | `09946f74588e3060c9f54a0bbefde0c173e1bb39efcdbb10e6d95633c522b373` | YSIG 4 (Y-5), argv 6 | `0a53d97cc2f24308` |
| 6 | `ea_uvol` | earnings_momentum | C+ | +1 | slow | 31 | slow | `15f682ffd7504fbb249d5cffedc80a8be28ce93e781e41a5843413f7135d515c` | YSIG 4 (Y-6), argv 6 | `9f61609229cf55d7` |
| 7 | `dato` | profitability_quality | C+ | +1 | slow | 126 | slow | `694fbe591a52faf131cebef6b0ccc753b3d30d7a7fa99609ce3f1915a916a20d` | YSIG 4 (Y-7), argv 6 | `61728bcc94dcc87f` |
| 8 | `fscore_hbm` | profitability_quality | C+ | +1 | slow | 252 | slow | `f4d3d3fa7f14fb6ff2d61651880837b187bb66429b4051f167593815e839f21f` | YSIG 4 (Y-8), argv 6 | `ceeed83aa97b7d0f` |
| 9 | `exch_switch` | filing_events | C+ | +1 | slow | 252 | slow | `f433b32af008e74c13f04026537a12f735f636cd3a2dadade94200dcd0263ac3` | YSIG 5, argv 6 | `7f2cac46fb20a586` |
| 10 | `ins_cluster` | ownership_flow | C+ | +1 | medium | 10 | slow | `a3dfad407c4cd493562022b50172e18ab764033812844a6b23b9209a788f23d2` | YSIG 2b.4 | `2c7272b4d4e4c852` |
| 11 | `smile_slope` | options_implied | C+ | +1 | medium | 21 [YPRE] | slow | `063202690eb1f981d789a31206f080318d73bdd7477aff6fe261f1ad32159609` | YDATA 2e | `bf1a6f07d786c4fc` |
| 12 | `stio_trade` | ownership_flow | C+ | +1 | slow | 63 [YPRE] | slow | `e845ba078144883ba13eefd48bfce4d1016dbf35e889515d64b46b4f6ccc619e` | YDATA 2e | `7dfca7c66bb61edc` |
| 13 | `div_event` | filing_events | C+ | +1 | slow | 126 [YPRE] | slow | `2dbe474ef4f26f2d0dd8e3a845a57bdfaae26728b5d58fa747a665ec1540bbe6` | YDATA 2e | `6e7436c951616fea` |
| 14 | `deal_target` | merger_arbitrage | C+ | +1 | slow | 126 [YPRE] | slow | `815952591ce849a82926f3144c0714ee5eba0925d0d7c82971842202e71aaf05` | YDATA 4 | `1caa171ac90d2ce8` |
| 15 | `conn_rev` | reversal_seasonality | C+ | +1 | medium | 21 [YPRE] | fast | `4a1cffe83a007531b9820da67da1a0938391832c79116c5319cd11e4858bcc45` | YDATA round 2 | `2afdabf99a96d7de` |

Horizon class and half-life: YSIG section 7 / 2b.4 (classes fast < 5, medium 5-21, slow > 21; PM8-5). YDATA registered
none; [YPRE] values are proposed here from the papers, blind (YP-4): smile_slope 21 (Xing-Zhang-Zhao, Yan: one-month
horizon; the string's decay 21), stio_trade 63 (Yan-Zhang: the next quarter; a quarterly field), div_event and
deal_target 126 (half the fields' 252-session event windows, YSIG's convention for `ea_uvol` and `ins_cluster`),
conn_rev 21 (Anton-Polk: monthly cross-stock reversal). Member half-lives decide nothing: Y-5 buckets by theme
(section 11). Themes: PM8-6, PM8-8 (2) ("`div_event` in filing_events confirmed"; `merger_arbitrage` the 13th theme),
PM8-11 (`conn_rev` in reversal_seasonality). argv: the lane report's printed line (placeholders included), full SHA-256
in section 14; root fills only the slots of section 14.

## 4. Deflated Sharpe (formula unchanged; PM7-7)

Formula, moments and V as v8x section 4 (`backtest_integrity.expected_max_sr`, `deflated_sharpe`; tool
`nav_summ.py <book dirs> --dsr-total <ledger> [--dsr-hand <dir> ...]`, logic `atx-impl/tools/dsr_total.py`). V is the
ddof-1 variance of `s2_net_sr / sqrt(252)` over the construction lines on research-window-v2 at the print: X-7, X-9, the
Y cells, X-10 and Y-1 included (v8x already put the leverage cell in V). The counts now include X-7, the campaign (M),
X-9 and Y. Under PM7-34 (2) no DSR gates; all three are printed beside the adoption rule (section 7).

| name | book | N | role |
|---|---|---|---|
| DSR_tot | Y-F0 | N_tot of the whole ledger at the adoption print | PM7-1, PM7-7; printed, gates nothing (PM7-34 (2)) |
| DSR_hand | Y-F0 when X-9 is not in its lineage | N_tot - M - the mined wave's lines (X-9's construction line and its admission lines); V as DSR_tot | PM7-2: what dropping the campaign gives (YP-2). When X-9 is in Y-F0's lineage the book holds mined members: "n/a" is printed and v8x's DSR_hand of H-F stands in its place |
| DSR_v8 | Y-F0 | N_c (v8 rule 3, `nav_summ --dsr-ledger`) | continuity with V8-F; gates nothing |

DSR_hand is `dsr_total.dsr_at(moments, N_hand, V)` with N_hand from the tool's printed parts (`ledger_trials`,
`campaign_registry`) less the mined wave's lines counted from the ledger by its cycle prefix; root prints the inputs in
the log (no tool change; YP-2). Printed beside, gating nothing: X-F0 at its own ledger state (`--dsr-hand <X-F0 dir>`,
the prefix rule: the X part of the gain) and V8-F at its own (public: .465 at N 50). "DSR up" = DSR_tot(Y-F0) at the
print above DSR_tot(V8-F) at V8-F's ledger state; V8-F at the print's N and V printed beside.
Scale (arithmetic, gates nothing): E[max] factor 2.276 at N 50 (with V8-F's public V 1.297e-03 per session it gives the
printed SR0 1.301: formula check), 2.326 at N_c 57 (after X-9), 2.364 at 63 (+6 Y cells); the hand count (construction +
admission lines, legacy kinds not included) about 119 after X-9 at the caps, 140 with Y (2.591 -> 2.647); N_tot about
229 -> 250 with M 110 (2.809 -> 2.838). Y raises SR0 at any V by 1.6% (N_c), about 2% (hand), 1.0% (N_tot). V
decides, not the count: every X / Y cell whose Sharpe sits far from the window's mean raises V.

## 5. The Y budget (PM8-8 (4), PM8-6, PM8-10, PM8-11)

| source (lane) | construction cells | admission trials | campaign evaluations |
|---|---|---|---|
| Y-S: YSIG 10 + YDATA 4 + YDATA round 2 (`conn_rev`) | 1 | 15 | - |
| Y-3 norm-score-v1 (YCOMB) | 1 | 0 | - |
| Y-2 theme-tsmom-v1 (YCOMB) | 1 | 0 | - |
| Y-5 two-speed-v1 (YCOMB; wired at `e2ac7d63`) | 1 | 0 | - |
| X-10 leverage L 2.0 (the X budget's cell, run in Y's order: PM8-3, PM8-10 (e)) | 1 | 0 | - |
| Y-1 vol-target-v1 (YCOMB) | 1 | 0 | - |
| Y-4 (unused, not refilled: PM8-10 (d)); iv_vol_of_vol clock repair (PM8-8 (6)) | 0 | 0 | - |
| **Y ceiling** | **<= 6** | **<= 15** | **0** |

N_c <= N_c(X-F0) + 6 (PM8-8 (4): "N_c +6 for Y cells"); N_tot <= N_tot(X-F0) + 21. X-10 counts once: it is inside the
+6, its slot in the X ceiling lapses unused, and v8x's absolute bound "N_c <= N_c(V8-F) + 10" is superseded for X-10
only (it now runs after four Y cells; YP-3). Roster: `house_budget.max_roster` 80 -> 96 before any Y screen (PM8-6: "the
cap was a budget guard, not a hypothesis"); X-F0 holds at most 80 (PM7-13), so the Y-S screen library holds at most 95.
Caps: Y cells keep W0-c (IC phases 2,560 MiB / 300 s, others 1,536 MiB / 180 s) and the marginal phase 360 s (PM7-31),
re-probed for Y-S (YP-12); field builds 2,560 MiB / 600 s (W0-i). Argument (v8x section 5, carried): one wave for the 15
strings (per-member cells would add 15 to N_c and decide nothing; the admission gate screens and counts each one); one
cell per rule (each acts on the whole library, fit or aim); the leverage pair last.

## 6. Order of Y cells and acceptance (fixed by PM8-10 (e); never re-ordered after a read)

PM8-10 (e), verbatim: "Y cell order, fixed now: Y-S (one add-alpha wave: YSIG 10 + YDATA 4 (+ YDATA round 2 if
delivered before the Y-S screen), parent = the accepted book after X-9) -> Y-3 norm-score -> Y-2 theme-tsmom -> Y-5
two-speed -> X-10 (L 2.0) -> Y-1 vol-target -> adoption print (section 7) -> OD-3 read. Why: signals before rules; the
rank-shape rule acts on member scores before sleeves; theme timing before the trade rule; leverage last and its managed
form after it -- cost if wrong: none; the order is set with no read."
Unless a row says otherwise: parent = last accepted; fields v15 (section 9); paired S2 net dSR against the parent; both
p printed (E-34, PM5-23); year table, 4x row (E-29), turnover and the Appendix A block printed. "Mechanics" = v8x's
limits (all-rows gross [.90, 1.05], |mean net| <= .02, tau mean <= .20, p95 <= .30) unless restated in section 8.

| cell | content | PM6-6 | acceptance (decides) | printed only (decides nothing) |
|---|---|---|---|---|
| Y-S | the 15 strings of section 3, one add-alpha wave, roster order; parent X-F0 | yes | dSR > 0 AND mechanics (PM7-34 (1)) | capacity criterion PM7-10 (net Sharpe at 4x NAV not lower than the parent's: met / unmet; PM8-11 for `conn_rev`), turnover, gate rows, marginal rows (report only) |
| Y-3 | `norm-score-v1`: `nav --rank-shape norm-score-v1`, NAV-only | yes (template: calibration at L_P, then a -gm copy) | dSR > 0 AND mechanics (PM8-10 (c)) | the lane's criterion of the hypothesis: S2 gross-of-cost annual return per unit of all-rows gross above the parent's; borrow and cost per traded dollar (PM8-10 (c)); net annual return, realised vol, turnover per unit gross, 4x net Sharpe |
| Y-2 | `theme-tsmom-v1`: `fit --theme-tsmom theme-tsmom-v1` on the parent's fit argv (u is the parent's) | yes | dSR > 0 AND mechanics (PM8-10 (c)) | dSR SE with its window 2021-2023 stated (PM8-10 (c)); theme_blocks_off, theme_on_fraction, turnover per unit gross, cost per traded dollar, net annual return, 4x net Sharpe |
| Y-5 | `two-speed-v1`: `fit --two-speed two-speed-v1` and `nav --two-speed two-speed-v1`; fast / slow sleeves netted before trading (buckets section 11; theta_s .05, theta_f .12945) | yes (template: calibration at L_P, then a -gm copy) | dSR > 0 AND mechanics (PM8-10 (a)) | turnover per unit gross, cost per traded dollar, 4x net Sharpe, net annual return, the fast mass share (`provenance.two_speed`) |
| X-10 | L 2.0 on Y-F0 (section 8) | no (PM7-11) | net annual return above Y-F0's AND S2 net Sharpe not lower than Y-F0's by more than .100 AND mechanics (PM7-34 (3)) | 4x guard; dSR and both p; section 8 report |
| Y-1 | `vol-target-v1` at cap 2.0 on Y-F0's NAV, the child of X-10 (PM8-10 (b)) | no (PM7-20 reading B) | paired S2 net dSR > 0 against X-10 AND X-10's rule against Y-F0 AND mechanics (YCOMB registered) | dSR against Y-F0; net annual return, realised vol, max drawdown of Y-F0, X-10, Y-1 side by side; mean L_t; 4x net Sharpe; cost per traded dollar |
| - | adoption print (section 7), once | - | - | - |
| - | OD-3 read (v8x section 10; on Y-F0, YP-11) | - | - | - |

Sign rule (Y-S only; PM7-35, additions, as coded in `wave_rules.py` `pm7-35`): admitted with runner sign = prior: kept;
runner sign 0: kept (R-2 precedent); runner sign opposite to the prior: dropped; not admitted: kept at weight 0 in the
fit (R-2 / R-7 precedent). The cell is the b library of the kept strings (same trial ids, 0 new lines). A string without
an admission row is a stop, never a drop (YINFRA review fix `4fcd8dde`).
One change per member (PM7-14): every Y-S string is an addition (no `--replaces`, no `--rescreen`); no member changed in
X is changed again; the Y rules act on the whole library, fit or aim. One mechanism, one lane, settled before any read:
YDATA's `iv_vov_21` withdrawn for `iv_vol_of_vol` (`5a79dc45`); ORATS put-wing `iv_skew` against `smile_slope`, one kept
if it arrives (PM8-8 (5)); a fund-level `conn_ret63` route, one kept (PM8-11). Restatements ruled: Y-5 is not R-3
(PM8-10 (a)); Y-1 is not R-8 (PM8-10 (b)); Y-3 and Y-2 accepted as registered (PM8-10 (c)). Plan section 13 binds Y
(PM7-14).
Undefined (0, logged; the next cell runs on the same parent; E-45, PM4-10):
- Y-S: no cell if the gate fails (exit 10) or PM7-35 drops every string; a string whose field is absent from v15
  (e.g. `exch_switch` without `exch_up_365d`) is withdrawn at 0 before the screen.
- Y-3: the parent's NAV is not aim-partial-v5, or carries `--hold-band`, `--vol-scale`, spo-v1 / v2 (the NAV refuses).
- Y-2: the parent's fit is not ew-theme-std-v1 / ic-shrink-v1 (with or without `--theme-erc`), or carries
  `--theme-resid` or `--era`.
- Y-5: the parent is spo, aim-partial-v6 or not at the fixed rate, or carries `--hold-band`, `--vol-scale`,
  `--adv-hold-q` (the NAV and the target replay refuse); the parent's fit has no rerank-true
  theme_standardise or carries theme_residualise; a weighted theme is missing from the two-speed table (the IC runner
  refuses it: `merger_arbitrage` unless YP-10 adds it); a bucket is empty. Y-3's rank shape and Y-2's schedule compose
  with it (YCOMB "Y-5 wiring").
- X-10: L_P >= 2.0 (no room under the executable's maximum).
- Y-1: X-10 undefined or void (no comparison book); the NAV refuses `--vol-target` with the parent's flags
  (`--risk-target`, spo, v6, no store): "if E-45 makes it undefined on X-10 at run time it is 0 and logged" (PM8-10
  (b)). On a Y-F0 that holds Y-5 it is defined since `3ff73201` (the scaler's L_t scales the netted target; YP-15).
  Y-1 runs when X-10 is rejected (YP-8).

## 7. The adoption print (v8x section 7 unchanged; once, after Y-1, PM8-3)

Run once, after Y-1 (after the last defined cell if Y-1 is undefined). v8x section 7 as PM7-34 (2) amended it (the v8x
text predates the amendment): Y-F0 is the deployable book if its S2 net Sharpe >= 1.0 AND mechanics of Y-F0. Printed
beside, gating nothing: `nav_summ.py --protocol v8 --bundle <R-2 nav> <Y-F0 nav>` (rule 4: block 21, seed 20260929,
4,999 resamples; both p), DSR_tot, DSR_hand, DSR_v8, PBO, year table, net Sharpe at 4x, net annual return, turnover, the
Appendix A block; also (YP-14) the bundle X-F0 vs Y-F0 (the Y increment) and B0c vs Y-F0. Y-F is reported per section 8
beside Y-F0. If the rule fails, R-2 stays the deployable book, the report says so, every per-cell verdict stands,
nothing is re-run, and the book is carried to v9 with its count. Every report keeps PM7-37 / PM7-38's X-5 lines
(in-sample shares; the review's decomposition; "about +.15 expected"). Claims as v8x section 7 with Y-F0 for X-F0:
Sharpe up, DSR up (section 4), Capacity up (sign-level, Y-F0 4x against R-2's), Return up split into Y-F0 vs R-2
(matched gross) and Y-F vs Y-F0 (leverage: the owner's risk decision, never called alpha).

## 8. X-10 and Y-1: the leverage pair (v8x section 8, PM7-11, PM7-17, PM7-34 (3), PM8-10 (b))

- **X-10.** Parent Y-F0 at L_P. **L = 2.0**, the executable's maximum: one value, no grid; r = 2.0 / L_P. Spec
  `change.set {"nav.leverage", "nav.output"}` on Y-F0's spec (its NAV flags carried); not gross-matched. Mechanics
  restated: all-rows gross inside [.90, 1.05] x 2.0 / L_P (PM7-17), written in the cell's plan before the run;
  |mean net| <= .02 r; tau mean <= .20 r, p95 <= .30 r; every other row unchanged. Acceptance (PM7-34 (3)): net annual
  return (S2, 1x) above Y-F0's AND S2 net Sharpe not lower than Y-F0's by more than .100 ("one SE", plan 12.2) AND
  mechanics; the 4x guard is printed.
- **Y-1** (template `scripts/specs/v8/y-vol-target.json` @ `47d6afd9`). L_t = clip(2.0 x sigma_ref_t / sigma_hat_t, 1,
  2.0), re-estimated every 21 sessions; sigma_hat = risk-target-v1's forecast on the pinned atx-risk-v1 store; sigma_ref
  = the running mean of the book's own estimates, current one included (Cederburg et al. 2020 real-time form); cap
  before the first estimate. Spec: Y-F0's spec + `nav.leverage` 2.0 + `nav --vol-target vol-target-v1 --risk-model
  <store> --risk-model-sha256 <pin>` (the R-8 store `build-equity/v8-risk-lo3-v10`, manifest `862515d9...`, if its role
  pin is Y-F0's; else built first at 0 trials). NAV-only (u, fit, card, w are Y-F0's). Paired reference: X-10's NAV. The
  template's description says "parent = X-F0": under PM8-10 (e) the unlevered parent is Y-F0 (Appendix C).
  Mechanics: all-rows gross inside [1.0, 2.0] x G_P / L_P widened by .005 (the template's restatement, from the clip
  only), in the plan before the run; |mean net|, tau mean, p95 at X-10's scaled limits (YP-9); before any return,
  `vol_target.csv`: estimates about scored sessions / 21, decisions before the first estimate, clip counts, priced_share
  near 1. Acceptance (registered): paired S2 net dSR > 0 against X-10 (the fixed-L book at the same cap: the hypothesis)
  AND X-10's leverage rule against Y-F0 (net annual return higher AND S2 net Sharpe not lower by more than .100) AND
  mechanics. If accepted it replaces X-10 as Y-F. Composition: Y-3's `--rank-shape` is not in Y-1's refusal list;
  on a two-speed parent the scaler's L_t replaces the run's L in the plan of the netted target, its sigma reads the
  net book, and the virtual fast sleeve stays at the run's L (YCOMB "Y-5 / Y-1 composition", `3ff73201`; YP-15).
- **Report**: Y-F0, X-10 and Y-1 side by side, each with net annual return, net Sharpe at 1x and at 4x NAV, realised vol
  and maximum drawdown; mean L_t for Y-1. Which book is deployed is the owner's risk decision (PM7-3). Both cells count
  in N_c and V; their dSR and both p are printed; neither is in the Sharpe claim. A NAV that reads `--risk-model` cannot
  be read on the history (OD-3 runbook stops on it): Y-1 gets no OD-3 read (YP-11).

## 9. The Y-S fields build ("fields v15"; 0 trials; caps W0-i 2,560 MiB / 600 s per process)

Fields v15 = v14 (78 = v13's 75 + `open_adj`, `high_adj`, `low_adj`; built in X batch 2) + `exch_up_365d` (PM8-6
YSIG-b; LIB2's holdings kind `xsw`, on the base since `b63edb31`) + `iv_skew_21`, `stio_chg_q`, `div_init_omit`,
`deal_pending` (YDATA, `prepare_research_fields_ydata.py`) + `conn_ret63` (PM8-11: "built in its own process with
`--reuse` (3-6 min est.)") = 84 fields. The YSIG strings other than `exch_switch` read only v14 fields (YSIG section 6).
```
PY="C:/Program Files/Python312/python.exe"
# A, one bounded run: the v14 build's argv with its entry modules registered plus the YDATA draft entry (XWQ section 7
#   pattern; `import prepare_research_fields_ydata as y; y.register(vars(b))`) and LIB2's xsw kind (LIB2 section 3), and
--fields <v14 names>,exch_up_365d,iv_skew_21,stio_chg_q,div_init_omit,deal_pending --reuse <v14 dir> --reuse-sha256 <v14 pin>
--price-source C:/Users/natha/Downloads/TickerHistory3.parquet --mgr13f-stage <alpha_panel/v1>/thirteenf
--mgr13f-stage-sha256 <the pin its --thirteenf-sha256 uses>      # the SEC options are already in the v14 argv
#   -> build-equity/train-2020-2023-lo3-fields-v15a   expect 78 reused / 5 computed
# B, its own process: A's argv with
--fields <A's 83 names>,conn_ret63 --reuse <v15a dir> --reuse-sha256 <v15a pin>
#   -> build-equity/train-2020-2023-lo3-fields-v15    expect 83 reused / 1 computed  (= "fields v15")
```
Checked before any string reads it: every reused payload bit-identical to v14 (hardlinks) and the reuse counts above;
`seal.exclusive_end` 2024-01-01, sealed rows dropped and counted; `source_checks.ivshape.orientation_line` = SPY's
vendor line; `source_checks.mgr13f.thirteenf.per_quarter` `short_term_filers` about a third of the classified filers;
peak and wall under the caps (estimates: stio_chg_q 0.3-0.5 GiB over 29 quarter scans, div_init_omit about 0.4 GiB,
iv_skew_21 one TickerHistory3 scan 1-2 min; conn_ret63 0.6-0.9 GiB). If A exceeds 600 s, `stio_chg_q` moves to its own
`--reuse` process the same way (a build split, 0 trials). `shD1` is read only after PM8-8 (3)'s check (P7). The v15
manifest SHA-256 is pinned by the PM's ruling (P11). Registry rows (L2) before add-alpha: `ea_window_pre5`, `noa_lag4`
(YSIG drafts), `ret_overnight`, `ret_intraday`, `exch_up_365d` (LIB2 verbatim), `ins_cluster_buy` (YSIG 2b.4),
`iv_skew_21`, `stio_chg_q`, `div_init_omit`, `conn_ret63` (YDATA rows), `deal_pending` (no row printed: root drafts it
from YDATA section 4); `clock` and `basis` copied from the v15 manifest (precedent `297c5d55`).

## 10. Theme registry change and the iv_vol_of_vol clock repair (0 trials each)

**`merger_arbitrage`** (PM8-8 (2): "approved as the 13th theme, appended last in Python and the C++ `theme_resid_order`
(PM7-39 precedent: one build + identity run)"). (a) Registry themes: appended last with YDATA's L1 text: "Risk-arbitrage
spread: targets of pending mergers and tender offers, from the target's own SEC merger filings until delisting or
deregistration"; the fitter's theme order: last, after `price_volume`. (b) `atx-impl/src/strategy_ic_theme_resid.hpp`
`theme_resid_order`: `merger_arbitrage` as the 13th and last entry (pin R6B-O-4 keeps the Python and C++ orders equal;
its test moves with it). (c) Built under the new build tag of P5 (one build with YCOMB's C++); identity (PM7-30,
PM7-39): X-F0's w pass, fit and NAV reproduce under it before fields v15 and Y-S. Last place leaves every existing
theme's residual under theme-resid-v1 unchanged (PM7-15 (a)). The theme gets members only through Y-S (`deal_target`); a
registered theme with no kept member must drop out of the fit (no theme composite, no share): checked at the Y-S fit (a
refusal is a tool defect, rule 7). Half-life: section 11 and YP-10.

**`iv_vol_of_vol` clock** (PM8-8 (6): "R-12 `iv_vol_of_vol` reads `iv_atm_21d` one session early under the IV clock ->
root corrects the lag to the IV-clock convention of `iv_rv_spread_xe` before the screen (a defect repair, 0 trials, one
variant), string SHA re-pinned and logged"). The registered string reads `iv_atm_21d` rows t-20..t; the vendor delivers
IV at 22:00 America/Chicago (03:00-04:00 UTC of t+1), after t's 22:00 UTC mark (XDATA section 1; atx-db TIER1_V3_STATUS
"IV clock", open). Note: `iv_rv_spread_xe` reads `iv_atm_21d` at row t too (`ts_backfill(..., 5)`, no delay), so its
string defines no lag-1 convention; the house lag-1 IV convention is XDATA's `beta_dvol_21` (rows t-21..t-1) and YDATA's
`ivshape-lag1-v1`. Recommended repair (YP-5), every read delayed one session:
```
rank(decay_linear(((-1 * (ts_std_mp(delay(iv_atm_21d, 1), 21, 12) / ts_mean_mp(delay(iv_atm_21d, 1), 21, 12))) + (0 * log(ts_std_mp(delay(iv_atm_21d, 1), 21, 12)))), 21))
```
SHA-256 `4d42a72b859c3ff4ae69ed82cf34c8263d9877c4f0600b03c6cc32887ac5fac0`. If K1 refuses it on the slot budget, the
same semantics with one outer delay is the mechanical rewrite (YSIG section 6 rule), SHA-256
`1ab6965b4b4a0d3d4e1331e509854306a0930a08086a79748cf2e4b07a909bdd`:
```
rank(decay_linear(delay(((-1 * (ts_std_mp(iv_atm_21d, 21, 12) / ts_mean_mp(iv_atm_21d, 21, 12))) + (0 * log(ts_std_mp(iv_atm_21d, 21, 12)))), 1), 21))
```
In the argv only `--dsl` and `--deviation` change (the deviation gains "; IV rows t-21..t-1, the vendor IV clock (PM8-8
(6) repair)"); the line SHA-256 is re-pinned with the string, `ysig_check.py`'s LIB2-equality line for this id is
updated (tests only), and the repair is logged before the Y-S screen. Outside Y: the roster member `iv_rv_spread` reads
the same row; a member change is not on this list (PM7-14); listed for the PM as a v9 item.

## 11. The half-life table pinned (PM8-5; consumer: Y-5)

| theme | YSIG Appendix A (class, half-life) | YCOMB (half-life, bucket) | recommended | Y-5 bucket |
|---|---|---|---|---|
| value | slow, 252 | 252, slow | 252 | slow |
| profitability_quality | slow, 252 | 252, slow | 252 | slow |
| investment_issuance | slow, 252 | 252, slow | 252 | slow |
| earnings_momentum | slow, 31 | 63, slow | 63 | slow |
| price_momentum | slow, 126 | 126, slow | 126 | slow |
| low_risk | slow, 126 | 252, slow | 252 | slow |
| short_interest | slow, 126 | 63, slow | 63 | slow |
| reversal_seasonality | fast, 5 | 5, fast | 5 | fast |
| options_implied | medium, 21 | 21, slow | 21 | slow |
| ownership_flow | slow, 63 | 63, slow | 63 | slow |
| filing_events | slow, 126 | 21, slow | 21 | slow |
| price_volume | fast, 3 | 5, fast | 5 | fast |
| merger_arbitrage | - | - | 126 [YPRE] | slow |

YCOMB (`97eb5d0f`) cites PM8-13 as pinning the table; its text was not given to this lane. If PM8-13 pinned YCOMB's
table, the recommendation below restates that ruling and only the `merger_arbitrage` row (YP-10) stays open.
Recommendation (YP-4): pin YCOMB's table (as coded in `strategy_two_speed.hpp` `two_speed_half_lives` and
`composition_two_speed.py` @ `e2ac7d63`) plus `merger_arbitrage` 126 (half the field's 252-session window; any value
over 10 gives the same bucket; YP-10). Why: Y-5 is the table's only consumer and its constants are YCOMB's; theta_f =
1 - 2^(-1/5) = .12945 needs one fast half-life, and YCOMB's table gives 5 to both fast themes where YSIG's gives 5 and 3
(two rates, an unregistered choice); under the bucket rule (fast iff half-life <= 10, YCOMB) both tables put every
theme in the same bucket, so the five differing values (earnings_momentum, low_risk, short_interest, filing_events,
price_volume) move no bucket and no rate. YCOMB reached the same reading independently ("the two tables give the same
rule bit for bit"; "Y-5 wiring"). Class rule for Y: YCOMB's bucket; YSIG's three classes are printed for information
(its Appendix A labels reversal_seasonality "fast" at 5, which its own rule calls medium). Half-lives are per theme;
YSIG's suggested member override is not adopted ("the sleeves split theme composites, and splitting a theme would undo
its standardisation", YCOMB), so the calendar and slow members of reversal_seasonality (e.g. `seasonality_same_month`,
`div_season`, `season_y2_5`, `inst_persist`, `day_rev_freq`, `conn_rev`) ride the fast sleeve; Y-5's printed turnover
and cost show the price -- cost if wrong: none on Y-5's arithmetic; a member-level split would be a second Y-5 variant
(v9).

## 12. What voids a cell

Void = never accepted, never a parent, never retried. If any return statistic of it was seen it is ledgered normally
and stays in N_tot and V; a cell stopped before any return read is not ledgered and adds 0. Void when: (1) it ran before
the PM ruled on this file, before the P1-P12 preconditions, or before the Y list was pinned; (2) any input differs from
the Y list or its template (string, constant, the iv_vol_of_vol string before its re-pin, parent not the last accepted,
flag, fields dir other than v15, L other than PM6-6's correction, 2.0 for X-10 and Y-1, the risk store pin); (3) a
return, IC, Sharpe or NAV statistic of the cell was read before its mechanics passed (cells-brief order; the driver's
`verify` stage); (4) a session at or after 2024-01-01 was opened (also a hidden-data disclosure), including a field
read before its seal check (section 9) or `shD1` before PM8-8 (3)'s check.
Not voids: a tool or data defect is rule 7 (invalid, out of N and V; blind re-run 0, returns re-run a new trial); gross
not matched after PM6-6's two corrections, or L < 1, is a mechanics fail (rejected, counted); undefined on its parent is
0; a driver stop (YINFRA plan section 3) is a stop for a ruling, not a void.

## 13. Preconditions before the first Y measurement (in order)

- P1 X closed to X-9: X-7, the campaign and X-9 ledgered, undefined or void by ruling; X-F0 named (section 1); N_c,
  N_tot and M at its state recorded (`expect.n_before` and the base of section 5's ceilings).
- P2 The PM's rulings on this file (Appendix B); PM8-1..PM8-11 appended verbatim to progress.md (draft line 1).
- P3 Merges by SHA in PM8-9's order: YINFRA `2697c3e1` first ("pure tooling, flag-absent identity; suites 273/0": `git
  diff 798d3b23 -- scripts/research_cycle.py scripts/research_add_alpha.py` additions only; `scripts/tests` 0 failed),
  YSIG `5431831a` (`ysig_check: PASS`), YDATA `e1cf4135` (new modules only; `atx-engine/tools` 340 passed), YCOMB
  `97eb5d0f` (Y-5 wiring and the Y-5 / Y-1 composition included), YOPS if delivered (at this writing `19d58b6d`:
  the `group_sum` op, no report; no Y string reads it, PM8-6 YSIG-c; its identity is the golden `0x889874a3b9b29c55`
  at 1 and 4 workers and `AlphaVmSlotReuse.*`). No path is touched by two Y lanes (checked per lane head against `798d3b23`): 0
  predicted conflicts among them.
- P4 Registry: theme `merger_arbitrage` (section 10), the L2 rows (section 9), `max_roster` 96 (PM8-6) with the
  regenerated recipe re-pinned (PM7-13 identity: golden and pinned bench).
- P5 One build under a new tag: YCOMB's C++ (`atx-impl-strategy-target-tests`, `atx-impl-strategy-ic-tests`,
  `atx-impl-tests`, the engine book group, `atx-equity-strategy`, `atx-equity-strategy-ic`), the 13-entry theme list,
  the two-speed table with `merger_arbitrage` (YP-10) and YOPS's op. Suites: `VolTarget.*`, `BookVolTarget.*`,
  `NormScore.*`, `BookNormalScore.*`, `TwoSpeed.*` (the Y-5 / Y-1 composition included), `BookTwoSpeed.*`,
  `TwoSpeedRunner.*`, `RiskTarget.*`,
  `ThemeTsmom.*`, `ThemeTsmomRunner.*` and the regressions YCOMB names. Identity (PM7-30): X-F0's NAV argv
  byte-identical with every Y flag absent; fit identical except `script_sha256` (and PM7-30's `admission_sha256`,
  `provenance/std/registry_sha256` substitutions); w pass byte-identical.
- P6 Release IC exe (PM8-9): root confirms the byte-identity lines (log 3344-3358), builds `atx-equity-strategy-ic` in
  `build-equity-rel` at the P5 tag, and re-runs X-F0's u and w passes under it byte-identical (PM7-30); cell specs name
  `exes.ic` Release and `exes.nav` Debug (the NAV differs by one ULP in Release, log 3359-3365).
- P7 Blind checks before the screen (outcome rule YP-6): Lee-Swaminathan Table II sign (`mom_turn`) and Soliman's sign
  (`dato`) against the citations; MTW's 504 / 252 windows (`div_event`) and the 252-session deal horizon
  (`deal_target`); the TickerHistory3 `shD1` semantics and the look-ahead probe (PM8-8 (3)); the iv_vol_of_vol repair
  re-pinned and logged (section 10).
- P8 Fields v15 built and checked (section 9). The parent's ref on v15 reproduces X-F0's ledgered S2 daily SHA-256 (the
  parent reads no new field), else a rule-7 stop.
- P9 K1: the 15 strings plan on v15 (`--plan-only`), YSIG's predicted rows (section 6 of its report) printed against the
  executable's; a refusal is rewritten mechanically or withdrawn at 0.
- P10 The IC-pass memory re-probed on the Y-S screen library and the marginal cap set (YP-12), mechanics only.
- P11 The Y list pinned: the PM's ruling completes section 14 (the re-pinned iv_vol_of_vol string and argv, the v15
  manifest) and records this file's SHA-256 in progress.md; the 15 queue files pinned (`candidates pin --by PM --ruling
  PM8-...`) with section 14's DSL SHA-256.
- P12 Driver trust (PM8-9): Y-S runs stage by stage (`wave run --until <stage>`), root comparing the first stage against
  a hand plan; the "b" reuse of the screen's marginal rows verified on Y-S (YP-14); the scoreboard's 4x row checked on
  one real `capacity_curve.csv`. Driver use per cell: YP-7.
- P13 Before Y-5 only: the parent's fit, w and NAV argv re-run on the P5 build reproduce with `--two-speed` absent
  (YCOMB "How root verifies flag absent"), and the cell's own w pass reproduces the parent's combined signal byte for
  byte (`TwoSpeedRunner.*`); `parent` set in the `y-two-speed.json` copy.
- P14 Before Y-1 only: the risk store's role pin equals Y-F0's role (else built at 0 trials, as R-8's).

## 14. The Y list (precondition P11; pinned by root at the PM's ruling)

Pinned before any Y measurement (no Y cell, screen or field build has run; no Y statistic exists). Basis: PM8-6 (YSIG-a,
b, c; roster 96), PM8-8 (YDATA 4 strings, theme, shD1, the clock repair), PM8-10 (cells and order), PM8-11 (`conn_rev`).
Lane reports (path @ commit, file SHA-256): YSIG `task-YSIG-report.md` @ `5431831a` (`7972288d...16147`); YDATA
`task-YDATA-report.md` @ `e1cf4135` (`96639115...30e50`, round 2 included); YCOMB `task-YCOMB-report.md` @ `97eb5d0f`
(`0f5c38c6...cfa5ce`, "Y-5 wiring" and "Y-5 / Y-1 composition" included); YINFRA `task-YINFRA-report.md` @ `dde31df3`
(`38fd6ba4...7ee91`; code `2697c3e1`). Fields of every Y cell: v15 lo3 (section 9; manifest pinned at P11). Every
add-alpha call carries the lane's frozen argv; root fills only `--parent`, `--name`, `--parent-spec`, `--fields`,
`--plan-json` (PM6-9) and the driver's opt-in `--save-plan` (YINFRA; no registration byte changes). Hand-written
admission trials: 15 (Y-S).

| cell | id | class | string or rule | constants (theme, tier, sign; flags) | source | SHA-256 of the DSL / rule text |
|---|---|---|---|---|---|---|
| Y-S | `peer_mom_1m` | new (YSIG) | YSIG section 4 | price_momentum, B-, +1 | YSIG 4 (Y-1), argv 6 | `67d4ff4a86c439d7734f7cf6ceff56318137ee1565a3729430a736caada2de77` |
| Y-S | `so_wang_rev` | new (YSIG) | YSIG section 4 | reversal_seasonality, B-, +1 | YSIG 4 (Y-2), argv 6 | `c145cd2a7c1f85ef88a9bed87662ee8e3af8c71becb1d2b3f742aacd10cf59c8` |
| Y-S | `iv_vol_of_vol` | carried (LIB2 C-1, PM8-6 YSIG-a); clock repair (section 10) | YSIG section 5, as repaired | options_implied, B-, +1 | YSIG 5, argv 6 | registered `c5ecec15fbdb480700d8db39b5f8042338ce1a49fd6d86de3a2f7c3a7fc9fcc7`; repaired (YP-5) `4d42a72b859c3ff4ae69ed82cf34c8263d9877c4f0600b03c6cc32887ac5fac0` |
| Y-S | `day_rev_freq` | carried (LIB2 C-2, PM8-6 YSIG-a) | YSIG section 5 | reversal_seasonality, B-, +1 | YSIG 5, argv 6 | `a5416c4ea13d4422592b378716d2a11b493d0152d06c2a48afc3b5937e42c5a7` |
| Y-S | `mom_turn` | new (YSIG) | YSIG section 4 | price_momentum, C+, +1 | YSIG 4 (Y-5), argv 6 | `09946f74588e3060c9f54a0bbefde0c173e1bb39efcdbb10e6d95633c522b373` |
| Y-S | `ea_uvol` | new (YSIG) | YSIG section 4 | earnings_momentum, C+, +1 | YSIG 4 (Y-6), argv 6 | `15f682ffd7504fbb249d5cffedc80a8be28ce93e781e41a5843413f7135d515c` |
| Y-S | `dato` | new (YSIG) | YSIG section 4 | profitability_quality, C+, +1 | YSIG 4 (Y-7), argv 6 | `694fbe591a52faf131cebef6b0ccc753b3d30d7a7fa99609ce3f1915a916a20d` |
| Y-S | `fscore_hbm` | new (YSIG) | YSIG section 4 | profitability_quality, C+, +1 | YSIG 4 (Y-8), argv 6 | `f4d3d3fa7f14fb6ff2d61651880837b187bb66429b4051f167593815e839f21f` |
| Y-S | `exch_switch` | carried (LIB2 C-3, PM8-6 YSIG-a, b) | `rank((-1 * exch_up_365d))` | filing_events, C+, +1 | YSIG 5, argv 6 | `f433b32af008e74c13f04026537a12f735f636cd3a2dadade94200dcd0263ac3` |
| Y-S | `ins_cluster` | new (YSIG round 2b) | `rank(ins_cluster_buy)` | ownership_flow, C+, +1 | YSIG 2b.4 | `a3dfad407c4cd493562022b50172e18ab764033812844a6b23b9209a788f23d2` |
| Y-S | `smile_slope` | new (YDATA) | `rank(decay_linear((-1 * iv_skew_21), 21))` | options_implied, C+, +1 | YDATA 2e | `063202690eb1f981d789a31206f080318d73bdd7477aff6fe261f1ad32159609` |
| Y-S | `stio_trade` | new (YDATA) | `rank(stio_chg_q)` | ownership_flow, C+, +1 | YDATA 2e | `e845ba078144883ba13eefd48bfce4d1016dbf35e889515d64b46b4f6ccc619e` |
| Y-S | `div_event` | new (YDATA) | `rank(div_init_omit)` | filing_events, C+, +1 (PM8-8 (2)) | YDATA 2e | `2dbe474ef4f26f2d0dd8e3a845a57bdfaae26728b5d58fa747a665ec1540bbe6` |
| Y-S | `deal_target` | new (YDATA) | `rank(deal_pending)` | merger_arbitrage, C+, +1 (PM8-8 (2)) | YDATA 4 | `815952591ce849a82926f3144c0714ee5eba0925d0d7c82971842202e71aaf05` |
| Y-S | `conn_rev` | new (YDATA round 2, PM8-11) | `rank((-1 * conn_ret63))` | reversal_seasonality, C+, +1 | YDATA round 2 | `4a1cffe83a007531b9820da67da1a0938391832c79116c5319cd11e4858bcc45` |
| Y-3 | `norm-score-v1` | construction rule (YCOMB Y-3) | template `scripts/specs/v8/y-norm-score.json` @ `02633038` | no free constant; `nav --rank-shape norm-score-v1` | YCOMB Y-3 | file `693c64f5471ceff6f21320c5bfee92d4f741226cbf7be295b1905e0a569e677f` |
| Y-2 | `theme-tsmom-v1` | composition schedule (YCOMB Y-2) | template `scripts/specs/v8/y-theme-tsmom.json` @ `43745dd7` | lookback 252, lag 3, step 21; `fit --theme-tsmom theme-tsmom-v1` | YCOMB Y-2 | file `9d3548b670547d138b3fdaeec0f170a2861b2a60d777c94788446e6c9b4cd83f` |
| Y-5 | `two-speed-v1` | trade rule (YCOMB Y-5, wired) | template `scripts/specs/v8/y-two-speed.json` @ `e2ac7d63` (code `0fbcb230`, `e2ac7d63`, `3ff73201`) | theta_s .05 (the parent's), theta_f .12945 (half-life 5), buckets section 11; `fit --two-speed two-speed-v1`, `nav --two-speed two-speed-v1` | YCOMB Y-5, "Y-5 wiring" | file `69cf613457ff5b474d56269ff200d3d0211b5f06a0e67162e03b9b3c06b3291e` |
| X-10 | `leverage-L2.0` | leverage cell (PM7-11, PM7-17, PM7-34 (3)) | `change.set {"nav.leverage": "2.0", "nav.output"}` on Y-F0 | L 2.0 | v8x section 8; section 8 here | rule text: this file section 8 |
| Y-1 | `vol-target-v1` | leverage rule (YCOMB Y-1) | template `scripts/specs/v8/y-vol-target.json` @ `47d6afd9` | cap 2.0, floor 1, cadence 21, annualisation 252; `nav --vol-target vol-target-v1 --risk-model <store> --risk-model-sha256 <pin>` | YCOMB Y-1 | file `4e190f5434dfc13aced1459721cde476faf3560ef99aa41e05bfe492e2f141b4` |

add-alpha argv of record (SHA-256 of the line as printed in the lane report, `"$PY"` and placeholders included; YSIG and
`conn_rev` print `<Y parent>` etc., YDATA round 1 prints `<X parent>` etc.: the same four slots):

| id | argv line SHA-256 | id | argv line SHA-256 |
|---|---|---|---|
| `peer_mom_1m` | `89e07be1e33db181185fd5971cd56ba0df1fd0534f4637fb1b4811adc56f1d82` | `ins_cluster` | `2c7272b4d4e4c852e400d5077ae223009f0865a60f4ab9639d81cb00b6368c8f` |
| `so_wang_rev` | `d7830943610fc0798c47da82cb7064b42b7b797e7484f7d2b041ef63dd1bf5a1` | `smile_slope` | `bf1a6f07d786c4fc47c17a9b7ee447d794d628ad4f1aefd3439581f24cb53727` |
| `iv_vol_of_vol` | `06ef2910b06dcc1ef6bcad444dc8d9ed1107d765f3b524ec9c3c668fbdad37f8` (re-pinned with the repair) | `stio_trade` | `7dfca7c66bb61edc8e32a000867dee1fe6def83c3c156298e554b621ef8b8c55` |
| `day_rev_freq` | `f57d3a76a9df22a1aa455107c9596133dc636899e3e0078a918b1fa818a2e434` | `div_event` | `6e7436c951616feada7fb6a29656e2600bf1ef9c76ffa09afdc3a9f4ccb3dac4` |
| `mom_turn` | `0a53d97cc2f24308f7c50b00ac90d7f18316c75a5d9099e3f8fa1fc3a01fb63f` | `deal_target` | `1caa171ac90d2ce81e8abb7e5253462efa3cb35b993f1ea445fae53db24ac2a4` |
| `ea_uvol` | `9f61609229cf55d7bf8724e0d659d117c38ac96faae8882564ce323a946327eb` | `conn_rev` | `2afdabf99a96d7deaf43747a0316a5131345422ec3466cdcdf97594d81f98aa4` |
| `dato` | `61728bcc94dcc87f80c4f8022be9c0a76da87c60b0a04a2564037e9fc12fb7a7` | `fscore_hbm` | `ceeed83aa97b7d0fb708f38ea5c6cde19b3780a9a906f8f123d1fe221fdc0703` |
| `exch_switch` | `7f2cac46fb20a586e132f64dffc6bd08f44f819bdae193154487215c6ba5f09f` | | |

Acceptance per cell: section 6 (PM7-34, PM8-10). PM6-6 on Y-S, Y-3, Y-2, Y-5; not on X-10, Y-1. The four YCOMB
templates name `nominal_parent` only: root sets `parent` to the last accepted spec at run time (X-6 precedent; Y-1's on
Y-F0). If YP-10 is ruled, the two-speed table change is a code change pinned by its commit before the P5 build (the
template bytes do not change; its description lists the 12 registered themes).

Appendix A block for Y results: `TRAIN construction cells <N_c>; admission trials this sprint <k> (v8 <k8>, X
hand-written <kx>, mined <km>, Y hand-written <ky>); mined campaigns <0|1> (v9-mine-c1: budget <B>, registry count
<C>, admitted <a>); N_tot <n>; N_hand <h|n/a>; window research-window-v2 (2020-2023); hidden 2024+ unread in this
sprint; validation reads before v8: 2 (2023-2024); history reads <r>; 2025+ never read.`

## Appendix B. Open choices for the PM (recommendation -- why -- cost if wrong)

- **YP-1 books.** X-F0 keeps v8x's meaning and is Y-S's parent; Y-F0 (after Y-5, unlevered) is the claims book and
  X-10's parent; Y-F the levered book -- PM8-3 moved X-10 behind Y-5, so v8x's "X-10 on X-F0" would lever a book
  that skips every Y cell -- cost if wrong: none (names only).
- **YP-2 DSR_hand in Y.** N_tot - M - the mined wave's lines on Y-F0, V shared; n/a if X-9 is in the lineage (v8x's
  H-F value printed instead); computed by `dsr_at` from the printed parts, no tool change -- the prefix rule of v8x
  cannot drop a campaign that sits before later hand-written cells -- cost if wrong: the owner reads "hand" as v8x's
  prefix value; both are printed.
- **YP-3 budget.** X-10 counts once, inside PM8-8's +6; v8x's absolute N_c bound superseded for X-10 only -- cost if
  wrong: none (a ceiling; unused budget lapses).
- **YP-4 half-lives.** Section 11: YCOMB's table + `merger_arbitrage` 126; YCOMB's bucket as the class rule; YPRE
  values for YDATA's five members (section 3) -- cost if wrong: none on any decision (Y-5 uses theme buckets only).
- **YP-5 iv_vol_of_vol repair.** `delay(iv_atm_21d, 1)` at each read (`4d42a72b...`), the outer-delay spelling if K1
  refuses; argv `--dsl` and `--deviation` only -- PM8-8 (6)'s reference string reads lag 0, so the ruling's intent
  (correct the one-session-early read) is met by the house lag-1 convention -- cost if wrong: if the PM meant
  `iv_rv_spread_xe`'s literal lag 0, there is no repair and `c5ecec15...` stands.
- **YP-6 verification outcomes (P7).** A sign or constant its citation contradicts is corrected before any read at 0
  trials, re-pinned and logged (PM8-8 (6) class: the string does not compute its stated definition, PM7-9); a paper
  that cannot be read leaves the registration standing with the risk printed -- cost if wrong: none (no read exists).
- **YP-7 driver use.** Y-S, Y-3, Y-2 and Y-5 through `wave run` (Y-S stage by stage, P12; the rules as `rule_cell`
  templates); X-10 and Y-1 by hand through `research_cycle.py` as in X batch 1 -- the driver has only `v8-mech` limits
  and no leverage acceptance rule, and a new named rule is a reviewed code change (YINFRA plan section 3). At the
  driver's stop after ONE PM6-6 correction, root applies PM6-6's second correction (PM6-6 allows two). Printed
  criteria with no named rule (Y-3's gross return per unit gross; PM7-10's "4x not lower", where the driver has only
  `capacity-4x-higher`) are written by hand from `wave-result.json` -- cost if wrong: hand steps; nothing decides on
  them.
- **YP-8 Y-1 after a rejected X-10.** Y-1 runs (X-10's NAV exists; its rule carries the leverage test against Y-F0) and
  may become Y-F alone; 0 only if X-10 is undefined or void -- cost if wrong: one trial on a managed form whose constant
  twin failed.
- **YP-9 Y-1 mechanics beyond gross.** X-10's scaled limits with r = 2.0 / L_P (|mean net| <= .02 r, tau mean <= .20 r,
  p95 <= .30 r) -- the template restates only gross; Y-1's leverage never exceeds X-10's -- cost if wrong: looser
  limits than the realised mean L_t would give (printed).
- **YP-10 `merger_arbitrage` in the two-speed table.** Append `{"merger_arbitrage", 126.0}` to
  `two_speed_half_lives` (`strategy_two_speed.hpp`, 12 -> 13 entries) and to `composition_two_speed.py`'s table and its
  pin test, by YCOMB or root, before the P5 build, blind -- the IC runner "refuses an unregistered theme", so without
  it Y-5 is undefined whenever `deal_target` is kept in Y-S; the value is a registration made now, with no read --
  cost if wrong: none on the rule (any value over 10 is slow); if not done, Y-5 may lapse at 0.
- **YP-11 OD-3 book.** OD-3 reads Y-F0 (the claims book), never Y-1 (a `--risk-model` NAV cannot be read on history); a
  Y-F0 holding Y-2 or Y-5 needs the OD-3 reader extended first (blind, synthetic tests), else OD-3 reads the latest
  ancestor of Y-F0 it can read, named before the read -- cost if wrong: the history covers a book without the last
  rule.
- **YP-12 Y-S scale.** Re-probe the IC-pass memory on the <= 95-member screen library (X-3: 2,024 MiB at 60 members, 4
  workers; linear scaling gives about 3,200 MiB): over 2,560 MiB, lower workers (4, 2, 1; PM7-12 pattern), cap
  unchanged; the marginal phase cap 360 s -> 720 s for Y-S (report-only phase; X-3 took 175 s at 60 members, and the
  pairwise work grows about with the square) -- cost if wrong: time only.
- **YP-13 roster order.** YSIG's rank order 1-9, `ins_cluster`, then YDATA's `smile_slope`, `stio_trade`,
  `div_event`, `deal_target`, `conn_rev` -- cost if wrong: the admission tie-break only.
- **YP-14 prints and checks.** The adoption print adds X-F0 vs Y-F0 and B0c vs Y-F0 bundles; PM8-9's byte-equal check
  of the reused marginal rows compares the columns that do not depend on the candidate set (`max_rho_member` may name
  a dropped string by design, YINFRA risk d) -- cost if wrong: none (prints, report-only rows).
- **YP-15 Y-1 on a Y-5 parent.** YCOMB `3ff73201` composes them, blind, one way: the scaler's L_t replaces the run's L
  in the plan of the netted target, its sigma reads the net book, F stays at the run's L; flag-absent and zero-share
  identities in `TwoSpeed.*`; "untested on real data". Recommended: accept it as Y-1's registered form on a Y-5 parent
  (verified at P5 with the other suites) -- it was written before any Y read, so no result decides whether Y-1 runs
  -- cost if wrong: if the PM rejects the composition, Y-1 is 0 and logged on an accepted Y-5 (PM8-10 (b)), and the
  managed-leverage cell lapses exactly when the multi-horizon cell is accepted.

## Appendix C. Cross-lane checks (YPRE, synthetic: text only)

- SHA-256 recomputed with Python from each frozen add-alpha line (`--dsl` value): 15 of 15 equal the lanes' registered
  values; YSIG's 16-hex table prefixes 9 of 9 equal; YSIG's ten argv line prefixes reproduced from the printed lines;
  the three carried argv lines equal `task-LIB2-report.md`'s up to the four placeholders; the prose strings equal the
  argv strings. **No mismatch.**
- Inconsistencies found (none changes a frozen byte): (1) YSIG numbers its candidates Y-1..Y-10, colliding with
  YCOMB's cell names Y-1..Y-5: this file names members by id only. (2) Horizon classes differ (YSIG fast < 5 / medium
  5-21 / slow > 21; YCOMB fast <= 10); YSIG Appendix A calls a 5-session theme fast. (3) Five theme half-lives differ
  between YSIG and YCOMB (section 11); no bucket differs. (4) `merger_arbitrage` has no half-life in either table. (5)
  YDATA registered no member horizon (PM8-5 asked YSIG). (6) PM8-8 (6) names `iv_rv_spread_xe`'s convention, but that
  string reads `iv_atm_21d` at lag 0 like the defect it is to repair. (7) The driver allows one PM6-6 correction
  (PM6-6: two) and has no "4x not lower" criterion. (8) YDATA round 1's argv placeholders read `<X ...>`. (9) The Y-1
  template's text says "parent = X-F0"; under PM8-10 (e) it is Y-F0. (10) No L2 row printed for `deal_pending`. (11)
  YSIG's roster arithmetic ("58 + 12 + 10 = 80") leaves out X-9's mined members and YDATA's five; the bound 80 + 15 = 95
  <= 96 holds. (12) YOPS wrote `group_sum`, which PM8-6 YSIG-c listed "as an op ask, not written in Y"; no Y string
  reads it. (13) The two-speed table (`strategy_two_speed.hpp`, 12 themes) lacks `merger_arbitrage`, which PM8-8 (2)
  adds: the IC runner would refuse Y-5 on a book holding `deal_target` (YP-10). (14) At `b713940d` two-speed refused
  `--vol-target`, which would have made Y-1 undefined after an accepted Y-5; `3ff73201` composes them (YP-15). (15)
  PM8-12 and PM8-13 are cited by YCOMB but are not in the PM8 draft given to this lane. (16) YCOMB's report changed
  twice after its first delivery (`4ffd0eb0` -> `b713940d` -> `97eb5d0f`) and YINFRA's code after its report
  (`dde31df3` -> `2697c3e1`, review fixes): section 14 pins the latest heads read at this writing.
