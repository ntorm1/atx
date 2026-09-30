# Review W1 part 2, area P: everything merged after 7af37e9d

Snapshot `C:/atx-wt/pool-2` at `81abfa12` (code identical to `237486fe`). Range `7af37e9d..81abfa12`: F3 (F-C, F-D),
H1 (era shards tooling), REPORT (v8 blocks, config, template), R6 (E-26), their merges. Test files (area T) and the
sprint directory excluded. Read-only; nothing built, run or edited; no data opened.

Severity: **I** important, **M** medium, **m** minor. "Verified" = follows from code read here; "unverified" says what
would confirm it. No I finding. Part 1 defects that recur in this range are cited by id at the end, not repeated.

---

## Findings

### P-1 (M) A one-era history line is read as a TRAIN cell by every ledger reader except the counters

- **Where:**
  - `atx-impl/tools/backtest_integrity.py:656-661`: one era, history. Writes `era` and window `ERA <id>`, no `era_of`.
  - `:628-635`: `is_era_line` = `"era_of" in rec`. `is_pool_line` = label `POOL`.
  - `:886-892`: `dsr_variance` filters only those two.
  - `:900-912`: `ledger_net_series`.
  - `scripts/research_ledger.py:80-95`: `cells` skips only lines with `era_of` or `eras`.
  - `atx-impl/tools/nav_summ.py:908-915`: `ledger_record` runs on every positional dir, before dedup.
  - `scripts/research_roles.py:176-181`: one history role gets a one-era `--pool`.
- **Defect:** a `roles:` spec with one history role (e.g. E1) ledgers one line. It has label `ERA E1` and an `era` block.
  Under `--protocol v8` it also has `window_id` = research-window-v2. It has no `era_of` and no POOL label, so it is
  neither an era shard nor a pooled line. Three readers treat it as a TRAIN cell:
  - (a) `dsr_variance` puts its 2014-2016 S2 net SR into the current-window V[SR]. Prereg item 3 limits V[SR] to cells
    re-run on 2020-2023 plus the v8 cells, and the function's own docstring says era lines are left out. Without
    `--protocol v8` the line enters the legacy variance instead.
  - (b) `research_ledger.cells` returns it. Any later cycle with `summ.cells_from_ledger` (v71.json has it, and so does
    every spec `add-alpha` derives from it) then passes the history NAV dir as a positional cell. With `--ledger` (also
    in v71.json), `ledger_record` → `window_of` refuses "outside TRAIN" and the summ step exits (SystemExit). So the
    summ of every later cell hard-stops. Without `--ledger`, the history SR joins V[SR_n] and PBO.
  - (c) `ledger_net_series` feeds it to `--effective-n LEDGER`.
- **Multi-era pools are handled:** their era lines and POOL line are skipped everywhere.
- **Blocks:**
  - the freeze gate's cell-count DSR (`deflated_ledger`, prereg item 9), after any one-era history read;
  - the summ step of every later cycle that lists ledger cells.
- **Fix:** one predicate `is_history_line` (`era` in the line or label starting with `ERA `). Use it in
  `dsr_variance`, `cells` and `ledger_net_series`.
- **Status:** verified by reading, not run. Reachable only after an OD-3 read on a single history role; none in v8.

### P-2 (M) Pitch config and scorecard template drop registered parts of the R-5, R-6, R-7 criteria

- **Where:**
  - `docs/plans/mega-alpha-v8-pitch.config.json:90-103` (R-5, R-6, R-7) and `:70` (R-1);
  - `docs/plans/mega-alpha-scorecard-v8.template.md:53-59` and `:113-115`;
  - against plan section 9 (lines 698, 780, 799, 821) and rulings E-14 and E-31.
- **Defect, per cell:**
  - **R-6:** `checks` holds only `cost_bps_traded le`. Two registered parts are missing:
    - "tripwire clear" (plan R-6);
    - "mean correlation of the traded book with the aim over scored decisions >= .9" (E-14; see A-4 for which
      correlation the code computes).

    E-31's validity rule (`limits_unmet > 0` on scored decisions of the primary book) appears nowhere either.
  - **R-5:** omits "S3 not lower".
  - **R-7:** omits "turnover not higher".
  - **R-1 (m part):** compares executed `tau_gmv_mean / mean_gross_leverage_all_rows`. The plan says "planned turnover
    per unit gross".
- **Effect:** the ladder's rule column (`mega_report/v8.py:515`: dSR > 0 AND mechanics AND criterion) can read "true"
  for a cell that fails its registration. Example: R-6 with dSR > 0 and lower cost but aim correlation .8. The
  template's criterion column asks the PM for the same incomplete list.
- **Blocks:**
  - v8_ladder's rule for R-5, R-6, R-7;
  - scorecard section 2a;
  - the acceptance of R-6 (prereg item 5).
- **Fix:**
  - R-6: E-14 as a metric check on the spo-v3 summary, plus the tripwire and `limits_unmet` as checks;
  - R-5: S3 as a check on the S3 scenario's net Sharpe;
  - R-7: `tau_gmv_mean le`.
- **Status:** verified by reading the config and template against plan section 9 and E-14.

### P-3 (M) The ladder never checks recorded verdicts or parents against the registered rule

- **Where:** `atx-impl/tools/mega_report/v8.py:520-551` (`ladder_checks`), `:464-517` (`ladder_rows`), `:336-360`
  (`paired_of`); the `parent` keys of the config.
- **Defect:** `ladder_checks` refuses only three things:
  - a paired test that was read while the verdict is pending or missing;
  - a `v8.final` that is not the last cell whose recorded verdict is accepted;
  - a top-level `final` that differs from it.

  It never checks the recorded verdict against the computed rule, in either direction:
  - a cell recorded `accepted` whose `rule` is false or n/a;
  - a cell recorded `rejected` whose rule is true.

  It never checks that each cell's configured parent is the last cell accepted before it (plan section 9: "Parent = the
  last accepted cell"). `paired_of` only checks that the bundle's base is the configured parent's directory.
- **Failing scenarios:**
  - R-6 is recorded "accepted" while its row shows dSR -.02. The page renders, R-7 stays parented on R-6, v8.final is
    R-7, and the freeze gate is computed on a chain item 5 rejects.
  - R-6 is recorded rejected, but R-7 is still configured on R-6 and its paired file matches. No refusal.
- **Blocks:** the pitched accepted chain and the identity of V8-F (prereg item 5, plan section 9).
- **Fix:** in `ladder_checks`, refuse:
  - `accepted` with `rule is not True` (an n/a rule needs an explicit override text in the config);
  - `rejected` with `rule is True`;
  - a parent that is not the last accepted key before the cell.
- **Status:** verified by reading.

### P-4 (m) Freeze gate and ladder accept an unregistered bootstrap

- **Where:** `mega_report/v8.py:356` (`registered`), `:611-614` (a note only), `:567-588` (`freeze_gate`).
- **Defect:** a bundle made without `--protocol v8` (seed 20260927, 2,000 draws) still fills `paired_p`, and the gate
  can read MET with only a note beside it. Per-cell paired files are never checked against prereg item 4.
- **Fix:** the `paired_p` item passes only with the registered bootstrap (else None), and every ladder row gets the same
  note.
- **Status:** verified.

### P-5 (m) era_data_audit's "opens no return" rests on a name-token regex

- **Where:** `atx-impl/tools/era_data_audit.py:49`, `:60-62`, `:205-214`.
- **Defect:** only names with a `ret` / `return` / `fwd` / `forward` token are skipped. Return-derived payloads such as
  `vol_126` and `coskew_60m` are streamed.
- **Impact:** only finiteness and counts are used or reported, so no statistic leaks. But the contract and its
  file-open test hold only for this heuristic.
- **Fix:** an explicit `return_derived` flag in the field specs, or an allow-list.
- **Status:** verified by reading; the audit never ran on data.

### P-6 (m) Ruling E-28 has no implementation at the reviewed commit

- **Where:** `scripts/research_cycle.py:181-184` (`RUNNER_PHASE_RULES`).
- **Defect:** a w pass on a role of more than 1,200 dates gets 2,560 MiB. E-28 rules 3,072 MiB for `ew-theme-std-v1`
  (admission about 2,606 MiB [est]). The cap sits in unmerged A2 `79440cfa` (status 2 section 4).
- **Blocks:** the R-1 w pass on the 4-year role until A2 merges (a refusal, not a wrong number).
- **Status:** verified by grep; known.

### P-7 (m) The scorecard template joins rows by position

- **Where:** `docs/plans/mega-alpha-scorecard-v8.template.md:127-135` and `:140` (`year_table.0..3` and `years.0..3`
  labelled 2020..2023), `:170` (the alpha table).
- **Defect, years:** nav_summ writes one row per year that has return rows, so a cell with no 2020 rows shifts every
  year label.
- **Defect, alpha table:** it zips `ADM.candidates[i]`, `RECIPE.members[i]`, `CARDS.candidates[i]` and `W.weights[i]`.
  `W.weights` is a dict keyed by id (`fit_composition_weights.py:2180`), and the lists come from three files in their
  own orders. A row can carry another member's theme, tier, ic_theta or weight.
- **Fix:** select by year and by id with the grammar's existing `[KEY]` selector.
- **Status:** verified.

### P-8 (m) An era shard line reuses the era's single-cell trial_id

- **Where:** `backtest_integrity.py:609`, `:664-666`, `:759-764`.
- **Defect:** the era line adds 0 to N, and `ledger_append` dedups by trial_id. If the same NAV series is later
  ledgered as a single cell, that line is skipped and N is one too low.
- **Reachable:** only when a TRAIN era's pooled NAV is byte-identical to a later standalone cell.
- **Status:** acknowledged in the H-1 report's open risks, not fixed.
- **Fix:** era lines get a derived id (e.g. `trial_id("era", pooled_tid + sha)`), or dedup ignores era lines.

### P-9 (m) `summ.cells_from_ledger` is not refused with two or more roles

- **Where:** `scripts/research_roles.py:57`, `:110-118`; `scripts/research_cycle.py:930-934`.
- **Defect:** a pooled history cell is scored beside every ledgered TRAIN cell. `dsr_rows`' cross-cell V[SR_n] and PBO
  then mix a 2014-2019 pooled series with 2020-2023 cells, and cycle_verdict reports that DSR as `dsr.cell_count`
  (C-1).
- **Fix:** add `summ.cells_from_ledger` to the multi-role refusals, and refuse it for a one-role history spec.
- **Status:** verified.

### P-10 (m) aim-partial-v6 still accepts the shaping flags silently

- **Where:** `atx-impl/src/strategy_nav_v7.cpp:716-725` (the refusal sits under `o.spo_v1` only), `:377`
  (`form_aim_v6(member, desired, ...)` reads the shared desired target).
- **Defect:** E-26 closed this gap for spo-v1/v2 but not for the v7 rule aim-partial-v6.
  `--rule aim-partial-v6 --hold-band .1` shapes v6's desired target with no refusal.
- **Status:** verified that v6 reads the shaped vector. Unverified: whether v6's relabelled rule id carries the
  `+hold-band` suffix.
- **Fix:** refuse both flags with aim-partial-v6, as for spo-v1/v2.

### P-11 (m) The v8 report's content seal skips values it cannot parse

- **Where:** `mega_report/v8.py:1141-1154`, `:125-129`, `:132-146`.
  - `_csv_last_session` skips a `session_ns` that `int()` cannot parse (e.g. `1.7e+18`).
  - `refuse_sealed_years` ignores non-numeric years, and `paired_of` passes `years[].year` through unchecked.
  - The JSON check looks only at keys named `session_ns`.
- **Impact:** the NAV writer emits integer sessions, so no current input hides a sealed row. This is defence in depth
  only.
- **Fix:** refuse a row whose session does not parse, and coerce years to integers.
- **Status:** verified.

---

## Rulings and identity claims checked (reading only)

| item | result |
|---|---|
| E-17: pooled re-admission, explicit mask, era window rule, no runner change | conforms (`fit_composition_weights.py` `pool_eras`, `fit_prior(pool=)`; `backtest_integrity.window_of`); no C++ in H-1 |
| E-20, E-30 (F-C, F-D) | conform. Checked: sale_ttm ratio, 12 of 16, me_company t-1, member peers, per-measure medians, ties-inclusive tercile, +-20 d later tie, denominator mean(abs EPS q-4, q-8), strict sign, NaN on 0. Point in time: `QuarterIndex.rows` admits a pair only if its first row is at or before r, and takes the latest row of the pair at or before r; `LatestRows` uses mark(t-L); `load_events` drops sealed rows. Host loaders are in the fingerprint (`self.h.X` is a handle, `code_fingerprint.py:146-149`) |
| E-26 | conforms for spo-v1/v2 (refusal for any value and position). Flags absent: only the in-memory `last_aim` and console counters change (`Timing` is not serialised). Gap for aim-partial-v6: P-10 |
| E-28 | not implemented in this range (P-6) |
| nav_summ without `--pool` | identical: one segment, `return_mask`, `deployment_rows`, `post_ramp_rows` and `integrity` reduce to the old expressions |
| fitter without `--era` / `--era-id` | identical path (`pooled()` false); only `script_sha256` moves |
| research_cycle with `role_key` None | `out`, runner argv, weights name, summ and fit argv, step keys, stop-after, lock: same strings |
| research_fields_v8 without the quarter fields | `check` returns early; the `v8_ff12f49` closure does not reach `OPTIONS` |
| mega_report without v8 blocks | new block names only |
| merge c23f1b43 conflict resolution (fitter, research_cycle) | both sides kept, no extra hunk; 37e84d81, a572d63f, 68d78dc3 have no evil hunks |

## Part 1 defects that recur here (not repeated)

- C-1: the pooled summ's `dsr.cell_count` is the dsr_rows figure.
- C-7: `v8_trial_accounting` prints k = 0 and the caveat quotes it.
- C-13: a pooled fit output and its era weight files are "done" by marker, so an edited era list reuses stale pooled
  weights.
- C-15: applies per era segment.
- A-4: which correlation E-14 reads, relevant to P-2.

## Table of findings

| ID | sev | file:line | one line |
|---|---|---|---|
| P-1 | M | `backtest_integrity.py:656`, `research_ledger.py:80`, `nav_summ.py:908` | one-era history line enters V[SR] and ledger cells; later cycles' summ hard-stops |
| P-2 | M | `mega-alpha-v8-pitch.config.json:90-103`, template `:53-59` | R-6 (E-14, tripwire), R-5 (S3), R-7 (turnover) criteria parts missing |
| P-3 | M | `mega_report/v8.py:520` | recorded verdicts and parents never checked against the item-5 rule |
| P-4 | m | `mega_report/v8.py:611` | unregistered bootstrap only noted; gate can read MET |
| P-5 | m | `era_data_audit.py:49` | "opens no return" is a name regex; return-derived fields are streamed |
| P-6 | m | `research_cycle.py:181` | E-28 cap absent at the reviewed commit (A2 unmerged) |
| P-7 | m | template `:127`, `:170` | positional joins of years and of four member lists |
| P-8 | m | `backtest_integrity.py:664` | era line shares trial_id with the single cell; later ledgering skipped |
| P-9 | m | `research_roles.py:113` | `cells_from_ledger` allowed with pooled or history roles |
| P-10 | m | `strategy_nav_v7.cpp:716` | aim-partial-v6 accepts the shaping flags silently |
| P-11 | m | `mega_report/v8.py:1141` | content seal skips unparseable sessions and years |

Counts: I 0, M 3, m 8.

## Coverage (what was read)

| # | file | lines read | result |
|---|---|---|---|
| 1 | `atx-impl/src/strategy_nav_v7.cpp` | diff; 585-800 | P-10 |
| 2 | `atx-impl/src/strategy_nav_v7.hpp` | diff | clean |
| 3 | `atx-impl/src/strategy_spo.cpp` | diff; 1440-1500 | clean |
| 4 | `atx-impl/src/strategy_spo.hpp` | diff | clean |
| 5 | `atx-impl/src/strategy_spo_v3.hpp` | diff | clean |
| 6 | `atx-impl/src/strategy_nav_replay.cpp` (context) | 2770-2900, 3100-3180 | clean (grid flags do not carry the shaping flags) |
| 7 | `atx-engine/tools/era_pool.py` | 1-175 (whole) | clean |
| 8 | `atx-engine/tools/research_fields_v8.py` | all 8 diff hunks; 510-560 | clean |
| 9 | `atx-engine/tools/prepare_research_fields.py` (context) | 1733-1784 (`load_events`) | clean |
| 10 | `atx-engine/tools/code_fingerprint.py` (context) | 130-200 | clean |
| 11 | `atx-impl/tools/fit_composition_weights.py` | diff; 490-560, 1590-1712, 1788-1860, 2036-2080, 2180-2232 | clean |
| 12 | `atx-impl/tools/nav_summ.py` | diff; 120-320, 425-700, 884-916, 1036-1140 | P-1 |
| 13 | `atx-impl/tools/backtest_integrity.py` | diff; 544-575, 609-700, 723-775, 805-817, 858-912 | P-1, P-8 |
| 14 | `atx-impl/tools/era_data_audit.py` | 1-306 (whole) | P-5 |
| 15 | `scripts/research_roles.py` | 1-261 (whole) | P-1, P-9 |
| 16 | `scripts/research_cycle.py` | diff; 170-195, 587-610, 760-1000 | P-6, P-9 |
| 17 | `scripts/research_ledger.py` | diff; 95-175 | P-1 |
| 18 | `scripts/cycle_verdict.py` | diff; 50-75 | clean (C-1 applies) |
| 19 | `scripts/run_bounded_research.py` | diff | clean |
| 20 | `atx-impl/tools/mega_report/v8.py` | 1-1273 (whole) | P-3, P-4, P-11 |
| 21 | `atx-impl/tools/mega_report/pitch.py` | diff | clean |
| 22 | `docs/plans/mega-alpha-v8-pitch.config.json` | 1-244 (whole) | P-2 |
| 23 | `docs/plans/mega-alpha-scorecard-v8.template.md` | 1-234 (whole) | P-2, P-7 |
| 24 | `docs/plans/2026-09-30-platform-v8-status-2.md` | 40-130 (partial) | P-6 (context) |
| 25 | `docs/plans/2026-09-30-platform-v8-next-goal-prompt-2.md` | 1-80 | clean |
| 26 | merge `c23f1b43` (fitter, research_cycle conflicts) | `--cc` | clean |
| 27 | merges `37e84d81`, `a572d63f`, `68d78dc3` | `--cc` | clean |
| 28 | context: brief, review-w1-A/B/C, progress.md, global-constraints, v8-prereg, plan section 9, task-H brief, task-H-1, task-F-3, task-R-6 sections 9-10, task-REPORT reports; `scripts/specs/v71.json` summ block | read | context |
