# Next parent agent goal prompt — equity platform, checkpoint 12 in progress

Use the following as the next parent agent's continuation prompt. The user explicitly
stopped the previous parent and requested this handoff on September 20, 2026.
Development and all three subagents were stopped. Resume development only when the
user starts the next run with this prompt; this handoff does not claim completion.

## Goal and working priorities

Continue building `C:\atx\atx-engine` into a complete low/medium-frequency equity
long/short systematic trading platform, with `atx-impl` as its concrete working
book and production implementation. The user's aspirational comparators are Jane
Street, HRT, Renaissance and WorldQuant. Treat this as an ongoing recursive
improvement goal, not a claim that ATX currently competes with those firms.

The user specifically instructed:

- Use subagent-driven development to preserve the parent context and develop fast.
- Prefer targeted meaningful verification over extensive test-first development.
- Always use web research where possible instead of guessing:
  **Research → Review → Implement → Measure → Repeat.**
- Use the Downloads `tbltickerhistory` ZIP for daily OHLCV, shares and
  `cumulReturnFactor` where needed.
- Build the actual equity L/S book in `atx-impl`—signals, combiners, optimizers,
  execution and accounting—to expose what `atx-engine` needs next.
- Prioritize genuinely high net out-of-sample Sharpe, high capacity and low
  turnover. These are objectives, not achieved results.

Continue useful engineering autonomously. Do not confuse passing synthetic
checks with accepted alpha, executable capacity or production readiness. No live
trading or broker actions have been performed or authorized by this work.

The existing goal was still `active` when stopped. Do not mark it complete or
blocked just to end a turn. If the next session inherits an unfinished goal, use
it rather than trying to create a duplicate. The user's stop is intentional, not
a technical blocker.

## Critical workspace rules

**All code, documentation and native builds belong in this isolated worktree:**

```text
C:\atx\.worktrees\equity-platform
branch: feat/equity-platform-20260920
```

The original shared checkout `C:\atx` was externally stashed during iteration 8.
The equity work was recovered into the isolated worktree. Do not edit the shared
tracked checkout, apply/pop its stash, reset/clean either tree, or overwrite the
many untracked equity files. They contain real work. No commit or merge was made
in this cycle.

Recovery reference: stash commit
`8f36c2682a811ecc123d8a9588c57ac3177c7c3a`, based on
`dffb609b7a3ccf874b88a07ae407a16961140c84`.
Recovery receipt: `build-equity/audits/iteration8-worktree-recovery.json`.
Shared immutable data and older audits remain under `C:\atx\data` and
`C:\atx\build-equity\audits`.

Read the isolated `CLAUDE.md` and `.agents/cpp/agent.md` before C++ work. They can
be ignored by ordinary file search, so read the explicit paths. User instructions
for faster development take precedence over their generic TDD guidance.

The user asks for graph-first code discovery: `search_graph`, `trace_call_path`,
`get_code_snippet`, `query_graph`, `get_architecture`. These tools were **not
available** in the last session after searching `ALL_TOOLS`; targeted `rg` was
therefore used. Check again in the next session, then fall back if still absent.

Windows PowerShell; permission policy was `never`, full filesystem access.
Do not pass `sandbox_permissions`. Use UTF-8 explicitly in Python text I/O.
Never chain destructive commands across shells.

Native configure/check/build/test/run actions are **parent-only**, serialized
through this worktree's absolute wrapper:

```powershell
Set-Location 'C:/atx/.worktrees/equity-platform'
$env:CCACHE_DISABLE='1'
& 'C:/atx/.worktrees/equity-platform/scripts/atx-build.ps1' ...
```

Use `equity-dev`, static Debug, clang-cl 18.1.8, 4 build jobs. Keep ccache disabled:
cross-worktree PCH reuse previously failed. Avoid more than two or three wrapper
invocations in one PowerShell process; a fourth previously made `vcvars64.bat`
exit 255, fixed by a fresh shell. No raw Ninja/CMake builds, clang-format or
clang-tidy. PCH is enabled; do not claim a hygiene/include-clean build occurred.
Dependencies: classic vcpkg at `C:/Users/natha/vcpkg/installed`, Arrow 24,
GoogleTest 1.17. About 16 GB host memory; budget large research jobs explicitly.

## Exact stop point — checkpoint 12 is NOT validated yet

The latest **completed** checkpoint is 11. Checkpoint 12 adds a pure signed
stock-and-cash corporate transition planner and a separate cash-payment planner.
The implementation and three grouped fixtures exist, but **no checkpoint-12
translation-unit compile, target build or native test has run**.

The last completed native tool action was **configure only**, exit 0:

```powershell
$env:CCACHE_DISABLE='1'
& 'C:/atx/.worktrees/equity-platform/scripts/atx-build.ps1' configure `
  -Preset equity-dev -Groups 'risk;data;core;book' `
  '-DVCPKG_MANIFEST_MODE=OFF' `
  '-DVCPKG_INSTALLED_DIR=C:/Users/natha/vcpkg/installed' `
  '-DFETCHCONTENT_BASE_DIR=C:/atx/.worktrees/equity-platform/deps/equity-dev'
```

Its log is `build-equity/audits/iteration12-configure.log`. CMake regenerated the
Ninja graph successfully. `atx-engine/CMakeLists.txt` now registers
`src/book/security_transition.cpp`; the existing `book` test group discovers
`*_test.cpp` with `CONFIGURE_DEPENDS`.

At stop, a process check found no running `atx-impl`, book-test executable,
clang-cl, Ninja, CTest or CMake. All three agents were explicitly interrupted:

| Agent | Ownership / last status |
|---|---|
| `/root/replay_policy` | New two headers and implementation. Released stable source after read-through and requested clock fixes. No native commands. |
| `/root/constrained_allocation` | New transition fixture only. Three groups and native numeric stdout export exist. Interrupted before final handoff/review completion. |
| `/root/source_audit_peer` | Independent exact arithmetic oracle completed. Native comparator and independent implementation review were requested but interrupted; no comparator file existed at stop. |

If these agents survive in the next session, explicitly reassign bounded work;
otherwise create equivalent disjoint agents. They share the same worktree.

### Files added or changed in this unfinished cycle

All paths below are relative to the isolated worktree.

| File | State |
|---|---|
| `atx-engine/include/atx/engine/data/security_transition.hpp` | New terms, identity, evidence, basis and payment types |
| `atx-engine/include/atx/engine/book/security_transition.hpp` | New borrowed views, fixed-size owned plans and two public planner functions |
| `atx-engine/src/book/security_transition.cpp` | New implementation, not compiled |
| `atx-engine/tests/book/security_transition_test.cpp` | Three grouped fixtures, not compiled/run |
| `atx-engine/CMakeLists.txt` | Added new source to engine library |
| `atx-engine/docs/SECURITY_TRANSITIONS.md` | New API/accounting guide; accurately says replay integration is later work |
| `build-equity/audits/iteration12-security-transition-plan.json` | Predeclared bounded plan |
| `build-equity/audits/iteration12_transition_oracle.py` | Independent Fraction/Decimal oracle writer, executed successfully |
| `build-equity/audits/iteration12-transition-oracle.json` | 14 exact mathematical scenarios; native comparison pending |
| `build-equity/audits/iteration12_run_transition_checks.py` | Parent-authored capture/archive/CTest runner, **not run** |

No checkpoint-12 validation receipt, native snapshot, test log, test XML or native
comparison receipt exists yet. Do not imply otherwise. `PLATFORM_PROGRESS.md` and
both project READMEs still describe completed checkpoint 11; update after actual
measurement, retaining historical entries and immutable receipts.

Stop-time SHA-256 pins:

```text
data/security_transition.hpp
77e149a8880af18483da5fac8309d1001a55763b8d1be516727a5f7e840523fc
book/security_transition.hpp
3c77b2384dad7b1bbc3f7c20e4530b38b80081ddfc5b868563cee81e2d6a3ca2
src/book/security_transition.cpp
a8552c432f9eed6a22d6ee52a7d1fe260167c71ce50ea8e5e7147a550074033f
tests/book/security_transition_test.cpp
7c593bfebe117740439b60c4d930059d847e24512bd911bccfd63d0239bad9cd
iteration12-transition-oracle.json
d0c81c7f4c5e3356816055883a89e811d07206f2804a6761ab1c409a4889df42
iteration12_run_transition_checks.py
8de2452df271e1f564aaba0a8593320db7e0765f3bec4b6eca35bb853d795862
```

### Checkpoint 12 design and implemented contract

Read `atx-engine/reviews/2026-09-20-iteration12-security-transition-design.md`.
Its frozen SHA is
`0a964aebc15f14ea25ddd19fad5be723835b6b7a6e99a6aa308f3b7ecf95caee`.
It was written before implementation and its closing statement describes that
design-time status. Preserve its hash; write a later validation record separately.

Supported first slice:

- One predecessor and one different successor identity/axis, a positive rational
  stock ratio, optional nonnegative USD cash per **pre-transition predecessor**
  share equivalent. Ratio integers must fit exact binary64 integer range.
- Only explicit `SyntheticFixture`, research reinvested-share-equivalent units,
  and fixed USD face-value claims. Unknown defaults, real historical modes and
  physical-share mode reject.
- Source/axes/adjustment recipe and mapping digests must be nonzero and agree
  between state, event and basis. Digests bind supplied evidence, not its truth.
- Stock, cash and vendor continuity must each be `ExcludedFromTri`.
  Embedded/unknown components reject to avoid duplicate economic application.
- Strict starting state version; sequence must equal last sequence + 1; no wrap.
  Borrowed ID guards are bounded by admission count and sorted unique nonzero
  IDs. Duplicate events/revisions, claims or payments reject.
- Last valuation < effective <= entitlement <= next valuation <= knowledge
  cutoff. Basis marks match supplied valuation endpoints. The caller remains
  responsible for actual adjacent required observations/calendar coverage.
- The root review added two chronology gates: basis evidence availability cannot
  predate the successor observation; actual payment evidence availability cannot
  predate allocation. Event terms may be announced before effectiveness.
- Inputs are immutable. Successful plans have fixed-size owned values; no I/O or
  successful-result dynamic allocation. Error strings may allocate.
- A later state owner must commit every delta and ID atomically while the version
  still matches. A pure plan does not erase a prior missing valuation.

Arithmetic order:

```text
converted_equivalents = old_tri_units * (old_TRI / old_raw)
delivered_equivalents = converted_equivalents * stock_ratio
signed_claim         = entitled_pre_split_equivalents * cash_per_old_share
successor_units_added = delivered_equivalents * (new_raw / new_TRI)
successor_units_after = successor_units_before + successor_units_added
old_units_after       = 0
settled_cash_after    = settled_cash_before
bridge = actual_incremental_successor_value + signed_claim - removed_old_value
```

The bridge spans the supplied marks; it is not automatically instantaneous
corporate-action profit. Existing successor holdings may net, including exact
zero cancellation, but delivery attribution is retained. Numerical netting does
not prove a stock loan was discharged.

Every nonzero economic leg must remain representable with its sign. Reconcile
within `64 * binary64 epsilon * max(abs(actual), abs(expected))`, without an
absolute tolerance floor or cash plug. Overflow, underflow and absorbed additions
reject. This deliberately rejects a tiny claim paid into cash too large to retain
its increment faithfully.

Payment requires exact matching full signed amount, identity, valid origin
version/sequence, allocation at/after recognition, evidence and unused payment
ID. Payment adds the claim to cash and removes it from pending claims. A negative
claim is a payable and debits cash. Partial/mismatched/unproved allocations reject.

Canonical independent example: old units 2, raw10, TRI30 gives six equivalents;
stock ratio1/2, cash40491/10000; successor raw12, TRI60. Delivered units0.6,
stock value36, signed claim24.2946, bridge0.2946 against old value60. With existing
successor units-0.4 and cash100, NAV136 becomes136.2946; payment cash124.2946
preserves that NAV. Mirrored short: payable-24.2946, NAV64 becomes63.7054,
payment cash75.7054.

The exact oracle contains 14 cases including independent normalization changes,
non-power-of-two scaling and exact netting. Current native fixture exports **five**
numeric cases: long/short canonical and three long rescalings (old8, successor4,
both). Other assertions, including exact cancellation and rejection paths, occur
inside the three groups. Do not claim all 14 oracle cases were executed natively.

The export prefix is `SECURITY_TRANSITION_MEASUREMENT `, followed by JSON with
`schema`, `case_id`, `inputs`, `values`, `residuals`. It uses classic locale and
`max_digits10`, actual planner results, no JSON dependency or file I/O. CTest
verbose output may prepend a test number; locate the prefix before decoding.

The parent runner will preserve selected source plus the test executable in
`iteration12-native-snapshot.zip`, verify archive readback, then invoke the
absolute wrapper's serial CTest with regex `^SecurityTransition\.` and verbose
output. Intended outputs:

```text
build-equity/audits/iteration12-tests.log
build-equity/audits/iteration12-wrapper-test-output.log
build-equity/iteration12-tests.xml
build-equity/audits/iteration12-native-measurement.json
```

Review the runner before use. It asserts output paths do not already exist,
records source/executable hashes before and after, and preserves the first
attempt. If a run fails, keep that evidence and version a subsequent attempt;
do not overwrite it. The archive is not a portable complete rebuild bundle.

## Recommended immediate continuation

1. Verify branch/worktree and stop-time files. Do not redo completed data scans or
   checkpoint-11 replay. Read the transition design/API/implementation and this
   cycle's fixture.
2. Delegate the independent comparator/review and fixture completion to disjoint
   agents. The comparator must be independent of C++ code, require the expected
   five case IDs, bind inputs/outputs with hashes and use defensible numerical
   bounds. Near-zero bridges/residuals need bounds based on contributing legs,
   not an arbitrary relative test against zero. Record the nine oracle-only cases
   separately. Review was unfinished when interrupted.
3. Parent runs a single-TU check, then owning target build, then the focused
   measurement. The configure step already succeeded. Example, fresh shell:

   ```powershell
   Set-Location 'C:/atx/.worktrees/equity-platform'
   $env:CCACHE_DISABLE='1'
   & 'C:/atx/.worktrees/equity-platform/scripts/atx-build.ps1' check `
     atx-engine/src/book/security_transition.cpp `
     atx-engine/tests/book/security_transition_test.cpp -Preset equity-dev -Jobs 4
   & 'C:/atx/.worktrees/equity-platform/scripts/atx-build.ps1' build `
     atx-engine-book-tests -Preset equity-dev -Jobs 4
   ```

   Check each exit code; do not proceed on failure. Then from a fresh shell with
   ccache disabled, run `python build-equity/audits/iteration12_run_transition_checks.py`.
   Do not build the whole repository out of habit.
4. Compare actual native values with the exact oracle. Address real failures,
   preserve attempted runs and finish a compact checkpoint-12 validation receipt
   and progress entry. Do not repeatedly broaden tests after the relevant checks
   pass unless changes or unresolved defects justify it.
5. Next engine requirement is **claims-aware state ownership and replay/accounting**:
   atomic commit, pending claims in NAV, settled cash separate, mandatory movements
   distinct from trades, retired-identity target protection, and explicit event-time
   borrow treatment. Allocation certificates must use the same claims-aware NAV.
   Use this to advance the `atx-impl` book; avoid a disconnected generic event
   framework or endless audit-only cycles.
6. Actual PCS historical admission remains rejected until evidence is sufficient.
   Do not force a completed backtest by inventing prices, proceeds, continuity,
   loan transfer or excluding the troublesome security retrospectively.

## Current working book and latest completed evidence

Read:

```text
atx-engine/docs/PLATFORM_PROGRESS.md
atx-engine/README.md
atx-impl/README.md
atx-impl/docs/EQUITY_BOOK_BASELINE.md
atx-impl/docs/EQUITY_BOOK_CONSTRAINED.md
```

The implementation has a reproducible `equity-baseline` signal/combo/target pipeline
and an `equity-book` constrained allocation/replay path. The frozen baseline uses
two fixed momentum expressions:

```text
ts_mean(delay(close, 21) / delay(close, 252) - 1, 5)
ts_mean(delay(close, 21) / delay(close, 126) - 1, 5)
```

Common readiness, rank/winsor0.025, equal blend, 256-observation warmup, weekly
decisions. Research split declared training2013–2019, validation2020–2022,
sealed2023–2025. Do not tune against sealed periods.

Current constrained profile:
`constrained-preference-weekly-observed-close-v3`.
Full original evaluation window 2013-04-04 through 2013-12-31, 189 observations,
1661 canonical instruments, 38 scheduled preferences, $100m initial NAV,
one-observation execution delay, 5bps each-direction actual-dollar trade fee,
365bps annual simple short borrow on ACT/365.

Allocation uses 63 adjacent decision-time returns (64 observations), population
variance floor1e-6, and preference-tracking objective
`0.5 * ||w-preference||² + sum(variance_i*w_i²)`. This is not calibrated expected
return. Constraints: net0, gross<=1, name<=1%, full-L1 trade budget<=20% against
execution-time marked holdings, including mandatory exits and immediate post-fee
recertification. No sector/beta/full covariance, participation, real borrow
availability or capacity qualification yet.

QP: 1200 iterations, rho1, sigma1e-6, base feasibility1e-8, Ruiz10, polish with
three refinement iterations/four solves, raw residual gate1e-6, guarded factor
payload256MiB. Earlier iteration9 repaired polishing to target unregularized KKT
equations and retain coherent primal/dual/normal-cone evidence.

Execution intents: `TargetWeight`, exact `HoldCurrent`, exact `Close`. Holds copy
held units/value; closes charge actual external dollar movement. Opt-in
`MachinePrecisionIntentsV1` removes only machine-scale dust under a fixed error
budget, records raw weights, and recertifies represented economics. It is not an
economic no-trade band. All held positions always require current marks.

Checkpoint11 adds opt-in `ObservedCloseEntryConstraintV1`: exactly unheld names
with no finite positive current execution close receive weight0 equality before
the QP. Original union, preferences, decision eligibility and risk remain. Public
helper default is strict requested-mark failure. The same sampled close is a
research abstraction; availability before real order submission is unverified.
No volume filter, future-missingness filter or permanent exclusion was introduced.

Latest completed receipt:

```text
atx-engine/reviews/2026-09-20-execution-availability-validation.json
SHA256 33336602e1a636e44cb1e999e75caf902f9c00b0d0213e22cbda8b27f6a45540
```

21 affected allocation/stage checks passed, including three new cases, 16.57sec.
An initial test-only std::string-versus-JSON comparison compile error was fixed
with explicit extraction; production had already built. Independent helper/stage
reviews passed. A previous dense-PCG reference battery timed out at120sec and
remains incomplete; it was not rerun or silently counted as passing.

Native checkpoint11 output:

```text
C:/atx/data/equity_book_training_2013_observed_close_20260920
```

Four certified proposals at decision0/5/10/15, execution1/6/11/16. Union sizes
973/988/1009/1002. Additional unavailable-entry zeros0/2/0/0. At execution6,
canonical604 GNW and865 are constrained. Exact Hold counts0/24/59/108. Each
rebalance uses about20% full-L1 turnover; post-fee gross approximately
0.200020002,0.390991874,0.535510089,0.717373455.

Independent verification reproduced all four proposals and **18 complete
intervals** of trades/cash/borrow. Startup has381 positions (192 long,189 short)
and agrees with the independent KKT oracle to max4.34e-19 per weight; objective
difference5.78e-20. This does not prove later QP optimality or profitability.

The run **fails**, as intended, before the period19 valuation on May1,2013:
canonical instrument1063, vendor security146189, archive tickerPCS. Held quantity
`-1888.5372719122356` TRI research units; last valid April30 mark11.84; signed
notional `-22360.28129944087`. Last resize execution11, exact Hold execution16.
May1/2/3 are absent original source rows, not parser loss or quarantine. No borrow
debit was fabricated for the incomplete interval. No completed manifest, report,
Sharpe or capacity result exists.

Native runtime32.6335533sec; peak working set163254272bytes,
peak private commit152481792bytes. Producer executable still at
`build-equity/bin/atx-impl.exe`, SHA
`80e457527154cb2ca7e9409be1627305d1968caddc64545daa088c14d0ee4fa1`.
The pre-checkpoint12 book-test binary is stale for the new source, SHA
`aea986f46e034570bad99092b05847dedeb5d4c3ef8a92847f4d97bb2079645d`.

Checkpoint11 preservation and independent evidence:

| Artifact under isolated `build-equity/audits` | SHA-256 |
|---|---|
| `iteration11-native-snapshot.zip` | `df04fa4d84402850db5b2d82532acd2ab9cfda1e1113d28722bfe74037c9f928` |
| `iteration11-allocation-proposal-verification.json` | `ce58228dd002a6f2d0b001f4080281cf727c8da54f1a2a6560ee600700da6be0` |
| `iteration11-startup-native-comparison.json` | `e084fd49c32984388bf57eb9cfaa34cd954e6f42be8570b0aa8309b919641616` |
| `iteration11-held-gap-diagnostic-qualified.json` | `46954dae8f1cd456ff6177af18be5a4c85aa106896dcbaf841959705ebd57916` |

The archive has39 selected executable/source/helper members plus its manifest;
it is not a full repository/dependency/data/compiler reconstruction bundle.
The independent verifier does not recompute the C++ float-JSON mark digest due
to serialization differences; it separately checks raw binary row identities and
integer reason-vector digests. Preserve that qualification.

Earlier immutable receipts, for historical context:

- Checkpoint9 QP polish:
  `2026-09-20-qp-polish-validation.json`,
  `f01564db748ad5e64a1f58a1dd9bf9a98471fd369cf107d0b8def669783e7fc1`.
- Checkpoint10 representation:
  `2026-09-20-execution-representation-validation.json`,
  `8337f0c8b5f82a94e4f35573f7af2737594d20816f1d76bc11dbd2a3ebea603f`.
  It removed592 dust positions totaling3.04435e-25 weight; then failed on a new
  unpriced GNW entry, not a held dust mark. Its executable was not separately
  archived before checkpoint11 relink; do not claim otherwise.

Do not rerun old receipt writer scripts: they bind historical bytes and evidence.

## Data inventory and qualifications

Original user archive:

```text
C:/Users/natha/Downloads/tbltickerhistory3_10y.zip
bytes: 3542506361
SHA256: 7d2b7a615a08ef3b8686608eda384c4e30e2ce23bdee294ea141b3c9334acd8f
member: tbltickerhistory3_10y.txt
member bytes: 11084562320; CRC15b6e051
```

31,598,499 rows,71 columns,3576 dates2012-03-26..2026-06-15,24959 IDs.
Invalid ID0 rows351651;489 conflicting positive-ID duplicate rows. A filename-only
Downloads search for `*ticker*` in this cycle found only this archive, not a
separate ticker/security-master file. No new full ZIP scan occurred this cycle.

Prepared bounded training source:

```text
C:/atx/data/tickerhistory_training_20120326_20131231_20260919
445 dates; 2910733 selected rows; 2883147 accepted; 27586 quarantined
accepted.zip SHA256
6131dc6482ee9e292c541bc14a76c0ba2b86c92582f7ed21cb8452985f509249
manifest SHA256
ee17d68bfe33d58fa7279877c43d370a6b6b6a577bdb11082a70babec6e8c3df
```

Whole-row rejection for invalid OHLC/order/factor/product/negative volume and
conflicting positive-ID duplicates. Shares invalidity is flagged. No repaired
rows or synthesized quotes. Provider/factor semantics, vintage, instrument type
and borrow remain unverified; earlier KLAC2026 split/factor1 contradiction is
documented. An optional vendor/documentation question was already asked without
an answer; don't repeatedly block engineering or ask it again reflexively.

Frozen research context:

```text
C:/atx/data/tickerhistory_training_native_20260919/context.bin
445 x 1661 x 12; 71697242 bytes; 425000 eligible cells
context ID ec572b826dce65fbd4cd391921f57f01e1ce084864ec84b3e6a944003ead8079
payload SHA c219dc237a58574e4b55df8ee51e9f239ef9d5963903e54aa253bd07eafb85ef
manifest SHA e9d53a8932b242750f44f8744a46856b535a0b1c27bd91dcf8dda60555e5f68a
```

Underlying native panel:7650 IDs,16 fields,409556920bytes. Frozen universe:
top1000, raw close>$5, current-inclusive21-observation raw-dollar ADV>=$20m,
no sector requirement, market-cap threshold0, compact, unaugmented. Mask and
finite seven-field cells were independently matched to accepted source.

Baseline intermediates used by `equity-book`:

```text
C:/atx/data/equity_baseline_training_2013_20260919
evaluation ID 57b7c21c9320e98c1dee6c97e5f3b8cde211c5efdd688c2daf6fb28daa3cd475
combo ID def31e5ca03dc2329bb05a2526c4470e77028fea060d0c3743634d6b4f86a692
books ID 615019e7c83c34adc950bf41bda2e28923960249fbef0fccd8b84e14c251eb88
```

Source reconciliation:
`C:/atx/data/equity_source_reconciliation_2013_20260919`.
110 required gaps across34 IDs/74 dates:104 absent original rows,6 quarantined
OHLC rows,0 parser losses. AuditID
`7dda241005f6f4f4549bba3acfab1e5982aeff0d71c9bc66443fada7d06965ea`,
manifest SHA
`5aa7119684701827529591aa199a860b2e83a38eff17b3d5eb1168cc057032c1`.

## Primary research and why PCS is not patched yet

The source gap coincides with the MetroPCS/T-Mobile transaction. Primary sources
support stock and cash terms, but not all historical accounting/admission inputs:

- SEC closing 8-K:
  https://www.sec.gov/Archives/edgar/data/1283699/000119312513193449/d527693d8k.htm
  April30,2013; one-for-two reverse split and $4.0491 per pre-split share;
  continuation as TMUS. A filing cover date is not proof of earlier availability.
- Charter:
  https://www.sec.gov/Archives/edgar/data/1283699/000119312513193449/d527693dex31.htm
  Says4:01pm Eastern Standard Time April30. Do not guess the UTC instant across
  its daylight-saving ambiguity. Holder aggregation/fractional cash/withholding
  rules are not established by applying continuous research quantities.
- OCC memo32600, MIAX copy:
  https://www.miaxglobal.com/sites/default/files/alert-files/PCS-TMUS_Memo.pdf
  Effective May1; old100-share option basket becomes50 TMUS+$404.91.
  TMUS1 is an option adjustment symbol, not a substitute equity quote.
- Issuer Form8937:
  https://s29.q4cdn.com/310188824/files/doc_downloads/merger_info_docs/1500056517.pdf
  OldCUSIP591708102, newCUSIP872590104, April30 action; signed June14, later.
  Captured PDF SHA
  `96be549ec0715d8b2e9d6fed8078a2f94d8449013b76c161c70b1f6fe1182c44`.
- Issuer announcement:
  https://www.t-mobile.com/news/press/t-mobile-and-metropcs-combination-complete-wireless-revolution
- SIFMA2000 lending guidance, section8:
  https://www.sifma.org/wp-content/uploads/2024/06/Master_Securities_Loan_Agreement_MSLA-Guidance_Notes_2000_Version.pdf
  Distinguishes cash-distribution transfer and noncash additions to loaned
  securities; supports separate obligations, not assumed historical loan terms.
- IBKR payment-in-lieu description:
  https://www.interactivebrokers.com/campus/glossary-terms/payment-in-lieu-of-dividends/
- Portfolio optimization/accounting reference:
  https://www.cvxportfolio.com/en/stable/_static/cvx_portfolio.pdf
- Execution availability reference inspected earlier:
  https://raw.githubusercontent.com/cvxgrp/cvxportfolio/1.5.0/cvxportfolio/simulator.py
  Distinguishes policy data and realized trade filters; not proof of information
  being available before a real order.

Do not apply a real PCS event until vendor predecessor/successor mapping, TRI
component coverage, entitlement basis, evidence availability, exact boundary,
successor marks, fractions, payment allocation and stock-loan handling are
established. Continuous `q * TRI/raw` is a research share equivalent, not verified
beneficial or borrowed physical ownership. Shares outstanding are not book shares.

Existing `data/corporate_actions.hpp` and `data/adjust.hpp` handle a separate
split-only-factor plus cash-dividend schema. They do not establish
`tbltickerhistory.cumulReturnFactor` coverage or provide mixed inventory-event
accounting. They were intentionally left unchanged in checkpoint12.

## Integration constraints after checkpoint 12

`book/replay.hpp/.cpp` currently carries cash and marked equities only. All held
marks are validated before callbacks, including a requested Close. Final
observation is valuation-only. Borrow is charged on post-trade short dollars for
the entire observed interval, with actual calendar duration. That convention does
not yet establish event-time loan transfer or financing.

`atx-impl/src/equity_allocation.cpp` currently validates
`cash + equities == pretrade_nav`; it needs explicit pending-claim state and
post-trade certification. Never hide claims in cash. A delivered successor holding
may be ineligible or lack63 valid adjacent returns; don't invent history or let
Hold bypass eligibility/risk. Mandatory inventory is distinct from alpha intent,
and a delayed predecessor target must not reopen a retired representation.

The eventual adapter should preserve canonical axes, reject missing successor
marks and earlier unresolved valuations, admit bounded event/claim/receipt counts,
retain signed receivables/payables, and publish no partial committed state on
failure. Plan fixed-size events with O(events+claims) retained state rather than
event-by-date-by-universe arrays. Keep mandatory movement out of turnover and
trade-fee ledgers; fees and financing require their own explicit conventions.

After a truthful complete baseline is available, return attention to the user's
investment priorities: comparable signal families, calibrated combination,
factor-aware risk/optimization, cost-aware holding horizons/no-trade policies,
borrow and participation evidence, capacity curves and robust held-out evaluation.
Do not let repeated accounting/audit cycles become a substitute for advancing the
actual `atx-impl` book. Preserve the research split and report rejected experiments
as clearly as accepted ones.
