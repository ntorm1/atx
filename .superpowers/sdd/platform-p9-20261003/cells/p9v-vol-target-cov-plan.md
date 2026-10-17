# P9-V preparation and exact hand read plan

PREPARATION ONLY under R15-ROOT6. No V spec, output, measurement or trial exists.
Executing model: GPT-6 Astra. The literal historical commit trailer is retained.
X is ACCEPTED and complete; F0=F0h remains H. No adoption or OD3 scope is released.

## Closed registration proposal

`p9v-vol-target-cov-proposal.json` is the complete normative proposal: candidate
template, full old/new template leaves and resolved delta against X, exact native
and bounded argv, producers and pins. Candidate file (not written):
`scripts/specs/p9/templates/p9v-vol-target-cov.json`; LF JSON SHA256
`95cf5c05d1c9257ee10592f255a2321a16f0112edb2305b7876bee973d4e98ed`.
Parent is `p9x-leverage.json` at ac60bd39, SHA256
`e8a308b593b67c16111f3e4792c92983e9c5cfd412c7e42d485ecfe089b549db`.
Pair X NAV is `build-equity/mega-nav-p9x-leverage`, summary
`da726a7dd1db5d867bb291d0cbfe15e281fc5e364cc47efb9784a23bbf9b0108`.

Sole scientific change: vol-target's v11 risk-model path/manifest pin becomes
`build-equity/p9-m2i-dcov-covv1`, manifest
`ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3`.
Covv1 model `0f90bf7c78203ca8c93d6003d58c8b2894747045485137756fbc13a32e3b642a`,
510678016 bytes, root `20919f7b338dfbd786994cbbcb506bdaabd9c3af3266d4394d4d2e07d90c9f8e`.
Old v11 manifest `397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7`,
model `cd48d5749dd7484ca69ae6b0b9df142b73e2beb64c814d3c143982ec59bb2ce3`.
The remaining delta is own name/description/output identity and X reference lock.
No MV target, second store, calibration or new rule. Gross_match remains none
(a protocol property, not an unsupported v1 key). Cap2/floor1/cadence21/annual252
remain fixed; aim-partial trading cadence1 is distinct from vol-target cadence21.
H's completed u/fit/card/w, flag-absent fit, v15 and aim-partial-v5 are reused.
Both inherited print opt-ins appear exactly once. Registration must commit the
entire old/new delta, candidate and resolved hashes/argv/pins in its body, then
adjacent PRE-40 B.8/Appendix IV notes and the final read guard before measurement.

`start-state-6.txt` records actual metadata: all54 exes and CRT match, all126
reference files match six listings, both model hashes streamed, cache686,
research-store/index and fields v9/v10/v13/v14/v15a absent. Receipt source5f48a25d
to HEAD has no atx-engine/atx-impl/CMake difference. Re-pin2078f877 is unchanged;
the subsequent reviewed Python commit-hygiene changes remain disclosed.
COV-9 returns_applied=false. No artifact was rebuilt or model decoded.

## Fixed decision and selection

V acceptance is paired net dSR>0 versus X AND all six finite H-scaled mechanics.
H G_P=.986241893721804 is the registered all-rows mechanics field, not native
exposure mean; H L_P=1.1674, r=2/L_P. Companion binds its exact reader/hash.

| Measure | Limit |
|---|---|
| all-rows mean gross | [.8398191654289909,1.6946383308579818] |
| absolute all-rows mean net | <=.03426417680315231 |
| mean tau_GMV | <=.3426417680315231 |
| p95 tau_GMV | <=.5139626520472845 |
| return identity error | <=1e-9 |
| cash/book relative error | <=1e-9 |

Never rescale from levered X. Separately check native realised exposure maximum
<=2 in all five scenarios and primary L_t in [1,2]; these are different quantities.
A cap/pin/source/driver conflict stops before return reads, with no criterion repair.
When 1-rho<1e-6, PRE-33/PRE-12 keeps point dSR>0 plus mechanics, prints paired
SE/t/p as na and exactly: "pair degenerate: the two stores give the same book to
within 1e-6 of correlation". Boundary or inconsistent encoding stops.

PRE-13 selection is separate: V becomes P9-F iff accepted AND its native primary
net annual exceeds H and native net Sharpe >= H-.100, with the same mechanics.
Otherwise accepted X remains fallback. No extra variant/rerun or adoption follows.

## Exact run and source routes proposed for PM release

Supported command is the companion's `cycle_command`: Python312 -u
`scripts/research_cycle.py run scripts/specs/p9/templates/p9v-vol-target-cov.json --stop-after nav`.
No monitor/summ/trial yet. NAV/mechanics/pair/book caps180s/1536MiB/floor512MiB.
R15-RAM-NOW: state current RAM and C:>=15GB immediately before each run; omit
admission-wait and host-budget arguments entirely. One bounded data run at a time;
no auto-attempt, polling, rebuild, suite or unchanged-tool test. Preserve any actual
resource/driver/permission stop and report it without retry or alternate route.

Own bounded receipt directories, each with precisely start.json, receipt.json,
stdout.log, stderr.log as source inputs:

- `build-equity/mega-nav-p9v-vol-target-cov-run`
- `build-equity/p9v-vol-target-cov-readers/mechanics-run1`
- `build-equity/p9v-vol-target-cov-readers/pair-run1`
- `build-equity/p9v-vol-target-cov-readers/book-run1`
- `build-equity/p9v-b8a-h-nav-run`
- `build-equity/p9v-b8a-book-run`
- `build-equity/p9v-b8a-risk-v11-run`
- `build-equity/p9v-b8a-risk-covv1-run`

Associated unread raw consoles under `cells/logs/` are, respectively,
`p9v-cycle-raw.log`, `p9v-mechanics-raw.log`, `p9v-pair-raw.log`,
`p9v-book-raw.log`, `p9v-b8a-h-nav-raw.log`, `p9v-b8a-book-raw.log`,
`p9v-b8a-risk-v11-raw.log`, `p9v-b8a-risk-covv1-raw.log`.
These forty exact files are the proposed extension, not already authorized by X.
After each producer scan all forms using safe wave_seal and unchanged X
`source_text`/receipt-span helpers (source hash pinned in companion), with explicit
V paths. Never import sealsrc.py. Reused phase logs retain their existing audits;
any fresh u/fit/card/w/ref phase is an unexpected-run stop.

Strict predicates: own2026-10-17 only inside parsed started_utc; exact five owner
PNG names only as complete parsed dirty_outside_pathspec strings; echoed receipt
must equal its associated receipt. Only p9v-cycle-raw.log may classify complete
entries in the canonical `# dirty outside the code pathspec (listed, not a stop): `
header. Closed names: docs/plans/2026-10-02-x5-equity-curve.png,
2026-10-03-p9-b0-yf0-equity-curve.png, 2026-10-03-wq-explore-equity-curve.png,
2026-10-03-wq-explore-sharpe-vs-turnover.png, 2026-10-06-p9-f0-yf0-equity-curve.png
(all under docs/plans). Only matching token spans qualify. Every other location,
day, path or hit remains OTHER and stops reads. Built-in seeds/seal-reference
forms retain their exact existing predicates. No blanket date exemption or wave
checker change. No unsupported seal-allow on v1. New V guard only receives focused
synthetic positive/negative controls; no data probe. PNGs are name-only throughout.

## Ordered, scalar-only read route

1. Source/pins/void/freeze/ledger first. Execute exact companion mechanics argv.
   Read `mechanics.json /navs/cell/{mean_gross_leverage_all_rows,
   mean_net_leverage_all_rows,tau_gmv_mean,tau_gmv_p95}` and
   `/navs/cell/accounting/{max_return_identity_error,max_cash_book_relative_error}`.
   Derive six checks independently, ignore generic unscaled reader pass.
   Native summary only `/scenarios/*/exposure/{mean_gross_leverage,max_gross_leverage}`
   plus scenario names; primary vol_target L_t bounds. Commit
   `cells/logs/p9v-mechanics-first.json` and own derivation before any return read.
2. Execute exact paired command X then V, frozen eval/v8. Scan sources, check
   B2-11 eval SHA e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c,
   pair paths/summary/daily/receipt bindings. Select only
   `/paired/{sessions,rho,one_minus_rho,degenerate}` first. Persist/commit
   `cells/logs/p9v-rho-first.json` before other paired fields. Optional omissions
   mean "not supplied by bundle" and inferred FALSE only under R15-V2-RHO's
   documented valid nondegenerate encoding; never invent a stable difference.
3. Then select precisely `/paired/dsr`, `/paired/memmel_se`, `/paired/t`,
   `/paired/lw/p_value`, `/paired/lw/p_one_sided`. No whole lw/cbb/CI. SE/t/p
   are nondeciding, na under fallback. Book command produces H/V/X; read only
   `/navs/{parent,cell,x}/{net_annual,net_sharpe,x4_net_sharpe,ann_vol,max_drawdown}`
   and binding metadata. H/V annual and Sharpe supply PRE-13. No generic verdict,
   summ/monitor/PBO result, old variant or other diagnostic is opened.
4. B.8(b): exact primary rows of X/V vol_target.csv; expected header
   session,book,rebalance,updated,gross,priced_share,sigma_hat,sigma_ref,raw,L_t,multiplier,clip.
   Select only session/book/L_t: print arithmetic mean L_t of each and V-X change
   in mean, plus the count of consecutive primary-row L_t changes per book.
   No sigma/clip or alternative volatility statistics. Finite [1,2] required.
5. B.8(a) executes its exact preregistered chain below once, after main mechanics,
   with independent source/pin/identity gates. B.8(c)-(e) copy the full existing
   prereg family table, all51 rank rows and directional counts, all three pooled
   eigen rows and D-COV-EIG no-exclusion finding. No new risk-model statistic,
   exclusion or parameter. Preserve near-null ranks00-02 above1000 and39/51 nearer1.
6. Commit own `cells/p9v-vol-target-cov-record.md`, ordered evidence, full file
   sizes/hashes and timings, and STOP for fresh DERIV. Ledger remains139. Only
   literal "DERIV agrees" plus separate PM completion release permits monitor/
   summ and exactly one driver-written construction trial. No manual ledger edit.

## B.8(a) exact prerequisites and producer chain

Companion diagnostic_B8a contains every original and proposed argv as token
arrays, actual build receipt hashes, actual H receipt, runtime producer SHA,
source pin and bounded flags. No step has been launched. COV-9 role=false holds.

Step0 uses actual `mega-nav-p9-theme-cov-hedge-gm-run/receipt.json` command,
including original --aim-leverage1.1828, --calibrate-gross.9862134133265011 and
--calibrate-tolerance.005. These describe the native producer of published H
L1.1674; do not replace them with the plain published-H argv. Only --output moves
to `build-equity/p9v-b8a-h-nav` and --emit-holdings adds
`build-equity/p9v-b8a-holdings`. Compare the complete non-holdings output filename
set and every byte to H, including summary/producer. No normalization/exemption.
Any difference means "not produced: step0 identity difference at <file>" and no
book-csv or risk step. No rerun to repair identity.

Step1 is the exact registered Q5 Python serializer, not an invented C++ verb:
Python312 `atx-impl/tools/book_diagnostics.py book-csv --holdings
build-equity/p9v-b8a-holdings --role build-equity/train-2020-2023-lo3/manifest.json
--role-sha256 e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4
--output build-equity/p9v-b8a-book.csv`. Source SHA256
`b2274b4037d2e9315847e74f2296e2a110ac02a4728c6d6d3d8bd132b922c9da`.
It serializes held dollars/nav_post, not a replacement statistics implementation.
Pin its produced CSV bytes before step2; the companion's sole prospective
BOOK_CSV_SHA256_AFTER_STEP1 token is replaced only by that streamed digest.

Step2 runs frozen Release risk once per recipe. Original store argv in both
receipts is risk --role <same role/pin> --fields fields-v10/manifest.json
--fields-sha256 a4a060ae29c10080ca89b4b29335709558c2ae4842bd29db0398b23a43070809
--output <pinned store> --emit-exposures all --emit-container --bias-families
extended --recipe atx-risk-v1.1 (or atx-cov-v1). Original producerccc73cd1/source
c8ddf736 is recorded, distinct from frozen runtime risk0c110534/source5f48a25d.
Only outputs become `build-equity/p9v-b8a-risk-{v11,covv1}`, fields input/pin
becomes witnessed v15/26fee5ce, and book path/hash is added. No max-bytes,
recipe/default/seed/portfolio change or store mutation/rebuild.
Each diagnostic stage600s/2048MiB/floor512MiB/C:>=15GB, no wait options.

`logs/p9v-fields-witness.json` contains70 streamed equality rows: v11
reused_from v10 payload pin = v15 field = v15 files pin = disk, each PIT true.
Risk source b74618f49e7dd94fafbd03fd1e09ebefb5f996a53caccb2e2b6557810e683740,
strategy_risk_verb.cpp:213-260, loads exactly at, at_lag4, be, gp_ttm, grp_ff49,
lt, me_company, ni_ttm, shares_out, si_dtc, si_shares. All11 are within70;
both pinned store fields_used name/payload sets match them. Existing witness SHA
`0b5b348a2b5f60654387bbfbf2f9ceef9bd5918551eea4dc9e5aa18ef64e2a66`.
No absent-field prerequisite found; execution feasibility and step0 identity are
unobserved. A refusal, outside70 field, absent input, limit/identity failure is
"not produced: <exact step/reason>" with preserved evidence and no probe/retry.
Read only each new bias_summary `/families/book/full_sample/{b_mean,b_median,
series_in_band}` and `/families/book/{status,series,series_ok,observations}`.
Require one ok book series, finite equal mean/median, then label its direction
against the registered band 1 +/- sqrt(2/observations); the producer supplies
the in-band count, and this explicit threshold arithmetic supplies direction.
Any unresolved schema is checked from source, not by a
value-bearing shape dump. Below band means over-forecast/over-corrected; above
band under-forecast; closer to1 alone does not confirm an over-corrected book.

## Fresh DERIV boundary and disclosure

Proposed DERIV inputs: this plan/companion, raw six mechanics leaves, X/V pair
file and exact rho-first file outside NAV dirs, H/V/X book leaves, named receipt
metadata and source reports, registered summary/exposure leaves and primary
daily/capacity files for binding checks, X/V primary vol_target rows, and B.8(a)
identity report/book-bias leaves or not-produced reason. It must not read root's
derived mechanics PASS/checks, record/verdict, trial, completion or PM disposition.
PM supplies a fresh dispatch; no already-prepared DERIV brief was opened here.

R15-V-PREPSCOPE: preparation accidentally printed covv1 manifest /run, including
historical mean_r2 .24678876760976343, and first28 manifest lines containing
existing prereg eigen diagnostics. R15-V-PREPSCOPE-2: a separate v11 si_shares
field-object inspection printed existing coverage/quantiles. These exceeded the
intended metadata projection; tool transcript preserves the raw displays. No raw
payload statistic was computed, no V result existed, and no fixed parent/store/
recipe/threshold/selection choice changed. Do not claim pristine preparation
blindness. Subsequent checks use exact metadata/scalar leaves or expected-type
booleans. Prior H CI, M parent-covariance and X DERIV navigation exposures remain
disclosed; they grant no widened V read scope.

Accounting unchanged: ledger139 SHA5caaccd36762571fffd9ff6fe1fdddfffca69790d2777755df3424fd0643a2dd,
prefix138 fe1bc97ecf9ccef7f042d637730581c3410792798ef2ab0a4a7a7fa2bfbc05f8;
Nc68, historical admissions61/P9+0, Nhand129,M110,Ntot239; eight historical window
reruns add0, history reads0. Future V adds one construction only after DERIV.
Universe linked-operating-v3 and fields identity-bridge-v2-pit depend on2026-dated
inputs; size unmeasured. Absolute return/Sharpe may be overstated by survivorship;
paired relative tests share this identity-bridge limitation.
