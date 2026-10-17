# P9 adoption print

Executed under R15-ADOPT-EXEC and the committed c3e3735 plan; proposal SHA-256 c0f44d62d9c3877430c7dfca6f9948c51b350a64a85d4cc0f32bbcd1956ec4d0. F0=F0h=H; selected levered F=X. Scenario modeled-1bn-stale5-v1+swap-fin-v1; research-window-v2 (2020â€“2023). This is the registered E print. The unlevered adoption rule (H net Sharpe>=1.0 AND mechanics) is met. No owner deployment decision or history evidence is implied. OD3-DB remains unanswered; stop for PM **Adoption print read** before the separate OD3 record.

## Producer and evidence identities

Every table below uses these named aliases; full output/source identities are also bound in cells/logs/p9-adoption-final-output-sha256.txt and the unchanged proposal. Aggregate paired/DSR/conditional statistics use frozen native atx-research-eval.exe SHA-256 e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c. Existing NAV/summary/capacity metrics use frozen atx-nav.exe SHA-256 255b58acedf7df82a1caa672ff30a58c317a8f0dccf7dbe7d3b7393b91063565. Year rows are the explicit nondeciding R15-ADOPT-YEAR exception: atx-impl/tools/nav_summ.py year_table:585â€“605 and bundle years:633â€“656, Python source SHA-256 7b5c5e242e789cb78f2a3fc6c6be2c359344f76f476a2aa31f2cfc4edb29ab69; remaining centralization gap goes to P10.

| Alias | Path | SHA-256 |
|---|---|---|
| P | cells/logs/p9-adoption-registered-prints.json | 64d46c89933f8953690301ba75cb43fbe434416b5dd0223b1d24eb01ce55f005 |
| Xprint | cells/logs/p9x-registered-prints.json | bd6934b6e93bd5c195871bf51858658859cf58edd9f4c159015e9a8da02e5b55 |
| Hprint | cells/logs/p9h-registered-prints.json | d3c71d24c8b6b3598e04b9829846297a20bf1123b114be219487ce109c816633 |
| Mechanics | cells/logs/p9-adoption-mechanics-first.json | 3fbd07f236f40ef672e49d9666b060881039414323ccda3d6b9987548b23c803 |
| DSR/year | build-equity/p9-adoption-readers/dsr-years.json | ce62fccd4970ebe5f60e29953c4dce469824ed0d07f79510331ec5b2882e7196 |
| r2-h | build-equity/p9-adoption-readers/r2-h.json | c998292004f164394fc409b108629265cb89b16eaed6693cc4d72ae991301841 |
| yf0-h | build-equity/p9-adoption-readers/yf0-h.json | fc76ee63c08a4c2c1b99fc6376b9d9dfbb99d203867bf8d5fa607ca9ae8401bc |
| r2-yf0 | build-equity/p9-adoption-readers/r2-yf0.json | 77316228751a84190cb2fe33d3d0866ed9e0dd30a1f903d647509070adcd093e |
| Conditional | build-equity/p9-adoption-readers/conditional.json | f7620cc9f108a154caa82d4c81cf39df2ce888b685967986dd186c069652b569 |
| YF0 summary | build-equity/p9-b0g-yf0-nav/summary.json | 826d5ec65329c24c4c0c1b72ee1f57dcdbdb136cf00d4113c6463310e79b8f38 |
| H summary | build-equity/mega-nav-p9-theme-cov-hedge-gm/summary.json | 533d0ef1fffcae07dcdbe1d241373b201a7108eeec842e1d96a40c2988bdca25 |
| X summary | build-equity/mega-nav-p9x-leverage/summary.json | da726a7dd1db5d867bb291d0cbfe15e281fc5e364cc47efb9784a23bbf9b0108 |
| Prereg prior/limits | p9-prereg.md | e525ff79e18e9be5c775ab73f42fbe292f5766d1670ad846feee75efa3cc513b |
| Historical PBO publication | root-R0-12-report.md section5 | f26218b8d692477d269277dbfc66c98f434e1ad0d3d7504e7bf5f0ced0ecb0f9 |

## E rows

| Row | Registered print and status | Producer/evidence |
|---|---|---|
| G-B1 | MET: H net Sharpe 1.875022136892088 >=1.0; target1.85 met. H mechanics PASS. | Xprint frozen NAV + Mechanics |
| G-B2 | MET: X net annual 0.08986194230342487 >=.08; **plan placeholder, not owner-stated**. | Xprint frozen NAV |
| G-B3 | PRINT ONLY: all three per-unit gross returns exceed historical X-5 .0654; selected X absolute gross annual .11346345795103402 exceeds target .10. Distinct quantities below. | YF0/H/X summary + bound all-rows mechanics/book |
| G-B4 | MET H and X: x4 Sharpe >=1.50 and >=parent-.10. H 1.759566530994928 vs L 1.6994264697927697; X 1.678945115577997 vs H 1.759566530994928. Target1.70 met H, unmet X. | Xprint/Hprint frozen NAV capacity_curve.csv multiple4 |
| G-B5 | PRINT ONLY: H/X DSR_tot and DSR_hand below .80 target; DSR_v8 above .80. Never gates. | DSR/year native eval; exact N table below |
| G-B6 | MET: Hâˆ’R2 dSR .6190779300146116; one-sided p .0372 <.05; target .01 unmet. | r2-h native eval; full registered bundle below |
| G-B7 | PRINT ONLY: six-step raw sum .6719401105622261; conditional adjusted sum -1.6477111820020742. | Conditional native eval; explicit six steps below |
| G-B8 | NOT EVALUATED: OD3-DB unanswered; actual history0. No OD3 record yet. | Canonical unchanged ledger / Appendix A; no historical run |
| G-B9 | MET: both H/X six mechanics limits and realised gross cap<=2 PASS; X L_t cap PASS. | Mechanics certificate, exact values below |

G-B1 identity flag: universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured.

G-B2 identity flag: universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured.

G-B8 identity flag: universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured.

Absolute Sharpe/returns may be overstated by survivorship; paired relative evidence shares this limitation.

### Gross quantities (G-B3, nondeciding)

Gross returns are gross of costs AND financing. Denominator is registered all-rows gross, not exposure return-row mean. H denominator comes from mech-matched.json SHA 7a1537f5dd162912759f2559aaa94b3edec7952e3e380b5f7a6c4862ff81f50e; X/YF0 denominators come from build-equity/p9x-leverage-readers/book.json SHA a4ff52666727e9401a00bce89d6b0fdc219977c935c2d01e13425a2585718d38.

| Book | Absolute gross annual | All-rows gross denominator | Per-unit gross | Per-unit reference | Absolute levered target |
|---|---|---|---|---|---|
| YF0 | 0.07067817348886382 | 0.9862134133265011 | 0.07166620584733897 | 0.0654 | not applicable |
| H | 0.07061758625960741 | 0.986241893721804 | 0.0716027038692467 | 0.0654 | not applicable |
| X | 0.11346345795103402 | 1.5745869901490488 | 0.07205918673333742 | 0.0654 | .10 |

### Final-count DSR (G-B5, nondeciding)

| Book | Existing trial | DSR_tot N240 | DSR_hand N130 | DSR_v8 N69 |
|---|---|---|---|---|
| H | 94a77d4af05ac735 | 0.7102851183610807 | 0.7826813340672519 | 0.8490225518942396 |
| X | 4b3e8c853dac51c2 | 0.6864145390923599 | 0.761921341074039 | 0.8322657780436604 |

Both bindings are in canonical ledger140, research-window-v2. Native full-ledger tot_minus_m maps directly to deflated_total.tot_minus_m; native house_v1 maps to deflated_ledger N69. Parts are ledger130/campaign110/total240. No prefix-hand or legacy default-N output was used.

### Three registered paired bundles

All native eval; block21, seed20260929, draws4999, 1005 sessions. All three rho-only certificates committed in 71226ef9 before paired scalars. Optional degenerate/one_minus_rho leaves were absent; inferred FALSE under the documented nondegenerate encoding. Rho respectively .7729073584239163, .8653199607726447, .8995266242016284; none near the conservative abs1e-12 boundary about 1-rho=1e-6. Only Hâˆ’R2 supplies G-B6; other bundles are prints.

| Producer (finalâˆ’base) | dSR | Memmel SE | t | p two-sided | p one-sided |
|---|---|---|---|---|---|
| r2-h | 0.6190779300146116 | 0.33914502635893456 | 1.8254076630904494 | 0.0834 | 0.0372 |
| yf0-h | 0.02554011056222616 | 0.2615496420444564 | 0.09764918950982554 | 0.9322 | 0.4744 |
| r2-yf0 | 0.5935378194523855 | 0.2258385027719382 | 2.6281515869407195 | 0.0308 | 0.003 |

### Conditional steps (G-B7, nondeciding)

Native conditional threshold0, alpha.05, Memmel SE; Andrews, Kitagawa & McCloskey (2024) median-unbiased estimator. Only registered point estimates are copied; no confidence intervals or unrelated daily-cell results. Explicit step source for all six rows.

| Step | Raw dSR | SE | Median-unbiased |
|---|---|---|---|
| R-2 | 0.0529 | 0.0945 | -0.03292925016922485 |
| X-2 | 0.0109 | 0.0603 | -0.2109105724695975 |
| X-3 | 0.1536 | 0.12 | 0.13351295349796757 |
| X-5 | 0.349 | 0.2011 | 0.3371993846799523 |
| Y-S | 0.08 | 0.1502 | -0.0669526333436618 |
| H | 0.02554011056222616 | 0.2615496420444564 | -1.80763106419751 |

Cumulative cells6; raw sum 0.6719401105622261; adjusted sum -1.6477111820020742. L is protocol-only; M/C/MV rejected; X/V leverage and W/S lapsed are excluded. The adapter performs lossless existing serialization only; no Python replacement statistics. Serialized input build-equity/p9-adoption-readers/conditional-input.csv SHA-256 f9f609e60acecd6320ec1e41228d000d8aa8f62d34ab1422fa5e360b5049aee7. Exact internal argv (metadata also preserved at conditional-command.json):

```json
["build-equity-rel\\bin\\atx-research-eval.exe","--daily","H=build-equity/p9-adoption-readers/conditional-input.csv","--protocol","v8","--threshold","0","--conditional-alpha",".05","--step","R-2=.0529,.0945","--step","X-2=.0109,.0603","--step","X-3=.1536,.1200","--step","X-5=.3490,.2011","--step","Y-S=.0800,.1502","--step","H=.02554011056222616,.2615496420444564","--json","--out","build-equity/p9-adoption-readers/conditional.json"]
```

### Mechanics and cap (G-B9)

H original source build-equity\waves\p9h-cov-hedge-attempt2\readers\mech-matched.json, SHA 7a1537f5dd162912759f2559aaa94b3edec7952e3e380b5f7a6c4862ff81f50e. X original source .superpowers\sdd\platform-p9-20261003\cells\logs\p9x-mechanics-first.json, SHA 8b222bfc9dacbefc111d00c36aa274297894b2deebd4ab5cc84df66f253ed015. X scaled limits use r=2/1.1674; net limits are absolute. Both accounting limits are1e-9.

| Book | Check | Value | Limit | Status |
|---|---|---|---|---|
| H | mean_gross_leverage_all_rows | 0.986241893721804 | [0.9,1.05] | PASS |
| H | mean_net_leverage_all_rows | 0.0050699651270206605 | 0.02 | PASS |
| H | tau_gmv_mean | 0.02706227150226712 | 0.2 | PASS |
| H | tau_gmv_p95 | 0.03234806696272993 | 0.3 | PASS |
| H | max_return_identity_error | 3.7816971776294395e-16 | 1e-9 | PASS |
| H | max_cash_book_relative_error | 1.2564514679600198e-13 | 1e-9 | PASS |
| X | mean_gross_leverage_all_rows | 1.5745869901490488 | [0.8398191654289909,1.6946383308579818] | PASS |
| X | mean_net_leverage_all_rows | 0.009567296396714624 | 0.03426417680315231 | PASS |
| X | tau_gmv_mean | 0.027435286052218615 | 0.3426417680315231 | PASS |
| X | tau_gmv_p95 | 0.034127886700244255 | 0.5139626520472845 | PASS |
| X | max_return_identity_error | 3.5735303605122226e-16 | 1e-9 | PASS |
| X | max_cash_book_relative_error | 7.921937179839092e-14 | 1e-9 | PASS |

Realised exposure (return-row means; never the G-B3 denominator), cap<=2 in every scenario. H exposure source p9c-clock-prints.json SHA 9d0318798adc013144fac7ff8c59eb31c4f414a7aa63c3b1f86bde35a913b601; X exposure source as above. X L_t cap also PASS.

| Book | Scenario | Mean realised gross | Max realised gross |
|---|---|---|---|
| H | linear-6bps-stale5-v1+swap-fin-v1 | 0.9841185509126439 | 1.1163097160400177 |
| H | modeled-1bn-stale5-v1+swap-fin-v1 | 0.9861861805709025 | 1.1182847720520317 |
| H | modeled-1bn-terminal-adverse-v1+swap-fin-v1 | 0.9868494270108918 | 1.1193437897089227 |
| H | modeled-1bn-stale5-v1+flat-300-v0 | 0.9872345147401129 | 1.1210012149525754 |
| H | modeled-1bn-stale5-v1+engine-tiers-v1 | 0.9863594141960205 | 1.1184320680021176 |
| X | linear-6bps-stale5-v1+swap-fin-v1 | 1.5662931516521497 | 1.8584441980160666 |
| X | modeled-1bn-stale5-v1+swap-fin-v1 | 1.5743764230832007 | 1.8619722830838408 |
| X | modeled-1bn-terminal-adverse-v1+swap-fin-v1 | 1.5775726999779318 | 1.865985818141723 |
| X | modeled-1bn-stale5-v1+flat-300-v0 | 1.5721120212358906 | 1.8671310431406234 |
| X | modeled-1bn-stale5-v1+engine-tiers-v1 | 1.574850300557488 | 1.8625576053523885 |

## Side-by-side books

Xprint/registered book source SHA a4ff52666727e9401a00bce89d6b0fdc219977c935c2d01e13425a2585718d38; frozen NAV producer. YF0/H leverage column is a fixed parameter, X is observed mean L_t. Turnover per gross uses the registered all-rows denominator.

| Book | Net annual | Net SR | x4 SR | Annual vol | Max drawdown | Fixed L / mean L_t | Tau GMV | Tau/gross | All-rows gross |
|---|---|---|---|---|---|---|---|---|---|
| YF0 | 0.05653814276222473 | 1.849482026329859 | 1.6994264697927697 | 0.030569717335626075 | 0.02580037230369092 | 1.1828 | 0.028134396709054203 | 0.028527696266223746 | 0.9862134133265011 |
| H | 0.05689681785648637 | 1.875022136892088 | 1.759566530994928 | 0.030344611264587394 | 0.02203729312571523 | 1.1674 | 0.02706227150226712 | 0.027439791063976805 | 0.986241893721804 |
| X | 0.08986194230342487 | 1.8407322968760098 | 1.678945115577997 | 0.04881858293893884 | 0.032292584985299744 | 1.864660752885562 | 0.027435286052218615 | 0.017423798255580415 | 1.5745869901490488 |

## Registered year prints (nondeciding Python exception)

Producer nav_summ.py SHA above; R2 from r2-h/base, YF0 from yf0-h/base, H/X from DSR/year. Net return is compounded. No new gate or native-year claim.

| Book | Year | Rows | Net return | Net SR | Annual vol | Tau GMV | Cost bps/traded |
|---|---|---|---|---|---|---|---|
| R2 | 2020 | 252 | -0.01113788517234493 | -0.3162488271963279 | 0.0336290841651406 | 0.026405001994462263 | 14.79467071504307 |
| R2 | 2021 | 252 | 0.09825204953085787 | 2.595853172125467 | 0.03636403316631543 | 0.02268385911380185 | 11.448950452719263 |
| R2 | 2022 | 251 | 0.0796167039727449 | 1.8405057733374794 | 0.042278194457704574 | 0.023500487109506637 | 12.103016715429133 |
| R2 | 2023 | 250 | 0.01932809927388801 | 0.6376629274795329 | 0.03101474540565754 | 0.0231135634840747 | 11.518920710061698 |
| YF0 | 2020 | 252 | 0.04307640277172409 | 1.3347101338009841 | 0.03198295355578269 | 0.030355260138651398 | 14.979026758733873 |
| YF0 | 2021 | 252 | 0.09831443215668667 | 2.9842686430774843 | 0.0315960420423554 | 0.027269895609570317 | 11.680073709202198 |
| YF0 | 2022 | 251 | 0.0690858221705637 | 2.0992837798545634 | 0.0321992329819299 | 0.027757861730566703 | 12.362797826284273 |
| YF0 | 2023 | 250 | 0.02105613717510013 | 0.8198277262266268 | 0.026033204609407893 | 0.027141252019944995 | 11.719306066806611 |
| H | 2020 | 252 | 0.037102751322435124 | 1.3683452031135312 | 0.026889190152948425 | 0.029461562511703385 | 14.883743766328239 |
| H | 2021 | 252 | 0.10524691690587851 | 2.6575193025924806 | 0.03793196457869714 | 0.026215572044215835 | 11.502596803919387 |
| H | 2022 | 251 | 0.06711230926015088 | 2.3091716021165682 | 0.02841957345692729 | 0.026273280255451347 | 12.121123859885655 |
| H | 2023 | 250 | 0.023875343819169226 | 0.9067141692314439 | 0.02662098216772736 | 0.026286302554482954 | 11.567559538900744 |
| X | 2020 | 252 | 0.05152311786320585 | 1.217030490606684 | 0.042006963612101905 | 0.030231322564568185 | 15.876022197677177 |
| X | 2021 | 252 | 0.1779490128369936 | 2.659840420987824 | 0.06231994185716224 | 0.026524779207849375 | 13.08968154644363 |
| X | 2022 | 251 | 0.11051371111341757 | 2.3384602409105026 | 0.045453444492984954 | 0.026409384600662698 | 13.92273801111028 |
| X | 2023 | 250 | 0.03534231415526423 | 0.8464485615214431 | 0.04242293965097193 | 0.026561180783180434 | 13.129404661459585 |

Per-year paired prints retain the same Python source; named bundle JSON aliases bind each row.

| Bundle | Year | Sessions | Final SR | Base SR | dSR |
|---|---|---|---|---|---|
| r2-h | 2020 | 252 | 1.3683452031135312 | -0.3162488271963279 | 1.6845940303098592 |
| r2-h | 2021 | 252 | 2.6575193025924806 | 2.595853172125467 | 0.061666130467013414 |
| r2-h | 2022 | 251 | 2.3091716021165682 | 1.8405057733374794 | 0.4686658287790888 |
| r2-h | 2023 | 250 | 0.9067141692314439 | 0.6376629274795329 | 0.269051241751911 |
| yf0-h | 2020 | 252 | 1.3683452031135312 | 1.3347101338009841 | 0.03363506931254712 |
| yf0-h | 2021 | 252 | 2.6575193025924806 | 2.9842686430774843 | -0.3267493404850037 |
| yf0-h | 2022 | 251 | 2.3091716021165682 | 2.0992837798545634 | 0.20988782226200486 |
| yf0-h | 2023 | 250 | 0.9067141692314439 | 0.8198277262266268 | 0.08688644300481707 |
| r2-yf0 | 2020 | 252 | 1.3347101338009841 | -0.3162488271963279 | 1.650958960997312 |
| r2-yf0 | 2021 | 252 | 2.9842686430774843 | 2.595853172125467 | 0.3884154709520171 |
| r2-yf0 | 2022 | 251 | 2.0992837798545634 | 1.8405057733374794 | 0.25877800651708394 |
| r2-yf0 | 2023 | 250 | 0.8198277262266268 | 0.6376629274795329 | 0.1821647987470939 |

## Dispositions and required historical prints

PM dispositions: L ACCEPTED (protocol-only); H ACCEPTED; X ACCEPTED; M REJECTED; C REJECTED; MV REJECTED; V REJECTED; S LAPSED (0). **P9-W lapsed (0), D3-Q1a: no dSR**. No new cell verdict is inferred here.

Beside MV REJECTED, prereg B.5/PRE-24a prior: spo-v1 rejected for implementation defect (gross .65, mechanics fail), no research content. spo-v2 rejected as research: paired dSR -.794, Memmel SE .476; cost per traded dollar +27%; held names1,514 vs1,930. With a binding gross budget the risk term does not discipline the book; alpha IC sigma z tilts to high specific-vol, lower-ADV names. MV is distinct: closed-form gross-normalised P V^-1 P alpha divides by specific variance and tilts toward low specific-vol, with no optimiser, cost term or gross cap inside the target rule; trading remains aim-partial. No prior rerun.

Prereg A.8 power line: MDE80%=2.49*SE; combination SE~.2 detects~.5, sign-level evidence. This limits interpretation of cumulative G-B6 and history G-B8; history MDE~.40â€“.45. This is the registered planning statement, not post-hoc power.

Historical PBO only (R15-ADOPT-PBO): nav_summ CSCV whole-ledger grid70 candidates,16 blocks,12,870 splits,754 common sessions2020-01-06..2022-12-30: .1526029526029526, evidence build-equity/cycle-v8y-vol-target-y-1/pbo.json SHA2beb256ccd64325d28f0b7f95d3eae8e94b10b4ab57197f48c7681e7876f4ea6. YF0 own-cycle65 candidates: .07661227661227661, evidence build-equity/cycle-v8ysb-gm/pbo.json SHA556192c0161051011587467b86c848b916ae4ff8de3fbfbd79cd1d7b4f23aa10. Both copied from root-R0-12-report.md section5, hash-verified without parsing PBO objects. These are historical scopes, not estimates of final H/X overfitting risk; no new PBO run.

ALD-DIAG copied from prereg C.2: role train-2020-2023-lo3, manifest SHAe1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4;1,405 sessions2018-06-01..2023-12-29, seal check passed. No return statistic or new diagnostic run.

| Year | Role names | Delisting exits through2023-12-01 | Share | Causes mna/performance/non_common/unknown/unknown_unlinked | Unclassified after2023-12-01 |
|---|---|---|---|---|---|
|2020|2,420|83|.034|82 / 1 / 0 / 0 / 0|0|
|2021|2,540|103|.041|97 / 1 / 0 / 5 / 0|0|
|2022|2,325|106|.046|102 / 1 / 0 / 3 / 0|0|
|2023|2,186|95|.043|88 / 2 / 0 / 5 / 0|2 within30 days of seal|

3,256 distinct names;387 classified exits+2 unclassified, a lower bound. Mostly M&A; five performance exits, few failure exits. Prereg C.25 cost-model limitation: modelled net Sharpe optimism about .06â€“.10 is a plan estimate/OD5-5, not a measured adjustment to an offered book. Fee-aware model absent; no data purchase.

## Appendix A: canonical accounting and limitation

Native backtest_integrity appendix_a/appendix_a_v8 block, exact isolated stdout copy at cells/logs/p9-adoption-accounting.txt SHA-256 36a81dcb1209d4417c31a1298d4970d6e4985dc4494b8413f33e1be6c8572c32; source script SHA3def2b12eb87dd58b58fff2f3ecfcd760c1f3afbdb43f26c96097d0ecbbbadd9:

```text
Appendix A (trial ledger build-equity/trials.jsonl): 130 trials in 140 ledger lines
   admission       61  [TRAIN 2020-01-01..2023-12-31]
   composition      0
   construction    30  [TRAIN 2020-01-03..2023-12-29]
   construction    37  [TRAIN 2020-01-06..2022-12-30]
   construction     2  [TRAIN 2020-01-06..2023-12-29]
   universe         0
   data             0
   mining-campaign     0  (1 campaign line(s), registry count 110: the campaigns' own budget, not in N)
   adding no trial: 9 line(s) (0 by the defect rule, 8 window re-run(s), 1 protocol line(s))
TRAIN construction cells 69; admission trials this sprint 61; window research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history reads 0; 2025+ never read.
```

Clarification of preserved native wording: admissions61=12(v8)+25(X hand)+9(X4 re-screens)+15(Y hand), historical/P9+0. Ka40 historical/P9+0; M110 inherited/P9 mining0; eight historical window reruns add0. Nc69/Nhand130/Ntot240, history0. Canonical ledger140 SHA086ca77e40806bb8f71039bd5e4b824325dcb2124b845b81ffd5784e364e8f5d; original139 prefix SHA5caaccd36762571fffd9ff6fe1fdddfffca69790d2777755df3424fd0643a2dd; original133 prefix SHA27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554. All unchanged before/after. Four record appendices preserve original M65/126/236,C66/127/237,MV67/128/238,X68/129/239 checkpoint counts.

universe `linked-operating-v3` and the fields reading `identity-bridge-v2-pit` depend on 2026-dated inputs through the identity bridge (paths listed in `al-data/atx-db-request.md` A0); size unmeasured.

## Execution, actual stops, and read boundary

Five exact commands completed once, sequentially, no RAM waiting or retries. Each launch met immediate RAM>=512MiB and C:>=15GB. Receipt metadata audit cells/logs/p9-adoption-receipt-audit.json SHAf89135654c0e8b948e211dadf1bfcfb72fcb740e05a5793bc56d1cac4db6dd76 binds complete commands, source head, output and argv hashes. Each completed exit0 within exact caps; no admission block. Resource receipts:

| Stage | Wall seconds | Sampled tree peak bytes | Seconds / RSS MiB cap |
|---|---|---|---|
|dsr-years|0.7970000000204891|52047872|600 / 2048|
|r2-h|0.5310000000754371|41943040|180 / 1536|
|yf0-h|0.7809999999590218|46665728|180 / 1536|
|r2-yf0|0.7970000000204891|58912768|180 / 1536|
|conditional|0.5320000000065193|31731712|180 / 1536|

Preparation path-selection assertion preserved in cells/logs/p9-adoption-preparation-stops.txt; R15-ADOPT-PATH corrected only the wrong preparer m2d path to registered m2i-dcov-covv1 before computation. No pin changed.

Actual first dsr-years source guard stopped before statistics on three OTHER occurrences in exact parsed PM draft dirt metadata. Original certificate SHA8289d6ee5744fbed4f06de821953aa4fe549f2a231aab8b67029c9f996f45369 remains unchanged. R15-ADOPT-PM-METADATA and the separate exact exception certificate adjudicated only those three PM_DOC_METADATA spans; original failed command/stop remains in cells/p9-adoption-source-stop.md. Commit f065ee97 preserved the stop, exception, source/void/mechanics barriers and exact PM draft. Reused the completed output, no rerun. Remaining four source audits have OTHER0. All25 exact source files bound; no broad folder/date exception or helper/proposal edit. All three rho-only barriers committed71226ef9 before paired scalars.

Final audit rehashed69 proposal source/input/NAV/historical-PBO bindings and ledger/prefixes successfully at head71226ef9; prereg input SHA as above. Raw results, stdout/stderr/start/receipt/raw consoles remain preserved under the exact proposal paths; output list binds them without expanding read permission. Historical H interval-overread disclosure remains in its original record; no interval reread/copy here. No permission refusal, build, suite, fit, NAV/IC generation, trial/history append, PBO run, cache initialization, atx-db or2024+ data access, ref/lease/foreign-pool action, or owner PNG byte access.

Platform capabilities are separate from alpha/deployment proof: G-P1 UNAWARDED/G-P5 PARTIAL and platform limitations remain in the platform report. Final-report/freeze-verdict are outside this commit. Stop now for PM Adoption print read; no od3-read.md is created.
