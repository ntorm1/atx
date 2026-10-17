# P9 sprint final report

**P9 closeout: registered research is complete; production qualification is not awarded.** The platform and alpha-research tracks have separate outcomes. Final audit and ruling-index evidence accompany this report.

The working tree is `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. The sprint's decisions are in the append-only `progress.md`; [rulings-index-final.md](rulings-index-final.md) is an integral appendix to this report, with every indexed decision's exact ledger line and supersessions, owner items, permission refusals and P10 work. Historical handoffs remain evidence of their own checkpoints.

The owner's expanded goal is recorded in [goal-two-tracks.md](goal-two-tracks.md): build the platform and infrastructure for alpha generation, and use it to find and evaluate actual alpha in parallel. Platform delivery and research evidence have separate outcomes. The parallel [platform audit and implementation brief](platform-track-report.md), committed at `b4c99e8c` and clarified at `2c22d439`, records delivered native computation, registries, continuation, provenance and SQLite capabilities, with their actual limits. Its next milestone prioritizes streaming payload identity, reliable continuation and complete associated catalog provenance. The research track reports the cell and adoption evidence below. A positive TRAIN comparison and higher return from leverage do not establish out-of-sample alpha.

## Completion and book decision

The registered cell chain and adoption are complete. [freeze-verdict.md](freeze-verdict.md) records **NOT PRODUCTION-QUALIFIED**, preserving the unmet platform and unevaluated history conditions. Adjudicated cells are:

| Cell | PM disposition | Trials | Basis |
|---|---|---:|---|
| P9-S | LAPSED | 0 | No new-signal cell registered. |
| P9-L | ACCEPTED | 1 | All six mechanics checks passed; fresh independent DERIV at86fcd5df agreed before the ledger write. Protocol-only acceptance; its paired gain is not a G-B7 step. |
| P9-H | ACCEPTED | 1 | Paired net dSR+0.02554011056222616>0 and all six mechanics checks passed; native pm7-34. Both registered vol/gross-alpha predictions met, print-only expected-effect band missed. [Cell record](cells/p9h-cov-hedge-record.md). |
| P9-M | REJECTED | 1 | Paired net dSR -0.03834150043478579; all six mechanics checks passed. Sleeve volatility-falls prediction unmet and Sharpe-rises prediction met, both print-only and in-sample. [Cell record](cells/p9m-mom-volman-record.md). |
| P9-C | REJECTED | 1 | Paired net dSR -0.13730612827997102; all six mechanics checks passed. Cost-lower and 4x-higher predictions unmet; clock and exposure prints complete. [Cell record](cells/p9c-info-clock-record.md). |
| P9-MV | REJECTED | 1 | Paired net dSR -0.43062895540654544; all six mechanics checks passed. Volatility-falls prediction met, gross-alpha-preservation prediction unmet; both nondeciding. [Cell record](cells/p9mv-mv-aim-record.md). |
| P9-W | LAPSED | 0 | D3-Q1a DIFFERENT; no dSR or missing prediction invented. |
| P9-X | ACCEPTED | 1 | Net annual8.986194230342487% exceeds H5.689681785648637%; net Sharpe1.8407322968760098 is within.100 of H1.875022136892088; six scaled mechanics and separate cap checks hold. Fresh DERIV04d4d43a agreed before the native ledger write. [Cell record](cells/p9x-leverage-record.md). |
| P9-V | REJECTED | 1 | Paired net dSR -0.00040932705902685385 fails the strict-positive rule; all six scaled mechanics and cap checks pass. Fresh DERIV161781af independently agrees, with its anchoring disclosure retained. [Cell record](cells/p9v-vol-target-cov-record.md). |

The accepted unlevered chain is Y-F0 → L → H. P9-F0 = P9-F0h = H: plain recipe `theme-cov-hedge-v1-attempt2-gm.json` atbd837403, SHA1d8eecaa077844d093b05a861db18787ead3223317154c25094cfc98f029e88f, NAV `build-equity/mega-nav-p9-theme-cov-hedge-gm`. P9-F is accepted X under PRE-13 after V's rejection; V passes the separate H comparison but fails its required acceptance test against X. X is the new hand trial on H required by PRE30-MVX; the old-Y1 exemption branch is inapplicable. Its plain spec is `p9x-leverage.json` at ac60bd39, SHA e8a308b593b67c16111f3e4792c92983e9c5cfd412c7e42d485ecfe089b549db; NAV `build-equity/mega-nav-p9x-leverage`.

Final cell accounting:140 ledger lines, SHA086ca77e40806bb8f71039bd5e4b824325dcb2124b845b81ffd5784e364e8f5d; first139 raw lines preserve5caaccd36762571fffd9ff6fe1fdddfffca69790d2777755df3424fd0643a2dd. N_c69, N_hand130, inherited M110, N_tot240; historical admissions61=12+25+9+15, P9 admissions0, historical K_a40/P9+0, P9 mining0 and history reads0. V's one native construction trial is c39e04e6f3a7b885, written after PM's DERIV agrees release. Metadata stops and same-state continuation added no extra trial. The canonical counter excludes eight historical window reruns. Window research-window-v2 (2020-2023); hidden2024+ unread in this sprint; historical validation reads2(2023-2024);2025+ never read. PM independently verified all100 final V outputs and all41 source-file hashes. Adoption added no trial and preserved the ledger and its prefixes; the fresh freeze audit independently confirms that accounting.

The original133-line ledger prefix remains SHA27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554. Each intermediate prefix134 through139 also matches its completed cell checkpoint. V's final record commit is11489ee3c0e44b139c220f759aad58337ee3cee1. Documentation-only accounting clarifications atc3e3735f expand the historical admission split in M/C/MV/X without replacing their original checkpoint counts or changing native evidence.

R15-X-DERIV accepts the independent derivation with disclosed incidental prereg headings, C12/C15 heading text, the G-B3 descriptive row and JSON key names seen while navigating. No unlisted result values or root's X verdict were read. The independent deciding inequalities agree. X's negative paired dSR is printed only; X contributes no G-B7 step. The generic cycle verdict and unregistered monitor/summ/PBO figures stayed unread and did not replace the hand decision.

R15-H-READSCOPE preserves an actual read-scope deviation: H's reader exposed existing same-pair confidence intervals by selecting cbb_ci95 and the whole lw object after mechanics and rho had passed. They were not named in the narrow read plan and did not change the fixed point-dSR/mechanics decision. No new computation or variant was run; the seven prereg void conditions still held. The original evidence is preserved and subsequent readers must select only required scalar fields.

R15-M-PREPSCOPE records a second inspection limitation: M preparation printed the accepted H weights' whole theme_standardise object, exposing inherited ERC covariance, dispersion/sweeps and shares beyond the prerequisite metadata scope. No M output or computation existed. The precommitted parent, constants, eligibility conditions and deciding rule stayed fixed; subsequent selectors were limited to explicit leaves. This did not create a M trial or a M before-mechanics result read.

R15-M-SLEEVE-SOURCE adjudicates the completed M sleeve reader's direct receipt separately from numbered wave consoles. Its raw console and receipt are identical (SHA c71c0c0ba03f82af131b81ee69b291dcb0c73658a4cc1344e861ee7049aa2bcf); the exact dated fields are the run timestamp and five known owner PNG filenames. The classifier was unchanged. The engine logs contain only the two built-in bootstrap seed tokens, with no unexplained sealed hit. An initial import of the sprint classifier also invoked its default historical Y-S metadata scan; only source paths/tokens were exposed. That extra inspection is disclosed, with no scientific choice, statistic or trial added.

R15-V-PREPSCOPE and R15-V-PREPSCOPE-2 record preparation inspection of an existing D-COV mean_r2, eigen-family diagnostics and v11 si_shares coverage/quantiles beyond the intended metadata projection. No V output existed. The fixed covariance-store replacement, X pair, H-scaled limits, selection rule and diagnostic recipes were unchanged; historical values supply no new selection criterion. Later reads require exact selectors, with schema uncertainty resolved from source or expected key/type checks. These limitations are retained without claiming pristine preparation blindness.

R15-V-DERIV accepts the fresh independent derivation with its complete anchoring disclosure: permitted sources revealed X's accepted status, historical prereg findings and diagnostics, preparation disclosures and preprinted limits. Root's own verdict/derivation and the ledger were not read. Absent native annual-return name probes supplied no result; the explicitly authorized book-reader leaves support the H selection comparison. The independent deciding inequalities agree. This is scoped independent verification, without a claim of complete blindness.

## Adoption and alpha evidence

[adoption-print.md](adoption-print.md) contains every registered E row, the three paired bundles, year and side-by-side tables, conditional steps, historical PBO readings and full accounting/identity limitations. PM read it under R15-ADOPT-READ-OD3 and verified its original63-file listing, five receipts, all25 source hashes, committed rho barriers and exact native paired/DSR/conditional projections. Later punctuation and accounting-producer wording corrections are separately certified; the original listing remains the historical2f34da25 snapshot.

| Book condition | Outcome |
|---|---|
| G-B1 | MET: H net Sharpe1.875022136892088, above floor1.0 and target1.85; mechanics pass. |
| G-B2 | MET: X modeled net annual return8.986194230342487%, above8.0%, **plan placeholder, not owner-stated**. |
| G-B4 | MET: H and X meet4x Sharpe floor1.50 and parent-minus.10. H1.759566530994928 meets target1.70; X1.678945115577997 does not. |
| G-B6 | MET: H versus R-2 dSR.6190779300146116, Memmel SE.33914502635893456, two-sided p.0834 / one-sided p.0372. The one-sided.05 rule holds; target.01 is unmet. |
| G-B9 | MET: six mechanics checks and realised gross caps for H/X; X's separate L_t cap holds. |
| G-B3 / G-B5 / G-B7 | Required nondeciding prints complete, including unfavorable outcomes. |
| G-B8 | NOT EVALUATED: [od3-read.md](od3-read.md) records the authorized not-read default; history0. |

The full identity flag on G-B1/G-B2/G-B8 is: universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured.

H is accepted under the registered positive-point-gain/mechanics rule, but its increment over YF0 is only dSR.02554011056222616, SE.2615496420444564, one-sided p.4744. The cumulative comparison to R-2 includes historical improvements and does not isolate a convincing new H effect. Final-count DSR_tot is.7102851183610807 for H and.6864145390923599 for X (N240); DSR_hand is.7826813340672519/.761921341074039 (N130). These miss the print-only.80 target, while DSR_v8.8490225518942396/.8322657780436604 (N69) exceeds it. The six-step conditional raw sum is.6719401105622261 and adjusted sum is-1.6477111820020742. These prints do not retrospectively change selection, but they do limit the alpha claim. **New out-of-sample alpha is not established.** X raises modeled return through leverage and is not an additional alpha discovery.

All five adoption readers completed once, sequentially, each in less than one second of bounded-run wall time, with sampled process-tree peaks below57MiB. No RAM wait, NAV/fit/IC rerun, new PBO run, history access or trial append occurred. The first source check stopped on three exact dated filename spans naming PM's uncommitted freeze brief in dirty-file metadata. R15-ADOPT-PM-METADATA preserves that original OTHER certificate and adjudicates only the three byte-bound spans. The brief was committed; the completed output was reused without rerunning the reader. The other four source audits have OTHER0. This is a disclosed metadata stop, not a permission refusal or a data-date exception.

## Platform and provenance limits already established

| Item | Recorded evidence and scope |
|---|---|
| G-P1 | **UNAWARDED.** H's metadata-only registration retry makes zero-spec-edits-on-retry **UNMET**; its67-member rule wave is not the73-member add-alpha example. Full receipt phase sum187.174s, including reused u51.578s; fresh phases/readers/bundle135.596s, fresh core132.610s. The measured phase target300/hard600 holds. Attempt2 invocation sum155.5112064s differs from checkpoint-inclusive elapsed680.5601286s; neither is presented as uninterrupted execution. Original guard/seal stops remain. Scoreboard total152.4 uses the documented stage_seconds override rather than a phase-column sum; carry label clarification to P10. |
| G-P2, G-P3, G-P4, G-P6, G-P7, G-P8, G-P9, G-P11 | Gate report FINAL and its G-P table record passes, with integration rows 902-909 and the PROCEED review. These are the named completed checks, not new post-cell runs. G-P11 gates the freeze. Cell-specific continuity is checked in the fresh freeze evidence; the native-statistics scope retains the year-table qualification below. |
| G-P5 | **PARTIAL: four pairs unlisted, retirement deferred to P10.** The mirror guard is green and SIGN's deletion landed; neither closes the remaining four-pair gap. |
| G-P10 | Print-only; gates nothing. Components (a), (b), (c), (e) landed; (d) **PARTIAL**, because stage-record side files are not hooked. The print below retains all five components and the original gate catalog's scope; it is not a fresh catalog of the post-cell tree. |
| Reference prints | Only the Y-F0 reference carries both `--ann-mean-gross` and `--info-clock-print`. R-2/X-10/Y-1 reference receipts carry the former only. FLAGS-ROUTE-2 permits the disclosed one-sided clock print against Y-1. |
| Producer freeze | Re-pin `2078f877`, producer source `5f48a25d`, Release p9-3f and Debug p9-3g remain the record. No build occurred. Fresh freeze preparation verifies all54 executable hashes, CRT, both covariance manifests/models, v15/role and126 reference files; final state records its snapshot and subsequent document continuity. |
| Python driver provenance | `5352c820` and `5bbe1231` add optional literal Git trailer/exclusion handling; the fresh review `a8f06490` APPROVEs it. One named real-Git test passed. No research arithmetic or frozen executable changed. Sprint checker classes C/D cover only the evidenced Git exclusion token and four exact owner-PNG paths in dirty-file metadata. All 37 focused controls passed; the actual stopped-run scan classified A4/B2/C6/D16 across 29 logs with zero OTHER. R15-H-RESUME-GO follows independent verification of all 32 wave files and 197 output/run files; completed data stages must be reused. |
| Resource policy | R15-RAM-NOW implements the owner's instruction: omit optional admission-wait and host-budget options, launch immediately at the registered stage limits, retain the 512 MiB free-memory floor and C: >=15 GB, and preserve/report an actual resource stop. P9-L's approved w limit is3584 MiB. M completed one native invocation in162.25s without retry; its w peak was1758 MiB and inherited u was reused. C completed in82.640s with u/fit/w/card reused; its NAV peak was693.465 MiB. Historical reused receipts retain their original wait configuration; C launched no admission wait. No foreign process or lease was changed. |

The gate's named Release golden skip remains a skip. TEST-2 prohibits wasteful full-suite repetition; the gate report states that its listed whole-suite checks had already completed before that instruction arrived. No missing test is invented from that chronology.

R15-ADOPT-YEAR makes the remaining statistical producer boundary explicit: aggregate moments, paired tests, DSR and conditional calculations use frozen native eval, while the preregistered nondeciding year tables and bundle per-year Sharpe differences still use the existing `nav_summ.py` Python code. Those tables are not a new implementation or a universal native-migration claim. R15-ADOPT-PBO retains the two historical labeled PBO readings and prohibits a new PBO run; they do not measure final P9 overfitting risk.

MV also reused all four completed input phases and finished one invocation in46.016s; its NAV used1182.453MiB within the1536MiB cap. No new wait, retry, test or build occurred. Fresh receipt phases38.438s and reused historical115.047s are distinct from the46.016s outer invocation.

X reused the same input phases. Its NAV completed in18.844s at1142.445MiB; mechanics/pair/book readers took.265/.515/.532s. After independent agreement, monitor/summ completed in1.312/8.609s at132.285/70.383MiB. All used180s/1536MiB/floor512MiB without admission wait or host-budget options; no NAV rerun. Exact direct-hand source predicates classify only the registered receipt UTC and owner-filename metadata, with all actual source hits retained. Observed primary gross maximum1.8619722830838408 and all-scenario maximum1.8671310431406234 are below2.0, separately from the L_t parameter cap.

V's NAV completed in21.969s at1142.391MiB with the same immediate bounded policy. Its B.8(a) diagnostic is **not produced**: H's holdings rerun matched all27 other output files byte for byte, and the registered CSV serializer completed, but the first risk wrapper rejected the95,310,443-byte CSV in `--bind` with `bind small config/source manifests, not full data payloads`. The0.141s exit2 occurred before a risk receipt or native output existed; the covv1 diagnostic was not launched. This is an internal runner validation stop, not a permission-system refusal or RAM wait. The exact attempted command/raw log is preserved, with no retry by removing the binding. Neither recipe's book-bias outcome is inferred. PM independently verified the27-file identity and CSV SHA ccda2610e3e89ac732a50cd1d59e9e40863310bca89963098e3ea9f30c4656f8.

After independent V agreement, monitor and summ completed in2.453s and15.188s, at133.848MiB and71.137MiB. V's eight completed bounded phases total69.607s; nine outer invocations including the stopped diagnostic wrapper total76.720s. Historical reused115.047s is separate. No NAV rerun, admission wait, build or trial beyond the one registered V construction row occurred.

G-P10's required print is the **gate snapshot**, from `root-gate-report.md` step 7 and `gate-logs/realcat-gate*.txt`:

| Component | Evidence |
|---|---|
| (a) Pin states | Artifacts verified 4,190, declared 84; pins ok 1,620, declared 169, stale 220, missing 630, unresolved 1,512. These categories are retained; the print is not an all-clear claim. |
| (b) Render identity | 435 records checked across eight classes, zero mismatches. |
| (c) Independent catalogs | Two fresh real-tree catalogs produced `adbcd348b5d82a1fd4f357274a34bbf387391cf82154a961e701bad39a0dd474`; fixture catalogs reproduced `a50fdcf9c9c83bdac0d41ff368666da31578ebb4407bbc4498175a63014f100c`. Catalog databases were outside the root. |
| (d) Writer hooks | **PARTIAL:** three writers hooked, stage-record side files not hooked. SQL3-S2 S-2 remains P10. X-5 opt-in identities are the existing M6b items 5/6, cited under FIELDS-X5, not repeated. |
| (e) Schema guard | Landed for `atx.run-store/v1`; `classes.json` SHA `5bc27ba489f24a0d1c8c42c8c29927c8d1bde6bf63cd5993f285a5b084086557`. The catalog did not itself record that schema hash, an identified P10 limit. |

## Identity bridge and handover

The required flag is: universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured.

Absolute Sharpe and return may be overstated by survivorship. Paired comparisons provide relative evidence under the shared exposure; the ALD-DIAG counts do not measure the bridge's return effect. The AL-DATA handover is [al-data/atx-db-request.md](al-data/atx-db-request.md), committed at `3fd0bf98`. It describes a future rebuild-versus-rebuild comparison and its residual limits. This sprint does not execute that request or claim its result. A later nonzero diff annotates the verdict and does not authorize a rerun.

## Owner items and P10

| Item | Required disposition |
|---|---|
| OD-3 / atx-db | No explicit OD3-DB authorization was received. `od3-read.md` records `not read in P9: era inputs under atx-db (A4-DB, OD3-DB)` and G-B8 **not evaluated**. No history pass, era result or clean-source outcome is inferred. |
| DATA-3 | Deliver [data-classification.md](data-classification.md) together with DATA-2 at `progress.md:657` and DATA-3 at :682. The ruled list is class D **minus `asof_smoke_ctx_2013_t1000_20260922`**, which moved to U because its source scratch CSV is gone. A/B/E/U, vendor/source/history/pinned entries, `finra_short_volume` and the trial-ledger-pinned cp16/cp19 entries remain owner items. Historical deletion refusal remains disclosed; no deletion was performed for this handover. |
| Numerics | Deterministic transcendental implementation and its one-time re-pin are P10 (OD5-2/M2I-HK). The recorded Debug/Release runtime difference and named Release skip are retained. |
| Model and tooling gaps | Carry the final index's actual P10 list, including the four G-P5 pairs, SQL3 side-file hooks, NAV-2STORE, registered model limitations, A4 flips/fix tests, TMP-length fragility, FIELDS-GONE cuts and FLAGS-ROUTE evidence cleanup. SQL4 and SIGN landed; do not list them as still-unmerged conditional deferrals. |
| Owner decisions | Data purchases, production leverage and the WQ/new-signal theme remain owner decisions. P9-S and P9-W are lapsed at zero; the missing W prediction is not invented. |
| Leases and pools | LEASE-DEAD replaces a blanket release requirement. No lease action, worktree pruning or removal occurred. Pools8/24 remain frozen; pool23's mutated teeth binaries were neither consumed nor deleted. Fresh checks observed P9 owners dead except32 alive; foreign35 and later foreign3 changed from alive to free outside our actions. Final read-only status is retained in freeze-final-state.md. |
| Owner plot | `docs/plans/2026-10-03-p9-b0-yf0-equity-curve.png` is prepared and recorded for the owner by PLOT-1, `status-4.md:22-23`, `progress.md:337-338` and `handoff-root-m2a.md:61-62`. No explicit delivery acknowledgement was found in the searched records. This session does not open or stage any of the five owner PNGs. |

## Historical ref and permission disclosures

The earlier main/origin-main merge/push finding, six main-side factory reds and broken development preset remain historical owner items (MAIN-MERGE, MAIN-V, MAIN-V2, OD5-3). Current-session checks retain main e696e208ef622aeb8a9ffbcc4bd3e4ea16a74548 and origin/main8786863866bcecfbfe3d6f7a90a132bc85a6915c unchanged; this does not erase that history.

The final audited index uses `PERM-R<n>` refusal labels, distinct from the ledger's RESUME tags. Exact recorded commands in `m6b-logs/perm-refusals.txt`, `gate-logs/perm-refusals.txt` and the original index are retained as historical evidence, never re-executed. The owner subsequently handled the M6b index deletion and gate cache unstage; their obstacles closed, their disclosures remain.

The GATE-REV scratch deletion named `mask.py`, `rehash.py`, `decscan.py` and `paths.py` under `C:/Users/natha/AppData/Local/Temp/claude/c--atx/cbb67823-1fc6-4abf-a38b-a72955c9e69f/scratchpad/gaterev/`. **Exact refused command not preserved in searched sprint records.** The ledger cites a hand-back that did not survive in those records; no exact argv is reconstructed. Other command-text gaps remain explicit in the final index. No current-session permission-system refusal was reported; internal driver/source guards are recorded separately.

## Final verification and ruling appendix

The PM completed prereg G from the actual records: book/accounting/G-P10 print/frozen producer/flag-handover/owner-list items are evidenced; history and full platform qualification remain unfulfilled, while lease housekeeping remains an owner item under LEASE-DEAD. Fresh freeze preparation at HEAD71226ef9 and2026-10-17T18:21:18Z independently verifies the pinned metadata, canonical counts, original prefix and completed cell/DERIV scopes. Its JSON SHA is8dae8948e46472b2e872332778e87883e093b2294633e16b79daac1bc2c6e372. It did not yet cover later adoption/OD3/index documents; [freeze-final-state.md](freeze-final-state.md) records their final verification and the actual commit/status boundary.

[rulings-index-final.md](rulings-index-final.md) supplies the complete corrected ruling list and owner/refusal/P10 tables. Its declared line count and SHA define the exact ledger snapshot; any later factual audit handback is identified separately and introduces no hidden ruling. The final frozen state and exact-path commit must agree before this report is delivered as completed closeout. The broader two-track direction persists after P9, with the next bounded platform work specified in the platform report and new research requiring its own prospective registration.
