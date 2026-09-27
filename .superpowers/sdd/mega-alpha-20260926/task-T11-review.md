# Task T11 review: admission screen `v3-admit-v1` + resumable/incremental weight fit

Reviewed: root commit `cd2ec619` (package `review-T11.diff`: `atx-impl/tools/fit_composition_weights.py` +/-825,
`atx-impl/tools/test_fit_composition_weights.py` +/-286). I read the diff once, in two passes (tool, then tests).
Line numbers are new-file lines, mapped from the diff's hunk headers (`F:` = the tool, `T:` = the test).
Outside the diff I made two focused checks, each for a named risk:
- **Runner weights parser acceptance.** I read `strategy_ic_runner.cpp:603-670` at the root's HEAD `8e8bf174`.
- **Candidate cache layout after the T1-fix and T7 commits that landed after `cd2ec619`.** I read
  `strategy_ic_runner.cpp:52,60-99,682-760,871-920` and library `pv_fields_ic121_v3.json`.

### Spec Compliance

- ✅ **Spec compliant.** Everything the brief and the two root additions ask for is implemented. Each check is
  cited below. One integration blocker comes from commits that landed after this one; see I1.
  - **Orientation (brief 1).**
    - `s_k = sign(mean unsigned f over live FIT days)`, and 0 on an empty series or a mean of exactly 0
      (`F:699-701`).
    - The runner sign is reported, not used: `sign_agrees` (`F:890`) and `admission.sign_conflicts` (`F:914`).
  - **Turnover (brief 2).** `tau > 0.70` gives `reject_turnover` (`F:708-709`). tau is computed over all TRAIN
    decisions on the unsigned book, which is sign-invariant (`F:586`).
  - **Stability (brief 3).** The check is `fit_sharpe > 0 and hold_mean > 0` on `s_k f`, with FIT and HOLD taken
    over live days (`F:702-704`, `F:710-711`).
  - **Redundancy (brief 4).**
    - Survivors are sorted by descending FIT Sharpe, ties in library order (`F:717-718`).
    - A candidate is compared against already-admitted ones only. `|rho| > 0.70` rejects it and names the largest
      |rho| (`F:720-733`).
    - A pair with fewer than 250 common FIT days where both are live, or an undefined rho, counts as uncorrelated
      and goes into `low_overlap_with` (`F:723-726`).
    - Pearson rho is taken on the unsigned f, and |rho| does not depend on sign (`F:682-690`).
  - **Insufficient (brief 5).** Fewer than 250 live FIT days rejects (`F:706-707`). Precedence is insufficient >
    turnover > unstable > redundant, and every failed check is listed. This is a decision that is recorded in the
    report.
  - **Output.**
    - `admission.json` has schema `atx.dsl-admission/v1`, screen id, rules, inputs with SHAs, window and counts
      (`F:898-915`).
    - Each candidate row has id, s_k, tau, FIT Sharpe, HOLD mean/Sharpe, max|rho| + with, and status
      (`F:886-896`).
    - `admission.csv` is written by `F:753-770`.
    - Bytes are canonical, and publication is exclusive and all-or-nothing (`F:774-791`).
  - **Weight fit over admitted only.** `eligible` is the admitted set (`F:884`) and `active ⊆ eligible` (`F:940`).
    Everyone else keeps `weight 0.0` (`F:931`). The fit runs on full TRAIN `s_k f_k` with flat days = 0
    (`F:925`, `F:944`). `sum_k w_k tau_k` is still reported (`F:947`).
  - **Fixture.**
    - `Admission` (`T:647-771`) has one candidate per rejection class plus two admitted.
    - The correlated pair `slow_a`/`slow_a_twin` is resolved by FIT Sharpe (`T:681-682`).
    - Bit-stability is proven by comparing seven incremental paths against the fresh run (`T:721-771`).
  - **Root addition (a).**
    - Records live at `<work>/<train-sha>/<semantics-tag>/factors/<cache-payload-sha>.json` (`F:612-622`).
    - Each record is verified on read: self-hash, schema, semantics, role SHA, payload SHA, decision count and
      value types (`F:590-604`). It must also name the loaded context's digest (`F:819`).
    - `--max-seconds` is a soft stop, checked between candidates (`F:825`). It raises `Incomplete`, which exits 3
      with nothing published (`F:1031-1033`, `T:725-735`).
    - Outputs are byte-identical to a no-work-dir run (`T:737-771`).
  - **Root addition (b).**
    - The weights JSON has top-level `train_manifest_sha256` (`F:969`) and `signs` (`F:970`). `signs` holds the
      FIT-mean screen sign for every candidate whose applied sign is nonzero, which covers every positive weight.
    - Conflicts appear as `admission.json.sign_conflicts` (all candidates) and
      `composition_weights.json.provenance.sign_conflicts_weighted` (w > 0 only). The weights file has no key
      literally named `sign_conflicts`, so whoever reads it (root or the T7 lane) must use those two names.
- **Global constraints, checked explicitly.**
  - **TRAIN only.**
    - The role seal is unchanged: `end_ns <= 2023-01-01`, and the last session is before it (`F:201-209`).
    - The sidecar must have `role == "train"` (`F:263`).
    - The FIT/HOLD masks fall inside the decisions, which are all < `TRAIN_END_NS` (`F:871-872`).
    - No validation or 2025+ path exists.
  - **FIT = 2020-01-01..2021-12-31, HOLD = 2022** (`F:94-95`).
    - The windows are assigned by decision session.
    - The labels of the last FIT decisions fall in the first sessions of 2022, but the FIT and HOLD return
      intervals stay disjoint: FIT's last label is (d+1 → d+2) for the 2021-12-31 decision, which ends before
      HOLD's first label starts.
    - The basis is recorded in `rules.window_basis`. ✅
  - **Every admission decision is emitted.**
    - Exit 0 publishes the weights and the table (`F:997`).
    - Exit 4 publishes the table only (`F:956`, `T:773-781`).
    - Exit 3 stops before any decision is made.
    - After the screen, the only other refusal is the 1 MiB weights bound (`F:996`). A v3 file (121 ids) is
      about 100 KB, so that cannot happen in practice. ✅
  - **Thresholds exactly as briefed.** tau <= 0.70 passes; the HOLD oriented mean must be > 0; the greedy step
    uses `|rho| <= 0.70` by FIT Sharpe; FIT needs >= 250 live days. All match (`F:97-99`, `F:706-727`). ✅
  - **The cache never serves a stale result.**
    - f_k depends only on the signal bytes, the role price payload and the code.
    - The signal bytes are covered by the cache payload SHA.
    - The role price payload is covered by the role manifest SHA, which pins the payload receipts, and by the
      context digest.
    - The code is covered by the semantics tag, a manually bumped version string as root specified (`F:87-91`).
      See M1.
    - I checked for a stale-serving path and found none. A tampered record fails its self-hash
      (`T:744-751`). A corrupt context is rebuilt, and records bound to another digest are recomputed.
    - The cheap all-cached path (`F:810-812`) reads no payload. That is sound, because each record was computed
      from verified bytes with that SHA. ✅
  - **Factor-return timing.** `forward = returns[begin+2 : end+2]` is unchanged from T9 (`F:495`), with f(d) using
    r(d+1 → d+2), which the T9 review verified against `strategy_nav_replay.hpp`. ✅
  - **Runner acceptance (named-risk check).** The landed parser `strategy_ic_runner.cpp:638-669`:
    - requires top-level `train_manifest_sha256 == --train-sha256`;
    - `composition_signs` (`:610-633`) allows known ids only, with integer ±1 values, and every weighted id must
      have a sign;
    - allows unknown keys.
    The fitter's file satisfies all of these. It emits no sign-0 entries, and it emits signs for zero-weight
    rejected candidates too, which the runner accepts. ✅
- ⚠️ **Cannot verify from the diff alone:**
  1. **Real C++ acceptance of a real output.** Root: run `--plan-only --composition-weights <out>/composition_weights.json`
     once on the v1 smoke output. It is cheap, because weights are validated before any payload load.
  2. **Real-data timing for 121 candidates.** The v3 library has 121 candidates, not 120-130. At the T9 real rate
     (about 27-30 s context plus about 1.47 s per candidate) that is about 210 s in total. The two-run plan
     (`--max-seconds 140` under a 180 s cap) fits.
  3. **Status of I1.** I1 depends on how root intends to run the v3 TRAIN IC pass that fills the cache. The
     current dev preset gives the legacy VM identity, so base entries stay where they are and only field entries
     move. A build with `/arch` FMA/AVX2 would move every entry.

### Strengths

- **One data path makes byte identity structural, not incidental.**
  - Every run (fresh, resumed, fully cached) builds the factor matrix from records (`F:867-869`).
  - Records store exact repr floats, so the JSON round-trip is lossless.
  - The test proves identity across seven paths: budget stop, time stop, resume, all-cached, tampered record,
    corrupt context with a bit-identical rebuild, and price payload absent (`T:721-771`).
- **Records are unsigned, which makes the sign ruling orthogonal to the cache.**
  - Negating the book is exactly antisymmetric through both projections, so `s_k f_unsigned` is bitwise the
    signed series.
  - The T9 reference still matches to 1e-9 under `--screen none`.
- **The screen is compact and literal to the brief.**
  - `ScreenRules` (`T:601-644`) exercises every rule on hand-built rows:
    - precedence with two failures;
    - an admitted candidate with s_k = -1;
    - the < 250 common-day note;
    - admission order by FIT Sharpe;
    - FIT Sharpe and HOLD mean against closed-form values to 1e-12/1e-15.
- **The publication hardening fixes T9 M3's leaked `.pending`.** The directory publish is all-or-nothing, and
  its pending dir is removed on any exception (`F:785-791`). `admission_sha256` binds the weights to the
  decision table.
- **The budget design is simple and safe.**
  - The stop is checked before a candidate starts, using the slowest candidate so far (`F:825`).
  - Every finished candidate is persisted before the next one starts (`F:831-833`).
  - `--max-new-candidates` gives a deterministic test hook.
  - Budget flags without `--work-dir` are refused (`F:850-851`).
- **T9 minor M2 is resolved.** The top-level `train_manifest_sha256` now exists, and the landed runner reads
  exactly that key.

### Issues

#### Critical (Must Fix)

None.

#### Important (Should Fix)

- **I1. The fitter cannot locate v3 cache entries under the runner's landed cache layout, so the report's v3 run
  refuses at startup.** This is a cross-task integration issue. It comes from `a9ef7175` and `060f440c`, which
  landed after `cd2ec619`. T9 review ⚠️2(b) predicted it.
  - The fitter reads only `candidate_cache/<train-sha>/<id>.json` (`F:860`, `F:248-252`).
  - The landed runner writes entries elsewhere:
    - under `DIR/<vm_identity>/` unless the identity is the legacy `dslvm1_clang18.1` (`strategy_ic_runner.cpp:685-688`, `:94`);
    - for any candidate that reads a non-base field (base = `close`, `raw_close`, `volume`, `:52`), under the
      sibling directory `ROOT/<fields-manifest-sha256>/` (`:689-694`, `:886-888`).
  - Library v3 declares `mkt_ret`, `si_*`, `iv_atm_*`, `earn_recent` and `shares_out`. By my token scan, **51 of
    its 121 candidates** read one of them.
  - `shas = [candidate_payload_sha(...) for c in library]` (`F:863`) therefore raises "candidate cache: missing
    entry" on the first field candidate. The failure is loud, cheap and never stale, but the T11 deliverable (the
    v3 screen) cannot run.
  - Fix, in the fitter:
    - (a) Resolve ROOT exactly as `cache_root` does. Take the VM identity as a pinned input, or read it from the
      TRAIN IC run's recipe or plan.
    - (b) For field candidates, look in `ROOT/<fields-manifest-sha256>/`. The fields SHA is a pinned CLI input
      or comes from the IC run's recipe. The field-ness comes from the library's DSL field references, or
      simply: try the base dir, then the fields dir, and require the sidecar's `fields_manifest_sha256` to match.
    - (c) Mirror `cached_payload_sha` (`:740-760`): require `vm_identity` to match (or the legacy rule) and
      `fields_manifest_sha256` present iff it is a field entry. A foreign-identity entry is then refused rather
      than scored.
  - The work-dir key is the payload SHA, so no record invalidation is needed. Add a fixture with one field-dir
    candidate and one VM-identity root.

#### Minor (Nice to Have)

- **M1. The code component of the cache key is a manually bumped string** (`F:87-91`).
  - Root asked for a "fitter semantics version", so this conforms. The failure mode is silent, though.
  - Suppose a later fix round edits the ranks, exposures, projection or `standalone_turnover` without bumping
    `CONTEXT_SEMANTICS`/`FACTOR_SEMANTICS`. Then the all-cached path (`F:810-812`) reuses every old record, while
    the outputs' `script_sha256` names the new script.
  - A tau change would move `reject_turnover` decisions.
  - Cheap hardening: fold a hash of the source of the math-bearing functions and constants into `SEMANTICS_TAG`,
    using `inspect.getsource` on `PricePanel`, `neutralization_basis`, `centered_tied_ranks`, `Context.build`,
    `Context.book`, `factor_record` and `standalone_turnover`, plus the numpy version. Or at least store
    `script_sha256` in each record and list the distinct producer SHAs in provenance.
  - Recommended before the real v3 run, since this file is about to get a fix round (I1).
- **M2. `Context.save`'s docstring says "Atomic replace", but the code is `rmtree` then `rename`**
  (`F:504-516`).
  - A kill between the two leaves no context. That costs one about 30 s rebuild and is never stale.
  - A concurrent second fitter on the same work dir would hit an uncaught `OSError` at the rename.
  - Fix the docstring, or rename the old directory aside before swapping.
- **M3. Late refusals after compute.**
  - A leftover `.<out>.pending` refuses only inside `publish_directory` (`F:779-782`). If the bounded runner
    hard-kills during that window, every rerun computes and then refuses. Add the pending check next to the early
    `out.exists()` check.
  - `os.rename` refuses an existing target only on Windows (`F:787`). On POSIX it replaces an existing empty
    directory, though the early `exists()` check makes that race-only.
- **M4. The test's Python port of the runner parser is stale** (`T:288`).
  - `runner_accepts` still mirrors the T9-era parser. It lacks the landed `train_manifest_sha256` binding and the
    `composition_signs` rules (`strategy_ic_runner.cpp:610-668`).
  - `Admission` asserts those keys directly (`T:697-700`), so behavior is covered. The port's name overstates
    it, though: update it to the landed rules.
- **M5. Boundary and invalidation gaps.** Each needs one assertion.
  - tau == 0.70 is admitted-eligible; |rho| == 0.70 is not redundant.
  - Exactly 250 live FIT days is not insufficient; exactly 250 common days is correlated.
  - HOLD with zero live days is unstable.
  - A changed `SEMANTICS_TAG` misses the cache.
  - Records valid under digest A with a stored context B are recomputed.
  - The T9 M6 items are carried over unchanged: sidecar and orientations identity mismatches, and a stale
    `.pending`.
- **M6. Column semantics a reader could conflate.**
  - `max_abs_rho`/`max_abs_rho_with` are measured against the final admitted set (`F:734-743`). They can
    therefore differ from `redundant_with`, which is the argmax at rejection time.
  - `low_overlap_with` also lists pairs with undefined rho that do have enough overlap (zero variance).
  - Both behaviors are documented in `rules`, but a `redundant_rho` column and a distinct note reason would make
    the ledgered table unambiguous.
- **M7. Structure.**
  - The tool now spans four responsibilities in about 1040 lines: the price-risk port, the incremental store, the
    screen, and fit/output.
  - Consider moving `WorkStore`/`Context` persistence and `screen_v3` into sibling modules.
  - T9 minors M4 (`SHRINK_LAMBDA` unused) and M5 (`m` meaning two things) remain open.

### Assessment

**Task quality:** Needs fixes

**Reasoning:** The screen, the sign ruling, the TRAIN-only sealing and the incremental cache are correct. The cache
is sound: it never serves stale records, and its outputs are byte-identical to a fresh run, as proven across seven
paths. The admission rules match the brief exactly, and the MV fit runs over admitted candidates only. One fix
blocks the real run: the fitter must learn the runner's landed VM-identity and fields-directory cache layout (I1).
Without it, 51 of the 121 v3 candidates cannot be located and the v3 screen refuses at startup.
