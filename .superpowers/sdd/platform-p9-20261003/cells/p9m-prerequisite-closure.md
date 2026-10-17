# P9-M prerequisite closure and registration proposal

Preparation only at HEAD `3f21e65e846a4f117cd29b528de3f08b30261bf8`, under R15-ROOT4.
No M spec mutation, measurement, diagnostic, test or trial write. Step0 evidence: `cells/start-state-4.txt`.
Ledger135 SHA `7b87d61ccb3285e9ea94aefb52222e610f74287b5559988339eccf09aed07e00`;
N_c64, N_hand125, M110, N_tot235, P9 admission additions0.

## Five exact template prerequisites

1. **Exact:** "the PM has read Y-2 (theme-tsmom-v1) and ruled mom-volman-v1 distinct from Y-2 and Y-1 before any P9-M read (plan DEC-16, lit section 7 restatement risk, E-45 / PM7-14)"

   **Proof:** progress.md:79 DEC-16 rules the different decision units/objects blind, before any Y read;
   :138 records completed Y-2 R0-8 (construction trial aeeb2073e0bd8e2a, report020cc018).
   Prereg B.3:865 explicitly binds both. Existing committed history, no diagnostic repeated. Proposed satisfied.

2. **Exact:** "P9-M registered by the PM in the P9 prereg (plan section 5.2)"

   **Proof:** ruled prereg B.3:865-918, PRE-2BII-RULED progress.md:1034 and final prereg
   review `a32b6f4c`; B.3 fixes id/rule/constants/acceptance/prediction/printed fields.
   PRE-8:520 and R15-ROOT4:1094 require the remaining parent/text/hash registration before execution.
   Proposed satisfied as PM prereg registration; operational registration remains pending PM's precise delta ruling.

3. **Exact:** "lane AL-COMB's combined round reviewed and merged by root (Rulings ALCOMB-5 / ALCOMB-5a: D2's head merged into the lane, the mom-volman-v1 row in D2's K-P9-9 params_schema form, the fixture regenerated from the merged C++; D2 kept composition_theme_tsmom.sleeves, so the sleeve source is unchanged)"

   **Proof:** `task-AL-COMB-rereview-1.md` APPROVE of efb7ab2e, D2 mergeaca3570d,
   fixture6ec102fb; report copied into root at5061579d, root merge9d518317
   (`handoff-root-m2e2.md:37-43`, commit3e283275). Review:80-99 preserves D2's compose/attach
   structure and checks the K-P9-9 schema; :155-161 checks regenerated fixture.
   Current `atx-impl/tests/fixtures/composition_rules_list.json` mom-volman row has type object,
   additionalProperties false, optional typed const rule=mom-volman-v1, lookback126, lag2, step21;
   no required params, so {} uses registered constants. Current `composition_mom_volman.py:99`
   calls `composition_theme_tsmom.sleeves` unchanged. Ancestry exit0 for9d518317/6ec102fb/5061579d;
   reviewer098e6ccd itself exit1 because the report was copied. Proposed satisfied.

4. **Exact:** "wave_kinds.COMPOSITION_FLAGS maps mom-volman-v1 to {fit: {--mom-volman: mom-volman-v1}} (lane E2's table; AL-COMB merge request)"

   **Proof:** current committed `scripts/wave_kinds.py:122` exactly
   `"mom-volman-v1": {"fit": {"--mom-volman": "mom-volman-v1"}}`; progress.md:562
   records E2 merged and MR-4 present. The fixture's row is a schedule with theme_schedule block,
   excludes resid/tsmom; its params schema accepts {}. No new exe/table run needed. Proposed satisfied.

5. **Exact:** "root sets parent to the last accepted cell whose fit is --composition ew-theme-std-v1 or ic-shrink-v1 (with or without --theme-erc), weights price_momentum and carries neither theme-tsmom-v1 nor theme-resid-v1, and runs the cell as an atx.research-wave/v2 kind rule wave whose rules block is [{name: mom-volman-v1, params: {}}] (Ruling ALCOMB-1)"

   **Proof:** H is PM ACCEPTED, trial94a77d4af05ac735, at ledger135. Parent
   `scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json` committedbd837403,
   SHA `1d8eecaa077844d093b05a861db18787ead3223317154c25094cfc98f029e88f`;
   paired NAV `build-equity/mega-nav-p9-theme-cov-hedge-gm`, L1.1674, libraryv8ysb.
   Read-only resolve gives ew-theme-std-v1 + theme-erc-v1 + theme-cov-hedge-v1, prior orientation,
   flag-absent fit, no tsmom/resid. Explicit weights selectors establish a positive price_momentum
   weight and absent theme_schedule/theme_residualise blocks. Weights SHA
   `153943b58587d6023917f410af2088987b185384d25415ae5ceec89850b352ae`.
   PRE-8:520/B.3 explicitly permit this hedge parent; fitter check_args:134-136 and fixture
   incompatibilities exclude only the stated unsupported modifiers. Proposed satisfied
   conditional on the exact parent/rules registration below; no run yet.

Native `research_spec.run_refusal:248-259` treats every retained string as open, irrespective of
these proofs. Proposed PM-authorized closure is /requires=[] on this template only, with all five
exact strings retained above. No source workaround or generic guard suppression.

## Exact closed registration delta proposed

Original template SHA `7e9b26c1bc4a6453447ccbf9fee2612ecb50a293330f239cfc93d035e45db6fb`,
Git blob `d9abc33844064951ade53d5a6c24c438659aacef`.
Original rule SHA `8a7222fa7d99b853abb102f87538fadf2ea4a17b8d22b698a913b64c58e2b493`,
Git blob `8509fcbcca7e96c90e073bb068388539dad32112`. Both are reachable at the starting HEAD.

Only these existing JSON leaves would change:

- `templates/mom-volman.json /parent`: null -> "theme-cov-hedge-v1-attempt2-gm.json".
- `/description`: exactly one substring replacement:
  BEFORE `with or without --theme-erc theme-erc-v1 (the parents the fitter's --mom-volman accepts, composition_mom_volman.PARENT_RULES)`
  AFTER `with or without --theme-erc theme-erc-v1 or --theme-hedge theme-cov-hedge-v1 (the parents the fitter's --mom-volman accepts, composition_mom_volman.PARENT_RULES)`.
  This is PRE-8's hedge-parent correction only. No two-speed addition, engine-fit wording change,
  prediction edit or sleeve sentence cut.
- `/requires`: the five strings above -> [], only on explicit PM ruling.
- `rules/mom-volman-v1.json /manifest_fragment/rule_cell/template_sha256`: original SHA above ->
  SHA of the exact registered LF-byte template. No other rule-file leaf changes; manifest_note,
  its historical requires and stale engine-fit note remain unchanged.

New `scripts/specs/p9/waves/p9m-mom-volman.json` would embed that re-pinned fragment with
schema atx.research-wave/v2, kind rule, rules [{name:mom-volman-v1,params:{}}], accepted H parent,
pinned v15, acceptance pm7-34, gross_match pm6-6-engine, expect n_before64, construction cap71,
native stage timing telemetry, inherited registered budgets including w3584MiB and floor512MiB.
No M native state or generated mom-volman-v1 / -gm file exists; original blob/hash plus the closed
registration diff suffice. No ready clone/attempt namespace is proposed. Native spec/lock/generated
commits remain the supported route after release. PRE-40 B.3/Appendix IV13-15 will record the new
pins and retain full-TRAIN/in-sample disclosure. These are proposals, not edits in this commit.

## Proposed CLI allowance and read boundary for PM ruling

Reuse unchanged sprint `tools/sealsrc.py` at967c2e91, SHA
`fd1aa3e60bf63c0988f74fa3fc0994d4dd9523904bd715079dbf5c7ef3eea551`;
existing37-control evidence applies. No new tests or source class proposed.
The exact anticipated suffix, conditional on PM release and actual per-hit source checks, is:

```text
--seal-allow 20261003=R15-SEAL-C --seal-allow 2026-10-02=SEAL-ALLOW --seal-allow 2026-10-03=R15-SEAL-D --seal-allow 2026-10-06=R15-SEAL-D --seal-allow 2026-10-17=SEAL-ALLOW
```

20261003 is only the exact standalone mandated Git exclusion token in numbered commit-header logs
(class C); ISO02 is only owner PNG filename (A), ISO03/06 only the exact owner PNG entries in the
native dirty-header/parsed receipt array (D); ISO17 only this run's started_utc timestamp (B).
A different actual UTC date requires its own source check and PM ruling, not an inferred allowance.
H's hits establish the generator provenance only. Every actual M hit is classified; OTHER stops
even if its token is on the CLI. Preserve any driver/resource/permission stop, no automatic retry.

Route after separate release:
`"C:/Program Files/Python312/python.exe" -u scripts/research_cycle.py wave run scripts/specs/p9/waves/p9m-mom-volman.json`
plus only the authorized suffix. Environment: PYTHONDONTWRITEBYTECODE=1;
ATX_WAVE_COMMIT_TRAILER="Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>";
ATX_WAVE_COMMIT_EXCLUDE=":(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache".
Immediate bounded launch, no admission-wait/host-budget argument or RAM polling, C:>=15GB,
state free RAM/disk first. No independent DERIV for this v2 cell.

Raw console stays unread; expose only fixed stage/status/time metadata. After completion:
(1) source/pin/void/input/commit-scope checks and six mechanics rows;
(2) hash-bound existing eval bundle, exact M/H pair and frozen eval producer, ONLY sessions/rho/
one_minus_rho/degenerate; record rho first. R15-V2-RHO omission encoding and C.11 fallback stand;
inconsistent encoding/boundary concern stops. No second statistic engine for rho;
(3) select only deciding dSR/mechanics, registered SE and individual p leaves, individual
registered parent/cell net-Sharpe scalars as needed, Appendix A/identity-bridge fields;
then M's own prints: IC composition_schedule_trace block index/sigma_b/target_b/scale_b,
turnover per unit gross, 4x net Sharpe, expected effect +.01 to +.03 [est] (nondeciding).
Do not copy whole lw/cbb/stat objects or H-only gross-alpha/vol-ratio/cost criteria.
(4) Registered sleeve print: bounded frozen Release `atx-research-eval --vol --daily
raw=<role>_mom_volman_sleeve_raw.csv --daily scaled=<role>_mom_volman_sleeve_scaled.csv`,
select each result's net_vol.vol_annual and net_vol.sharpe_annual only. D3W merge08f04e07,
APPROVE1e6c4336/reviewf0726acb, B.3:895-906 support retaining the original sleeve sentence.
These files are the fit's TRAIN decisions, zero outside TRAIN, produced only with a recorded block
in that role; no overlap means no file. This is explicitly an in-sample/full-TRAIN schedule figure,
not an OOS sleeve. No Python derivation or filling a missing result.
(5) native judge writes exactly one construction trial; record ledger/prefix/count/hash and
timing-only scoreboard on this wave's explicit result path. G-P1 remains unawarded.

## Preparation read-scope disclosure and unresolved release

During parent-eligibility metadata inspection, the agent mistakenly printed the entire
H composition_weights.json /theme_standardise object, exposing its inherited theme_erc covariance
matrix, dispersion/sweeps and member shares alongside theme mappings. This exceeded the authorized
prerequisite metadata scope. The tool output remains in the conversation; no matrix values are
copied here and no statistic was recomputed. No M output exists. The agent immediately disclosed
this to PM, stopped broad inspection, and used only explicit boolean leaf selectors thereafter.
PM directed recording the limitation and continuing only this metadata proof, with adjudication
at the registration ruling. No scientific constant, parent choice or criterion changes because
of that exposure. Initial filename/path lookup errors did not execute research or change files.

Await PM adjudication of this limitation, five closures, exact delta and source/read plan.
No M measurement or next-cell work is authorized by this preparation document.
