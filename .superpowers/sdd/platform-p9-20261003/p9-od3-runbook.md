# P9 OD-3 history read: runbook (`p9-od3-runbook.md`)

Lane OD3P (pool-21), RESUME 7, dispatch R7-1 item 4 (the lane's item 6). Registered text: `p9-prereg.md` section F
(F.1 runbook, F.2 OD3-TOOL); rulings DEC-3, PRE-10, PRE-16 .. PRE-21, PRE-26, PRE-31, OD5-1, ALD-9, OD3P-1,
ALCLOCK-1, ALCLOCK-2; adapted from `v8-od3-protocol.md` section 5 (record in this dir, sha256
`640cd91ecfc0dd8e6b0b3a3da13e87e52b6e361c42fc2347d099b9da6976c9bb`, = the `a7671d1a` blob). Where this file and the
prereg differ, the prereg and the ledger rulings win; where this file and `v8-od3-protocol.md` differ, this file wins.

The runbook is root's, run once (DEC-3). It was written blind: it holds no return, IC, Sharpe or NAV figure of any
period, and every value that does not exist until the P9 cells have run is marked **[UNKNOWN: ...]** with the event
that fixes it. Root fills each one in the read's own record (`progress.md` and the OD-3 print), never in this file.

## 0. Rules that hold for every step

1. **Once.** One read event, after the adoption print, on the frozen P9-F0h with Y-F0 beside it. No era u, w or NAV
   pass runs before the last P9 cell is ledgered (PM7-29 choice 11, carried). Data builds, coverage and mechanics
   checks (steps B and P below) may run earlier. Nothing in any book changes afterwards; a fail is reported, never
   re-run.
2. **`atx-impl/tools/holdout_gate.py` is never run in P9.** It is the OD-1 gate and reads 2024+ (OD-1, PRE-21). The
   plan's citation of it for OD-3 is an error (prereg F.2 item 1). Nothing dated 2024-01-01 or later is opened.
3. **Identity-bridge flag on every print** (OD5-1 (i), text per ALD-9): every OD-3 print, every history line and the
   G-B8 verdict carry, verbatim:
   > universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs
   > through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured

   and beside it OD5-1 (ii): absolute Sharpe / return figures are possibly overstated by survivorship; the evidence of
   record is the paired, relative tests. A print without the flag is a stop (F.1).
4. **History NAVs run through the cycle only** (`scripts/research_cycle.py run <od3 spec>`), never by a hand NAV
   `--spec` / NavSpec. The cycle's PRE-10 check refuses a history role whose nav / ref flags read `--risk-model`
   (`research_roles.py` `check_history_risk_model`); a hand NavSpec with `options.risk_model` is not seen by it.
5. **No book reads `--risk-model` on history** (PRE-10): Y-1, P9-X, P9-V, P9-MV and risk-target are not readable; no
   per-era risk store is built in P9. If P9-MV is accepted the read is on P9-F0h and G-B8 is labelled
   "history-readable book, not the deployable one".
6. **Weights carried, no re-fit** (PM7-29 choice 1; `fit.carry`, `atx-impl/tools/carry_weights.py`): TRAIN admission,
   signs and weights carried back unchanged; each book's L carried; no gross calibration on history, gross printed.
7. **Bounded real-data runs** (root only): `scripts/run_bounded_research.py`, `--seconds` <= 600 (RUN-2), memory as
   RUN-1 (2,048 MiB unless the step is known to need more; the TRAIN fields v15 run used `--max-rss-mib 2560` under
   the runner and `--max-rss-mib 2048` in the child: root reads free memory first and states the figure).
8. **Trial ledger.** Only R0 and R3 write to `build-equity/trials.jsonl`, through the cycle (E-41: each history line
   adds 0 to N and 1 to history reads).

## 1. The books and the four history lines (PRE-16 .. PRE-20, PRE-31; prereg A.4, F.1)

| line | book | spec / base | fields of E3 | carry | decides |
|---|---|---|---|---|---|
| 1 | **P9-F0h** (PRE-10): the last accepted book that does not read `--risk-model` | **[UNKNOWN: the adoption print]** (= P9-F0 unless P9-MV is accepted; = the re-based Y-F0 if no P9 cell is accepted) | **[UNKNOWN: the last accepted cell]** (`train-2020-2023-lo3-fields-v15` unless a cell added a field) | its TRAIN `composition_weights.json` + `admission.json` **[UNKNOWN: the adoption print]** | G-B8 |
| 2 | **Y-F0** (beside) | `scripts/specs/v8/lib-v8ysb-gm.json` (sha256 `1e3ec118273cb52d7248568011e7dc24a211e7204c25b1116f5a5bd7deaa8626`), library `atx-impl/strategies/fund_industry_ic_v8ysb.json` (67 candidates) | `train-2020-2023-lo3-fields-v15` (pin `26fee5ce...`, full value from its manifest at the read) | its re-based TRAIN w dir **[UNKNOWN: wave-3 P9-B0, ROUND 2b]** | nothing |
| 3 | **R-2** (G-B8's reference) | `scripts/specs/v8/lib-v80.json` (sha256 `306a070b865d3625dc03b477e528a63ae52558cc8c8b231d3df787ec3bab3a21`) | its re-based TRAIN fields dir **[UNKNOWN: wave-3 P9-B0, ROUND 2b]** | its re-based TRAIN w dir **[UNKNOWN: wave-3 P9-B0, ROUND 2b]** | reference only |
| 4 | **Clean-source book** (PRE-31): the last accepted book of the lineage that applies no rule whose `source_sample_end` is 2015 or later, with the 12 listed members absent | **[UNKNOWN: the adoption print]** (Y-F0 if no accepted P9 cell is free of the flagged rules) | as its TRAIN cell | its TRAIN fit + `fit.carry.drop` = `scripts/specs/p9/od3/flagged_members.json` | nothing |

- **Clean-source line, the drop (OD3P-1).** A listed member is absent on history exactly as the engine treats a member
  with no data on a date: it adds nothing to the composite and its mass stays in its theme; the TRAIN weights, signs,
  theme blocks and the IC runner's verify are unchanged (theme-erc-v1 / ic-shrink bases are accepted). The cycle
  passes `--absent-members <list> --absent-members-sha256 <pin>` to the u and w IC passes of E1 and E2 only (not E3);
  contract `od3p-absent-contract.md`. If the IC exe of the freeze build does not offer capability
  `absent-members-v1` (the C++ lane not merged and APPROVE at the wave-3 gate), the cycle refuses that spec at plan
  time: the line is then printed **"clean-source book: not read: no producer"** and nothing else changes (the line
  decides nothing).
- **Clean-source print text** (PRE-31, OD3P-1), verbatim on its line: "clean-source book: no flagged member, no flagged
  rule". Beside it: "the absent members' mass stays in their themes, spread over the members present, so this is not
  the same book minus twelve signals; it differs from P9-F0h by rules as well as members, so the split does not
  isolate the members' share; it decides nothing".
- All 12 listed ids are candidates of Y-F0's library (checked by OD3P against the library's candidate ids, no data
  read). The carry refuses an id that is not a member of the base book's weights: for a base other than Y-F0 a
  refusal at B6 is a stop for line 4 only.
- **Flagged rules** (`rules` of the list, PRE-31 / ALCLOCK-2): theme-cov-hedge-v1 (2017), mom-volman-v1 (2016),
  info-clock-rate-v1 (2019). **mv-aim-v1: [UNKNOWN: COV-MV's registration]** (in `rules_pending`; root resolves it
  before the read, step P0).

## 2. Steps before the read (may run while cells run; no u, w or NAV pass)

**P0 preconditions** (each a stop if false; PRE-21, OD3P-1, F.1):
- OD3P merged in root and reviewed (PRE-21).
- `scripts/specs/p9/od3/flagged_members.json`: root moves `mv-aim-v1` from `rules_pending` into `rules` (with its
  `rule_source_sample_end`) if its registration's `source_sample_end` is 2015 or later, else removes it; `rules_pending`
  is then empty and root records the file's new SHA-256 (the carry and the cycle refuse the list while
  `rules_pending` is non-empty). **[UNKNOWN: the re-pinned SHA-256]**
- The freeze build's IC exe: `<ic exe> --list-rules --json` lists `absent-members-v1` -> line 4 is read; else line 4
  is "not read: no producer" (not a stop for the other lines).
- T2 (`nav_summ` pooled one-sided p on the engine verb, OD3P item 3) merged after B2's redo **[UNKNOWN: B2 / T2
  merge]**. T2 is part of the OD3P port: if it slips, OD-3 waits for it (PRE-21).
- The exe and `ucrtbase.dll` records equal the freeze build's (C.6, C.8); every spec names the frozen build.

**P1 pins.** The stage pins (identity bridge `V2PIT`, SIC, delisting `DL`, fundamental events `FEV`, the `w0-2` runbook
variables used in `v8-od3-protocol.md` section 5) equal the input pins of `build-equity/train-2020-2023-lo3/manifest.json`
and of P9-F0h's TRAIN fields manifest; re-hash every stage manifest now. STOP on any difference.

**B1 projection, B2-B5 era roles.** Exactly the argv of `v8-od3-protocol.md` section 5 (B1 `project --start 2013-06-01
--end 2020-01-01`; per era E1 = warm-up 2013-06-01, scored [2015-01-01, 2017-01-01); E2 = warm-up 2015-06-01, scored
[2017-01-01, 2020-01-01)): B2 base role (`--top-n 3000 --max-union 8000`; STOP if score_begin != 399 after one
`--start` move), B3 factor-break scan, root reviews the listed sessions, repair with exactly that list (STOP if
unreviewed), B4 lo3 role (`--universe linked-operating-v3`, identity bridge, SIC, delisting), B5 its dlret label role
(the same plus `--delisting-returns`). Each argv equals the TRAIN role receipt's (`train-2020-2023-base-v1-run`,
`-base-run`, `-lo3-run`, `-lo3-dlret-run`) except dates, outputs and the bound role (prereg F.1 "same argv, era
dates"). **[UNKNOWN: each era role's SHA-256 and the reviewed factor-break session list]**

**B7 era fields** (prereg F.1, ROUND 2b item "the era fields argv derived from P9-F0h's fields manifest"). One build per
era, with P9-F0h's field list (fields v15, 84 fields, the engine-kind rows included; not v10); every book reads its
fields from it (R-2 and Y-F0 read subsets: root checks at P6 that each book's K1 plan reads only fields in the list).
- Source: P9-F0h's TRAIN fields receipt `start.json` (for v15: `build-equity/train-2020-2023-lo3-fields-v15-run/start.json`;
  **[UNKNOWN: the receipt if a cell added a field]**). Its `command` is a list: `<python> -c <program>` and the options.
- The era argv is that list with exactly three values replaced and the reuse options removed:
  `--role <era lo3 dir>`, `--role-sha256 <its manifest SHA-256>`, `--output <era fields dir>`; drop `--reuse X`,
  `--reuse-sha256 X`, `--reuse-hardlink`. Everything else (the `-c` program text, `--fields` and its order, every
  source and pin, `--mgr13f-stage`, `--max-rss-mib`, `--max-seconds`) stays byte-identical. The runner wrapper keeps
  the receipt's `limits` and `bindings` except: the role manifest binding becomes the era's, the reuse dir's manifest
  binding is removed, `--output` is the era run dir.
- Self-check before use: apply the same rule with the TRAIN values (TRAIN role, its SHA, TRAIN output) and compare it
  with the receipt's list minus the reuse options: equal, or STOP. Then diff the era list against the receipt's: the
  only differences allowed are the three values and the removed reuse options. Any other difference: STOP (F.1 "fields
  argv beyond the allowed keys").
- A run refused at the 600 s cap (RUN-2) is a STOP before any return; the PM rules the staging (the TRAIN v15 dir was
  built with `--reuse` of `-v15a`; mirroring that lineage per era is a PM decision, not a runbook default).
- **[UNKNOWN: P9-F0h's fields dir and list until the last accepted cell]**; Y-F0 uses v15 (pin `26fee5ce...`).

**B6 roles specs** (`scripts/specs/p9/od3/od3-{p9f0h,yf0,r2,clean}.json`), each written from the book's resolved cycle
spec as `v8-od3-protocol.md` section 5 "B6 specs" says, with these P9 rules:
- `name` `od3-<book>`; `inputs`: library and recipe only (drop `role`, `label_role`, `baseline_*`, `reference_*`).
- `roles`: E1 `{"id": "E1", "dir": "build-equity/od3-E1-lo3", "begin": "2015-01-01", "end": "2017-01-01", "universe":
  "linked-operating-v3", "fields_dir": "build-equity/od3-E1-lo3-fields-v15", "label_dir": "build-equity/od3-E1-lo3-dlret"}`;
  E2 the same with E2 and `2017-01-01` .. `2020-01-01`; E3 `{"id": "E3", "dir": "build-equity/train-2020-2023-lo3",
  "begin": "2020-01-01", "end": "2024-01-01", "universe": "linked-operating-v3", "fields_dir": <the book's TRAIN
  fields dir>, "label_dir": "build-equity/train-2020-2023-lo3-dlret"}` (E3 is the TRAIN role as built; nothing dated
  2024 or later is read: its end is the exclusive TRAIN end).
- `fields`: `{"output": "build-equity/od3-fields-unused", "manifest_sha256": null}`, no list (every era as built).
- `fit`: the book's own section with `output` REPLACED by a fresh dir of the book's own, `build-equity/od3-fit-<book>`
  (`<book>` = `p9f0h`, `yf0`, `r2`, `clean`), plus `"carry": {"weights": {"path": <its TRAIN
  composition_weights.json>, "sha256": null}, "admission": {"path": <its admission.json>, "sha256": null}}`; line 4
  adds `"drop": {"path": "scripts/specs/p9/od3/flagged_members.json", "sha256": null}` (the clean-source book's spec
  is the carry base, PRE-31).
  - **Never the book's TRAIN fit dir, never another line's dir** (OD3P review R-1). The cycle takes the carry as done
    when `fit.output` holds `composition_weights.json` (`research_roles.py` `carry_step`). The book's own `fit.output`
    is its TRAIN fit dir, the dir holding the carried file (Y-F0: `scripts/specs/v8/lib-v8ysb-gm.json` has
    `build-equity/mega-weights-v8x-theme-erc-v8ysb`): kept, the carry would never run, no
    `composition_weights.E1/E2.json` would be written, line 4's drop-list checks would never run, and the run would
    HARD-STOP at `w:E1` after the fields and the history u passes. Lines 2 and 4 get DISTINCT dirs (`od3-fit-yf0`,
    `od3-fit-clean`) even when both carry Y-F0's TRAIN fit; one shared dir would make line 4's carry "done" from
    line 2, and its list would be neither checked nor recorded.
  - Before P7, none of `build-equity/od3-fit-<book>`, `build-equity/od3-fit-<book>-run1`,
    `build-equity/od3-fit-<book>.pending` exists. The cycle refuses at plan time (exit 2) a `fit.output` that exists and
    whose `carry.json` does not record exactly this carry (W, A, anchor, eras, the drop list's pin or no drop); after
    its own carry has run, the same spec re-plans with `fit` done (resume).
- `ic.u_output`, `ic.w_output`, `nav.output`: `build-equity/od3-u-<book>`, `-w-<book>`, `-nav-<book>`. The NAV rule,
  L and flags stay unchanged. ALCLOCK-1: if P9-C is accepted and AL-CLOCK is merged, the two books of the
  turnover-share prediction (P9-F0h and Y-F0) both name `--info-clock-print` (print-only; only `summary.json` changes,
  by that block); otherwise that prediction prints "no producer".
- Dropped: `static_check`, `ref`, `card`, `marginal`, `monitor`, `compare`, `gate`, `verdict`,
  `summ.cells_from_ledger`. `summ`: the book's own summ section (P9's summ protocol) with `summ.ledger`
  `build-equity/trials.jsonl` and each book's TRAIN `origin`; no `--dsr-total` (T3 dropped, PRE-21).
- No nav / ref flag reads `--risk-model` (the cycle refuses it, PRE-10: rule 0.5).

**Lock and plan (P7).** `"$PY" scripts/research_cycle.py lock scripts/specs/p9/od3/od3-<book>.json --write`, then
`plan`: the plan lists exactly E1, E2, E3 (`fields:E1..E3` pinned, `u:E1..E3`, fit = carry, `w:E1..E3`, `nav:E1..E3`,
summ). The fit line reads `# phase fit [bounded; pending; fit.carry: ...] -> build-equity/od3-fit-<book>`, and the carry
argv (after the runner's `--`) has `--output build-equity/od3-fit-<book>`. STOP for that line unless the fit phase is
`pending` with that `--output` (a `done` or `failed` fit, or a refusal naming `fit.output`, means a reused or stale dir:
OD3P review R-1). For line 4 the plan also shows `# pin carry:drop`, the carry argv ends with `--output
build-equity/od3-fit-clean --drop-list scripts/specs/p9/od3/flagged_members.json --drop-list-sha256 <pin>`, and the E1
and E2 u / w argv end with `--absent-members scripts/specs/p9/od3/flagged_members.json --absent-members-sha256 <pin>`
(E3 without). A plan with other eras, a carry pin, anchor or drop-list refusal (the carry tool's list checks run at plan
time), or (line 4 only) a capability refusal: STOP for that line (the capability refusal makes line 4 "not read: no
producer").

**P2 TRAIN identity.** Each of P9-F0h, Y-F0, R-2 and the clean-source book reproduces its TRAIN NAV byte for byte on
the frozen build (for a book naming `--info-clock-print`, every file but `summary.json`'s `info_clock` block).
STOP on a difference.

**P3 windows.** Each era: score_begin 399; era end <= 2020-01-01; `score_member_counts` >= 1,000 lo3 members on every
scored E1 session; `dropped_by_reason` reported per year.

**P6 K1 plan-only.** Per era and book: the plan's w-pass argv with `--plan-only`, under the caps the book's TRAIN w pass
ran under (the v8 figures were 2,560 / 3,072 MiB). STOP if over.

**B8 / P4 coverage audit** (opens no return; prereg F.1 "coverage by year"), once per book library:
`"$PY" atx-impl/tools/era_data_audit.py --era E1 build-equity/od3-E1-lo3/manifest.json <sha>
build-equity/od3-E1-lo3-fields-v15/manifest.json <sha> --era E2 ... --library <lib> --library-sha256 <sha> --plan <P6
JSON> --json build-equity/od3-audit-<book>.json`. Printed: coverage by field and year, delisted share, mask exits,
first valid date per candidate; the members NaN on each era's first scored session go into `progress.md`.
2015-2017 tests a different book (FINRA short interest, FTD, Russell proxy from 2018; 13F from 2013q2; options from
2012).

## 3. The read (after the last P9 cell is ledgered and the adoption print)

**R0 protocol line** (adds 0): `"$PY" scripts/research_cycle.py ledger-protocol --ledger build-equity/trials.jsonl
--owner-ruling "DEC-3 OD-3 read, p9-od3-runbook.md sha256 <S>, books P9-F0h Y-F0 R-2 [clean-source]" --date <D>`.

**R1 era runs to NAV**, through the cycle only: `"$PY" scripts/research_cycle.py run scripts/specs/p9/od3/od3-r2.json
--stop-after nav`, then `od3-yf0`, `od3-p9f0h`, `od3-clean` (line 4 only if P0 found the capability).

**R2 mechanics only** (no return column; `v8-od3-protocol.md` section 5's R2 one-liner reads `status`, `rule`,
`role_sha256`, `composition_weights_sha256`, rows, post-ramp gross, minimum held names): per NAV dir exit 0, status
`complete`, the role pin equals the era's, the weights pin equals the carry file's; the E3 daily CSV SHA-256 equals
the book's TRAIN ledger series. For line 4 also: each E1 / E2 w-pass `summary.json` holds the `absent_members` record
with the list's SHA-256 (contract section 4), and E3's holds none. STOP on any refusal or mismatch.

**R3 summ** (ledgered pooled lines; E-41: each adds 0 to N and 1 to history reads):
`"$PY" scripts/research_cycle.py run scripts/specs/p9/od3/od3-<book>.json` for each book (the summ phase).

**R4 G-B8** (PRE-17: decided on E1+E2 pooled, 2015-2019; the 2015-2023 pool E1+E2+E3 printed only): pooled rows by
`nav_summ --pool <book E1 E2 NAV dirs> --pool-ids E1,E2` and the paired block P9-F0h vs R-2 by `--pool-reference <R-2
E1 E2 NAV dirs>`, with the one-sided p of the engine verb (`--one-sided`, `eval_cli.cpp:110-111`) through T2
(`--pool-one-sided`) **[UNKNOWN: T2 merged after B2's redo]**. Pass rule: S2 net Sharpe of P9-F0h on E1+E2 > 0 AND
paired dSR P9-F0h vs R-2 on E1+E2 > 0; both p printed. A fail is reported, never re-run; the freeze wording per OD5-1
(v) ("passed with the identity-bridge flag").

## 4. The OD-3 print (one print, every line with the identity-bridge flag of rule 0.3)

1. The four history lines (R3), each with E1, E2 and pooled E1+E2 rows, and the 2015-2023 pool printed only; line 4
   with its text of section 1 or "clean-source book: not read: no producer".
2. G-B8 (R4) on P9-F0h, with the PRE-10 label if P9-MV is accepted.
3. The predictions registered in prereg F.1, each against its outcome **[UNKNOWN: which P9 cells are accepted and
   active on history]**: P9-F0h vs Y-F0 mechanism signs (P9-H: vol ratio < 1, the hedge 0 in each era's first 252
   decisions, PRE-26; P9-C: turnover share up, gross up, with `--info-clock-print` on both books or "no producer";
   P9-M: inert, no effect predicted; P9-L: history labels from each era's own label role, whose delisting stage is
   empty before 2018); P9-F0h's history S2 net Sharpe = its TRAIN S2 net Sharpe + P9-W's dSR (PRE-39; prereg B.6
   as PRE-39 reads it), and "lower than on 2020-2023" (PM7-28's registered expectation, carried); the ERC
   prediction printed "not read" (PRE-18).
4. Producers: NAV exe `ann_vol`, `ann_mean`, `net_sharpe` per era; pooled rows from `nav_summ --pool`; turnover as B.2.
5. Contamination flags: every member of each book and every P9 rule with its `source_sample_end` (prereg Appendix III;
   mv-aim-v1 **[UNKNOWN: COV-MV's registration]**).
6. Coverage by year (B8 / P4) and the per-era mechanics (R2).

## 5. Stop conditions (each before any return is opened; the PM rules)

A P1 pin difference; a P2 identity failure; score_begin != 399; an era past 2020-01-01; unreviewed factor-break
sessions; an E1 scored session under 1,000 lo3 members; a fields argv beyond the allowed keys (B7) or a fields run
refused at the cap; a plan with other eras or a carry refusal; a P7 plan whose fit phase is not `pending` into the
book's own fresh `build-equity/od3-fit-<book>` (R-1); K1 over its caps; a book reading `--risk-model`; the last
P9 cell not ledgered; an R2 refusal, a pin mismatch or an E3 SHA mismatch; an exe or `ucrtbase.dll` record differing
from the freeze build (C.6, C.8); a print without the identity-bridge flag; OD3P (T2 included) not merged and reviewed
(PRE-21); `rules_pending` not empty at the read (P0). Not a stop: the IC exe without `absent-members-v1` (line 4 prints
"not read: no producer"; OD3P-1). After R3 nothing stops the read: the result is printed as it is; a defect found then
is printed beside it and the PM rules whether the read is void.
