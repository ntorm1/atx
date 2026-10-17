# P9-X independent derivation (CELLS-2)

Verdict: **ACCEPTED** under the registered B.7 conjunction. Primary S2 net annual X 0.08986194230342487 > H 0.05689681785648637; net Sharpe X 1.8407322968760098 >= H 1.875022136892088 - .100 = 1.775022136892088; all six scaled mechanics checks pass. No observed exposure-cap conflict, rounding-boundary concern, actual permission refusal, or missing deciding evidence. This independent report does not authorize a hand trial write or monitor/summ completion; PM adjudication/release remains separate.

## Scope and registration

Worktree C:/atx-wt/pool-2; branch verified as feat/platform-p9-20261003. The released dispatch was read whole. Read order: prereg A.6, B.0, B.7, C.11, C.14 and E G-B9; plan and proposal; mechanics and native exposure; rho-first evidence and pair hash; paired scalar projection; H/X book projection. No producer, build, statistical recomputation, calibration, network request, ledger access, trial write or subagent was used. Only registered inequality arithmetic and read-only SHA-256 hashing were performed.

Registration is `cells/p9x-leverage-proposal.json`: `/limits/L_P` = 1.1674 < 2.0, so the cell is defined; `/limits/G_P` = 0.986241893721804. Parent is accepted H, P9-F0h = P9-F0, per the premeasurement plan. PRE30-MVX uses the new-trial branch; the old-Y1 zero-trial branch is inapplicable. Gross match is none. The plan's registered identity-bridge limitation remains: absolute returns/Sharpe may be overstated by survivorship, with a shared limitation on relative comparisons.

## Independent scaled arithmetic, before return reads

Recalculated r = 2 / 1.1674 = 1.7132088401576153. Let M denote `build-equity/p9x-leverage-readers/mechanics.json`, with each pointer below rooted at `/navs/cell`. All six deciding values are finite. No reader pass label was used.

| Measure and exact M pointer | Observed value | Independently calculated requirement | Result |
|---|---:|---|---|
| `/mean_gross_leverage_all_rows` | 1.5745869901490488 | G_P/L_P-.005 <= value <= 2G_P/L_P+.005; [0.83981916542899093, 1.6946383308579818] | PASS |
| `/mean_net_leverage_all_rows` | 0.009567296396714624 | absolute value <= .02r = 0.034264176803152309 | PASS |
| `/tau_gmv_mean` | 0.027435286052218615 | <= .20r = 0.34264176803152308 | PASS |
| `/tau_gmv_p95` | 0.034127886700244255 | <= .30r = 0.51396265204728453 | PASS |
| `/accounting/max_return_identity_error` | 3.5735303605122226e-16 | <= 1e-9 | PASS |
| `/accounting/max_cash_book_relative_error` | 7.9219371798390921e-14 | <= 1e-9 | PASS |

M `/scenario` is `modeled-1bn-stale5-v1+swap-fin-v1`; `/dir` is `build-equity/mega-nav-p9x-leverage`. M `/daily_csv_sha256` matches the directly hashed primary daily CSV: `faa0b16ae93324e37c6a69f56f80e52bc664ffd111f4ac5400a6ca7b9110b3a3`.

OD5-7/C3-4 realised gross cap 2.0 is distinct from the registered clipped L_t parameter. Native X `build-equity/mega-nav-p9x-leverage/summary.json` `/scenarios/i/exposure/{mean_gross_leverage,max_gross_leverage}`, identified by `/scenarios/i/scenario`, gives:

| i; scenario | Mean gross | Maximum gross | Maximum <= 2.0 |
|---|---:|---:|---|
| 0; linear-6bps-stale5-v1+swap-fin-v1 | 1.5662931516521497 | 1.8584441980160666 | PASS |
| 1; modeled-1bn-stale5-v1+swap-fin-v1 (primary) | 1.5743764230832007 | 1.8619722830838408 | PASS |
| 2; modeled-1bn-terminal-adverse-v1+swap-fin-v1 | 1.5775726999779318 | 1.865985818141723 | PASS |
| 3; modeled-1bn-stale5-v1+flat-300-v0 | 1.5721120212358906 | 1.8671310431406234 | PASS |
| 4; modeled-1bn-stale5-v1+engine-tiers-v1 | 1.574850300557488 | 1.8625576053523885 | PASS |

M `/max_gross_leverage` = 1.8619722830838408 agrees with primary native maximum; `/aim_leverage` = 2.0 identifies the parameter, not a proof of realised exposure. Native scenario mean and all-rows mechanics mean are separate measures, as registered. No cap breach is observed; no seventh acceptance predicate was invented. No L_t descriptive statistic was computed or read.

## Rho-first binding and print-only pair

Before paired scalars, read `cells/logs/p9x-rho-first.json` binding, producer and rho metadata. `/paired/sessions` = 1005 (integer >= 2); `/paired/rho` = 0.9970789273786574 (finite in [-1,1]). `/missing` explicitly identifies `one_minus_rho` and `degenerate`; their null projection is absence, not a measured stable difference. `/provenance` is `inferred FALSE; documented nondegenerate encoding, R15-V2-RHO`, consistent with the plan. Independent ordinary subtraction gives 1-rho = 0.0029210726213425575, far above 1e-6; this is inequality arithmetic, not a replacement stable-form producer statistic. No boundary or encoding inconsistency is evident.

C.11 is unchanged: if 1-rho < 1e-6, SE/t/p print na; B2-13 preserves the same nonpaired X decision. The fallback is not invoked here. Existing paired scalars remain print-only and have no acceptance weight.

The directly hashed `build-equity/p9x-leverage-readers/pair.json` matches rho-first `/sha256`: `b3ec7017daaecefbd7f2543aaadb6b4408d3da8f064d657a22d4395609fcfd35`. Pair receipt directly hashes to registered `73eff67e06d7ee5d83e446f0b087af87371b463bcc8f67a81e5fcb1926b330b1`. Rho-first reports source `4b441f5d7ab9a943c83ab74664c9b2d81097c8ce`, argv SHA `741ee783d7ca4bbe11f2c829d2c75b74a2fbcfb4b12f57bf76a1a070af6a5355`, frozen eval SHA `e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c`, and limits 180 seconds / 1536 MiB / 512 MiB minimum free. These are recorded provenance, not a fresh producer/source audit.

All four rho-first `/files` bindings were directly hashed and matched:

| File | SHA-256 |
|---|---|
| H summary.json | 533d0ef1fffcae07dcdbe1d241373b201a7108eeec842e1d96a40c2988bdca25 |
| H primary daily CSV | d0bfba7604e2d83ef6765f145d6e7541092d8a20cd9ad8e0cbb3837379b0d5dc |
| X summary.json | da726a7dd1db5d867bb291d0cbfe15e281fc5e364cc47efb9784a23bbf9b0108 |
| X primary daily CSV | faa0b16ae93324e37c6a69f56f80e52bc664ffd111f4ac5400a6ca7b9110b3a3 |

H directory is `build-equity/mega-nav-p9-theme-cov-hedge-gm`; X directory is `build-equity/mega-nav-p9x-leverage`; primary daily filename is `daily_modeled-1bn-stale5-v1+swap-fin-v1.csv`. Daily contents were hashed only.

Only the permitted pair result leaves were read:

| pair.json pointer | Value (print-only) |
|---|---:|
| `/paired/dsr` | -0.0342898400160776 |
| `/paired/memmel_se` | 0.03854238099854994 |
| `/paired/t` | -0.8896658464708672 |
| `/paired/lw/p_value` | 0.309 |
| `/paired/lw/p_one_sided` | 0.853 |

## Deciding return comparisons

Source `build-equity/p9x-leverage-readers/book.json`, `/navs/{parent,cell}` identifies H/X and the primary scenario, with daily hashes matching those above. All deciding values are finite.

| Rule | Exact source pointers and values | Arithmetic | Result |
|---|---|---|---|
| Net annual X > H | `/navs/cell/net_annual` 0.08986194230342487; `/navs/parent/net_annual` 0.05689681785648637 | Difference 0.0329651244469385 > 0 | PASS |
| Sharpe X >= H-.100 | `/navs/cell/net_sharpe` 1.8407322968760098; `/navs/parent/net_sharpe` 1.875022136892088 | X >= 1.775022136892088; difference -0.034289840016078266 >= -.100 | PASS |

The conjunction is true: **ACCEPTED**. Paired p values do not overturn the registered nonpaired decision. No unrelated B.7 descriptive figures or Y-F0 results were read.

## Anchoring disclosure

Before deriving, the agent saw the user AGENTS/environment instructions, parent task/release text, and full prepared dispatch. It then saw the allowed prereg sections, whole plan/proposal and projected raw evidence in the sequence above. The permitted prereg/plan themselves contain historical verdict words (including H accepted, MV rejected, W lapsed and Y-1 accepted), historical test/HOLDS/APPROVE labels, reference-pair numbers and registration/source commit identifiers. These were visible registration context; this is not a claim of blindness to all historical results.

Incidental exposure: the initial heading-location command printed all Markdown heading titles of prereg, including unrequested section/appendix titles and historical status words such as ruled/lapsed. A C-heading location command also printed the first heading lines of C.12 (including named unqueued items) and C.15 (lineage-pin citations). A scoped C/E search printed a C.11 filter sentence and the G-B3 table row (descriptive gross floor/target) while locating G-B9. No corresponding unallowed section bodies or old verdict tables were opened. JSON schema-name enumeration exposed mechanics cell key names (including `gross_by_year`, post-ramp fields and `meets_daily_turnover_*`) and book parent key names (including descriptive return/volatility fields), but none of their unlisted values. Attempts to select nonexistent native scenario `name` and book `summary_sha256` metadata printed null, not additional statistics. Rho receipt timing metadata was visible within the permitted binding metadata.

No root result record, own mechanics derivation, ledger, progress, review, handoff, final report, raw console, git subject/body, owner PNG, source code, or unlisted result value was read before deriving. Independence is limited to the registered arithmetic and allowed evidence, with the incidental exposures explicitly retained here. No source/void audit beyond the permitted binding evidence is claimed. No actual refusal occurred.
