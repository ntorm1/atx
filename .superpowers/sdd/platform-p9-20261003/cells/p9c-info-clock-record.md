# P9-C native cell record

One native invocation completed exit0 in82.640s. Native pm7-34 verdict **REJECTED**;
explicit PM disposition pending. No retry, admission wait or next-cell work. Actual model
GPT-6 Astra; prescribed literal owner trailer retained. Registration0ca14564 preceded execution;
prelaunch c2b73ade committed the full delta/pins in its body, scalar plan/filter, adjacent PRE-40
notes and PM scratch22/23. R15-C-REG-BODY corrects the missing full evidence in0ca14564's original
body before measurement; that commit was preserved, not amended. No scientific change.

## Ordered source, pins, mechanics and rho

Source audit40 logs: A7/B4/C6/D28, OTHER0 (logs/p9c-seal-complete.txt). Unchanged CLI checker
used explicit C manifest; no imported module/default Y-S scan and no M sleeve exception applied.
All54 frozen exes, CRT and six P9-B0 reference listings match. Ten input/hash checks include
accepted H, v15 and D-COV reference manifests; cache686, research-store/index absent.
Current inherited H fit/IC definitions are unchanged; native run stage1.250s reused completed
u/fit/w/card. Native actual NAV argv has aim-partial-v5, info-clock-rate-v1, trade-fraction.05,
cadence1 and both print flags each once, no two-speed or duplicate rule. Seven own bounded
receipts and four reused receipts complete. Evidence: logs/p9c-input-commit-audit.json,
p9c-final-receipt-flags.json, p9c-frozen-exes.json and p9c-freeze-complete.txt.

Generated commits each contain one spec and the prescribed trailer:
83c43705 info-clock-rate-v1.json;5bfe8258 -cal.json;88584804 -gm.json.
Cell of record scripts/specs/p9/templates/info-clock-rate-v1-gm.json, SHA
`723b981a5ccc2f55427b18708359dc1ae0b68459a261da3c85d0b25aaa4fd4c7`, L1.1239,
NAV build-equity/mega-nav-p9-info-clock-gm. pm6-6-engine native calibration binds the published
plain recipe to that NAV; no hand orchestration or alternate result.

Mechanics saved before paired fields in logs/p9c-mechanics-first.json; all six PASS:

| check | value | limit |
|---|---:|---|
| gross all rows | 0.9862042240985196 | [.90,1.05] |
| absolute net all rows | 0.006548684683784417 | <=.02 |
| daily turnover mean | 0.03305737324750237 | <=.20 |
| daily turnover p95 | 0.04945272707779896 | <=.30 |
| return-identity error | 3.621235256101585e-16 | <=1e-9 |
| cash-book error | 6.826282596561157e-14 | <=1e-9 |

Next, existing bundle rho1005 sessions/0.9862992771637219 was recorded in p9c-rho-first.txt
before deciding fields. Both optional one_minus_rho/degenerate keys absent: numeric stable
difference not supplied, FALSE flag inferred under R15-V2-RHO. Bundle SHA
`ac489cf4797384b40d8d0da9c3e45c1a0eceed16bac4c4f1eaff072095d0646b` binds to08-judge,
exact C/H NAV pair and frozen eval SHAe2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c.
No statistic engine rerun, confidence interval or whole nested statistics block read.

## Registered deciding and nondeciding scalars

Only dSR_positive FALSE and mechanics TRUE decide the native rejection. Result SHA
`1750bbf8725fbb87bd7ce672d28969b1903eb71b3779998ac6230a84f344e34e`.

| scalar | C | H parent |
|---|---:|---:|
| net Sharpe | 1.7377160086121153 | 1.8750221368920863 |
| cost bps per traded dollar, native book | 13.359305074226517 | 12.490795824692832 |
| capacity4x net Sharpe | 1.6634910903860418 | 1.759566530994928 |

Paired net dSR-0.13730612827997102; Memmel SE0.08347705087942155;
two-sided p0.0994, one-sided p0.945, nondeciding. Printed cost-lower and capacity4x-higher
criteria both UNMET. Expected +.02 to +.06 net Sharpe [est] MISSED. The lower4x print is
consistent with the registered capacity-down direction, but does not decide acceptance.
Capacity CSV cost values13.359305074226501/12.490795824692793 differ from native book values
only in last rounding digits; both registered source paths/hashes are preserved.

Clock and exposure prints for all five scenarios are in logs/p9c-clock-prints.json. In every
scenario for each book:1,853,636 name-days,596,814 fast name-days,99,464 missing name-days,
fast_share0.32196936183803077. Clock field ea_days_since SHA
fb6742f084c04b14302d0d26a91c740e2e08be1e6704856d815a62053a6d709b; window21,
theta_fast0.12944943670387588/theta_slow.05. H rate=fixed; C rate=info-clock-rate-v1.
Counts are the same clock census, not a new result criterion. Missing clocks take the slow rate.

Primary S2 planned turnover H25.164589192854553/C31.36438786527308; fast planned turnover
H8.52439774600637/C15.970751549414706; fast turnover share
H0.3387457542293939/C0.5092001673368431, so the printed turnover-share-up direction is met.
TRAIN mean gross is matched by construction: the +~5% gross estimate is not established by
this matched TRAIN comparison; no uncalibrated statistic was added to adjudicate it.

| scenario | H realised max | C realised max | H realised mean | C realised mean |
|---|---:|---:|---:|---:|
| linear / swap | 1.1163097160400177 | 1.0954554315279275 | 0.9841185509126439 | 0.9850839548849983 |
| primary modeled / swap | 1.1182847720520317 | 1.0982968560980533 | 0.9861861805709025 | 0.9861380929669935 |
| terminal adverse / swap | 1.1193437897089227 | 1.0983829831573262 | 0.9868494270108918 | 0.9864007008290815 |
| modeled / flat300 | 1.1210012149525754 | 1.1132066818547721 | 0.9872345147401129 | 0.9869176067799572 |
| modeled / engine tiers | 1.1184320680021176 | 1.0984548004445642 | 0.9863594141960205 | 0.9862816704315294 |

These are the C3-4/ALCLOCK-4/OD5-7 realised figures, not the loose planned bound. All per-scenario
planned-turnover provenance and definitions remain in the scalar artifact. FLAGGED2019 stays;
clean-source OD-3 excludes this rule. The x1=S2 sentence was tested only at fixed rate; rule-on
P10 remains untested and that sentence is no acceptance input. No M sleeve or H-only criterion.

## Accounting, timing and identity

Exactly one native construction trial52988f05af490865, count1, origin prior. Ledger136->137,
first136 prefix unchanged; full SHA
`21522bb42c9adf7859dbf042878338b3b0374b2f920055a8f2e59980c2a9a76b`.
Canonical trial_counts: N_c66, historical admission61, P9 additions0, N_hand127, M110, N_tot237;
eight historical window reruns add0. Appendix A in p9c-accounting.txt. Research-window-v2
TRAIN2020-2023, hidden2024+ unread in this sprint, validation reads before v8:2 (2023-2024),
history reads0,2025+ never read. Identity-bridge flag: universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured.

Immediate launch with free3503MiB RAM/C37.1GB, no wait/host-budget option. Native NAV48.453s,
sampled_peak_tree_rss_bytes727150592 =693.46484375MiB, within180s/1536MiB/floor512MiB.
Seven own receipts have no admission configuration/wait. Reused historical u receipt retains
its old1800s admission limit and .016s historical wait; it was not launched or waited on in C.
All own receipts completed within their bounds. Timing-only scoreboard82.0s is the documented
stage_seconds override (sum81.969s before record.344s), outer invocation82.640s. Distinct receipt
phase sum184.500s includes115.047s reused u/fit/w/card; fresh phases/readers/bundle69.453s.
Git timing1.827s is not added again. Both native outer wall and receipt sum are below300s target
and600s hard ceiling. G-P1 remains UNAWARDED: prior limitation and67-member rule route versus
73-member add-alpha example remain. Evidence p9c-timings-only.json/p9c-timing-summary.json.

## Void checklist, audit limitations and disposition boundary

B.0(1) ruled registration, lane gate, re-pin and exact body supplement precede execution.
(2) Authorized H/rule/flags/fields/fit mode and frozen eval/IC/NAV inputs hold.
(3) Source/pin/mechanics, then logged rho, then explicit deciding/registered scalars.
(4) No2024+ session/data read; every actual metadata hit has permitted source provenance.
(5)54 exes unchanged. (6)CRT unchanged. (7)No cache-index flag/index.sqlite, cache686/store absent.
No void found. Native rejection counted once; STOP for explicit PM disposition before MV.

Audit corrections: first completion receipt enumeration omitted NAV/monitor directories;
expanded via native phase run_dir before mechanics/rho to include actual argv and all receipts.
First resource extraction used nonexistent peak_mib; corrected to sampled_peak_tree_rss_bytes
and reported above. Historical admission settings are selected from limits/admission, not a
nonexistent top-level field. These are metadata-audit corrections, not native stops or retries.
Prior M H-fit and Y-S metadata-scan disclosures remain preserved, with no new C exposure or
scientific-choice change. No checker edit/test, build/suite, permission refusal or next-cell work.

95 own output/receipt files are hashed in logs/p9c-output-files-sha256.txt, listing SHA
`9ec8b9052add2944d3da0e25ae6967bc807391f173df659eecebe6113c667c2a`.
Native record copies in cells/waves/p9c-info-clock match original bytes. Raw native console
SHA6f188c11fe3b5746e44bee30429c97fa10fdad1a88c10aa85fd1bc11f6ef35c8 and scoreboard outputs
remain preserved unread except declared metadata, unstaged. final-report.md untouched; owner
PNGs never opened/hashed/staged/moved/deleted; no foreign-pool/ref/lease action.

## Historical accounting clarification (R15-APPENDIX-SPLIT)

This is a documentation-only clarification of the original checkpoint, not a recount:
N_c66, N_hand127, M110, N_tot237. Historical admissions61 =12(v8)+25(X hand)
+9(X4 re-screens)+15(Y hand); P9 admission additions0. K_a40 historical hand-written
strings/P9+0; M110 inherited/P9 mining0; eight historical window reruns add0.
The native accounting evidence is cells/logs/p9c-accounting.txt (relative to the sprint
directory); its historical/native wording is preserved. Final V totals do not replace
this checkpoint. The original research-window/history and full identity-bridge flags
above remain in force and unchanged; no producer rerun, ledger write or disposition change.
