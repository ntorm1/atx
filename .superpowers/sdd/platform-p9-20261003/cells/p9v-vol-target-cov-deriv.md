# P9-V independent derivation (CELLS-2)

Verdict: **REJECTED**. The deciding primary S2 net paired dSR of V versus X is
**-0.00040932705902685385**, which fails the strict `dSR > 0` predicate.
All six registered mechanics predicates pass. The separate PRE-13 X-10
selection comparison against H passes, but the necessary V acceptance fails.
Final P9-F selection belongs to the PM; this report neither adopts a cell nor
creates a hand trial. No evidence/cap conflict was found in the inspected scope.

## Scope and order

Independent read in `C:/atx-wt/pool-2`, under the released
`r14-prompts/deriv-p9v-resume15.md` dispatch. Read only prereg A.6, B.0, B.8,
B.7's Acceptance bullet, C.11, C.14 and E's exact G-B9 row; then the permitted
plan/proposal; then raw mechanics; exposure/cap confirmation before returns;
then rho-first and the pair-file hash; then exact paired figures; then H/V
book selection leaves and permitted native confirmation/hashes. The early
exposure confirmation implements the plan's stop-before-returns cap gate.
No producer, build, statistical recomputation, trial write or diagnostic
derivation occurred. Arithmetic below is only registered inequality arithmetic.
No owner PNG, atx-db, raw console or 2024+ session data was opened.

## Six independently evaluated mechanics

Source M = `build-equity/p9v-vol-target-cov-readers/mechanics.json`.
M `/navs/cell/dir` identifies `build-equity/mega-nav-p9v-vol-target-cov`;
`/navs/cell/scenario` is `modeled-1bn-stale5-v1+swap-fin-v1`.
Schema is `atx.wave-reader/v1`.

The registered H constants are `L_P=1.1674`, `G_P=.986241893721804`.
Independently computing `r=2/L_P` gives `1.7132088401576153`.
There is no second scaling from X, no calibration, and gross_match is none.
Every observed deciding value is finite.

| M JSON path | Observed value | Registered formula and numeric limit | Result |
|---|---:|---|---|
| `/navs/cell/mean_gross_leverage_all_rows` | 1.5689054552945432 | `[G_P/L_P-.005,2*G_P/L_P+.005]` = `[.8398191654289909,1.6946383308579818]` | PASS |
| `/navs/cell/mean_net_leverage_all_rows` | .009515183983300727 | absolute value <= `.02*r` = `.03426417680315231` | PASS |
| `/navs/cell/tau_gmv_mean` | .0274239219358025 | <= `.20*r` = `.3426417680315231` | PASS |
| `/navs/cell/tau_gmv_p95` | .0340207434251207 | <= `.30*r` = `.5139626520472845` | PASS |
| `/navs/cell/accounting/max_return_identity_error` | 3.918306651362613e-16 | <= `1e-9` | PASS |
| `/navs/cell/accounting/max_cash_book_relative_error` | 1.0425862266790606e-13 | <= `1e-9` | PASS |

Native V `summary.json` `/scenarios/0..4/exposure/max_gross_leverage`
values are respectively 1.8574538217014218, 1.8610479460638438,
1.864999781180776, 1.8661622915687062 and 1.8616312760698774: each <=2.0.
The corresponding scenario order is linear stale5/swap, modeled stale5/swap,
modeled terminal-adverse/swap, modeled stale5/flat300, modeled stale5/engine-tiers.
Permitted mean exposures also seen were 1.5601522494947695,
1.5686889463334153, 1.5718856185161847, 1.5660362758167476,
1.5691450384302525; these do not replace the all-rows deciding gross field.
V `vol_target.csv` primary-book rows were present and every selected `L_t`
was finite in [1,2]. Only book identification and L_t were inspected; no mean,
change count or volatility statistic was calculated. Realised gross <=2 and
clipped L_t in [1,2] are distinct checks; neither was substituted for the other.

## Rho-first and exact V-versus-X pair

`cells/logs/p9v-rho-first.json` `/path` binds
`build-equity/p9v-vol-target-cov-readers/pair.json`; `/sha256` is
`a8d3da1576d5f620369e48dd8f747395a8f1a2dcec47adf75c17f4de5c1a6303`,
equal to my streaming hash before opening paired figures.
Its `/paired/sessions=1005`, `/paired/rho=.9999794724534574`;
`/paired/one_minus_rho` and `/paired/degenerate` are null in this rho log.
The optional native evidence is not supplied by this encoding. Under the plan's
documented nondegenerate encoding, degeneracy is **inferred FALSE**, not a
supplied native boolean. Ordinary subtraction for the threshold gives
`1-rho=2.0527546542581376e-5`, exceeding `1e-6`; this is not a new stable-difference
statistic. No rounding-boundary concern is apparent. PRE-33/PRE-12 fallback is
not activated, so SE/t/p remain numeric and the degenerate announcement does
not apply.

Exact pair-file leaves, read only after rho:

| JSON path | Value |
|---|---:|
| `/paired/dsr` | -0.00040932705902685385 |
| `/paired/memmel_se` | .003229996480990207 |
| `/paired/t` | -.12672678172744264 |
| `/paired/lw/p_value` | .9072 |
| `/paired/lw/p_one_sided` | .5678 |

Only dSR decides acceptance; the remaining four numbers are nondeciding.
`/protocol=v8`; `/nav_summ_run/argv` names X first, V second and the exact pair
output. `/nav_summ_run/engine_exe_sha256` matches frozen eval
`e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c`.
The script hash is `7b5c5e242e789cb78f2a3fc6c6be2c359344f76f476a2aa31f2cfc4edb29ab69`.
No whole lw/cbb object or confidence interval was printed.

## Separate PRE-13 selection comparison: V versus H

Source `build-equity/p9v-vol-target-cov-readers/book.json` identifies
`/navs/parent/dir=build-equity/mega-nav-p9-theme-cov-hedge-gm` and
`/navs/cell/dir=build-equity/mega-nav-p9v-vol-target-cov`, both primary scenario.

| Selection predicate | Evidence and arithmetic | Result |
|---|---|---|
| V net annual > H net annual | `/navs/cell/net_annual=.0893596400030433` > `/navs/parent/net_annual=.05689681785648637` | PASS |
| V net Sharpe >= H net Sharpe-.100 | `/navs/cell/net_sharpe=1.8403229698169834` >= `1.875022136892088-.100=1.775022136892088`, using `/navs/parent/net_sharpe` | PASS |
| Same six mechanics | All six checks above | PASS |

This numerical selection condition passes independently of the failed
V-versus-X acceptance. No new pair against H was produced or required.
Native H/V `/scenarios/1/net_sharpe` exactly confirms the book-reader values.
Native `/scenarios/1/net_annual` was not supplied under that name; attempts at
four annual-return spelling variants also yielded no value. Annual-return
evidence therefore rests on the explicitly authorized book-reader leaves,
not an asserted native cross-check. No schema inventory or wider read followed.

## Identity and binding evidence

Streaming SHA256 of primary `daily_modeled-1bn-stale5-v1+swap-fin-v1.csv`:

| NAV directory under build-equity | SHA256 |
|---|---|
| mega-nav-p9v-vol-target-cov | `7c00ceb75e22b64722fdc999f7814cd88960577bdc4158aea829864bbde726c4` |
| mega-nav-p9x-leverage | `faa0b16ae93324e37c6a69f56f80e52bc664ffd111f4ac5400a6ca7b9110b3a3` |
| mega-nav-p9-theme-cov-hedge-gm | `d0bfba7604e2d83ef6765f145d6e7541092d8a20cd9ad8e0cbb3837379b0d5dc` |

V equals M `/navs/cell/daily_csv_sha256` and book `/navs/cell/daily_csv_sha256`;
H equals book `/navs/parent/daily_csv_sha256` and the registered parent binding.
Summary SHA256 values respectively are
`ac83c97006150d45a05bec4e5c9895bc773cc9afee73c2878c38d68003d57233`,
`da726a7dd1db5d867bb291d0cbfe15e281fc5e364cc47efb9784a23bbf9b0108`,
`533d0ef1fffcae07dcdbe1d241373b201a7108eeec842e1d96a40c2988bdca25`.
X/H match proposal reference hashes. V native producer is release,
`engine_git_sha=5f48a25d3730789914941fbc7aa42f4458cea6b1-dirty`.
This is a scoped derivation, not a repeat audit of every producer/void receipt.

## Complete anchoring disclosure

Before deriving I saw the root dispatch stating its derivation was committed,
the prepared-scope identifier/hash and instructions, the permitted prereg
sections, and the entire permitted plan/proposal. No root result record,
mechanics PASS labels, hand ledger, reviews, commit subjects/bodies or unrelated
sprint document was opened. No git history was read before deriving.

The authorized sources themselves contain anchoring material: the plan explicitly
says **X is ACCEPTED**, H is the parent, and accepted X is the fallback; the
proposal calls H accepted. I therefore do not claim blindness to X's disposition.
B.0 and C.11 contain historical gate findings, verdict words (including APPROVE,
HOLDS, DIFFERENT, LAPSE-TABLE), hashes, test counts and historical correlations.
B.8 whole contains existing D-COV family/eigen diagnostics and directional
findings; these were seen as required prereg text but were not rederived or used
to decide V. A.6 contains trial rules and Y-1 verdict-of-record language. The
plan contains accounting totals/hashes and preparation disclosures (historical
mean_r2 .24678876760976343, eigen diagnostics, coverage/quantiles, prior H CI/M
covariance/X navigation exposure descriptions). These are exposures to the
permitted document's descriptions, not independent reads of those underlying
artifacts or the actual ledger. Plan/proposal limits were visible before my
independent arithmetic. Their B.8(a) prospective commands were also visible.

Exact-heading extraction printed no unrelated headings or neighboring G-B rows.
The first long displays were truncated; the missing B.0 tail and compact entire
proposal were subsequently displayed. No unlisted result field was printed by
that truncation. Metadata projection later printed the whole permitted
`nav_summ_run` object, including argv, engine/script paths and hashes, git_head
`eecf6ebe9b4a692f4c5d7d83349af683358ad362` and empty warnings, plus native producer's
definition text. Several guessed metadata/annual field names were absent and
printed only absence markers, or nothing. No commit subject was exposed.
All six PASS labels and the V verdict in this report are my own derivation.
The historical required coauthor trailer is literal commit protocol and does
not identify the executing model (GPT-6 Astra).
