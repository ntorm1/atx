# Lane COV-MV: re-review 1 (fix round 1, COVMV-S; RESUME 10)

- **Verdict: APPROVE** (0 Required, 4 Suggested). S-1, S-2 and S-6 are done as COVMV-S / COVMV-S6a order. No
  hunk touches arithmetic (COVMV-BYTES). The S-2 teeth (`p9-covmv-j`, my one build) caught the mutation:
  exactly `MvAimRule.PrintFailureLeavesTheBookAndWritesNull` failed and the other 15 passed.
- Lane `COV-MV`, pool-31, branch `feat/p9-covmv2-20261007`. HEAD `942f499b` (records). The round's only code
  commit is `cc63c38f` (`WIP(cov-mv-fix1)`, COVMV-WIP); then `12f5c094` (records), `53013342` (merge of root
  `b182dfdd`), `19e967d0` and `cfa991a7` (sprint-dir tools), `39576746` (merge of root `08f04e07`), `1df31bcc`
  (merge of root `094bcd3b`), `942f499b` (records).
- BASE `e9e794b3` = the first review's APPROVE commit (code `846739c1`), per REV-FRESH / REV-BASE / FU-REVIEWS.
  Reviewed: `git diff e9e794b3 942f499b` only (scratch copy, 56,106 lines; 364 files).
- Fresh reviewer (REV-FRESH). Contract: ROOT-SDD `resume4-dispatch.md`, `resume6-addendum.md`,
  `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md` (TEST-1), `resume10-addendum.md`,
  `r7-review-dispatch.md`, `r8-review-notes.md` "COV-MV", `r10-dispatch.md` "Scoped re-reviews" (COV-MV bullet),
  `briefs/review-template.md`; in pool-31: `task-COV-MV-review.md` (S-1 .. S-7), `task-COV-MV-report.md` "Fix
  round 1 (COVMV-S)", `handoff-COV-MV.md` (fix-round sections), `covmv-logs/r10-*`, `covmv-logs/i-*`,
  `covmv-logs/tools/{teeth-s2.py,wait-build.ps1,run-gtests.py}`, `build-equity/mega-p9-covmv-i-receipt.json`;
  `.agents/cpp/agent.md` sections 3, 4, 6 and 10.
- Rulings grepped in ROOT-SDD `progress.md`: COVMV-S (:723), COVMV-WIP (:770), COVMV-S6a (:771), COVMV-BYTES
  (:793), the re-review dispatch (:794), COV-13 (:587), COV-14 (:621), COV-4 (:196), HOST-4 (:774), HOST-5
  (:788), TEST-1 (:688), TEETH-RESTORE (:674), EXE-CUR (:567), PERM-1 (:595), REV-FRESH (:775), FU-REVIEWS
  (:780).
- Pool-24 (frozen) was not read for evidence and nothing was written there. Nothing was built, run or committed
  in `C:/atx`.

## 0. What the range holds

| part | files | how checked |
|---|---|---|
| root content via the three merges | 112 of the 122 non-record files; 226 of the 242 sprint-dir files | each file's blob at `942f499b` equals its blob at root `094bcd3b` (loop over `git diff --name-only`, `git rev-parse <sha>:<path>`) |
| the lane's code (S-1 .. S-6) | 9 files | `git diff e9e794b3 cc63c38f` is byte-equal to `git diff e9e794b3 942f499b -- <the 9 files>`; `git diff cc63c38f 942f499b -- <the 9 files>` is empty; root did not touch them (`git diff <merge-base> 094bcd3b -- <the 9 files>` is empty) |
| the lane's merge resolution | `atx-impl/CMakeLists.txt` | below |
| the lane's records and tools | 16 sprint-dir files (`handoff-COV-MV.md`, `task-COV-MV-report.md`, `covmv-logs/i-*`, `r10-*`, `tools/*`) | read |

**The three root merges.**

- `53013342` (root `b182dfdd`): tree `35aa47e4` = `git merge-tree --write-tree ^1 ^2`. Plain.
- `39576746` (root `08f04e07`): tree `26a1ce2f` = the recompute. Plain.
- `1df31bcc` (root `094bcd3b`): the recompute `899f6b25` reports ONE conflict, `atx-impl/CMakeLists.txt`, and
  the commit's tree differs from it in that file only (`git diff --stat 899f6b25 1df31bcc`: 1 file). The
  resolution keeps both tail appends to `atx-impl-strategy`, root's ABS row first, then the lane's three lines;
  `git diff 1df31bcc^2 1df31bcc -- atx-impl/CMakeLists.txt` is exactly the lane's three lines and
  `git diff 1df31bcc^1 1df31bcc -- atx-impl/CMakeLists.txt` is exactly ABS's three lines, each side's bytes
  unchanged. This is the one keep-both the dispatch names; the merge is otherwise plain.

## 1. Spec verdict, item by item (COVMV-S, COVMV-S6a, COVMV-WIP, COVMV-BYTES)

| item | verdict | evidence |
|---|---|---|
| S-1 (REQUIRED, allocation) | done | (a)-(d) below |
| S-2 (REQUIRED, failure handling) | done | (e)-(h) below; my teeth: section 4 |
| S-6 (REQUIRED, check timing; COVMV-S6a) | done | (i) below |
| S-3 (structure) | done | (j) |
| S-4 (structure) | done | (k) |
| S-5 (registry text) | done | (l) |
| S-7 (record) | done | (m) |
| COVMV-WIP (one WIP commit read by S item) | done | the 30 hunks of `cc63c38f` are classified by S item in section 2 |
| COVMV-BYTES (a) S-1 bit-identity test + unchanged `MvAimRule.*` expectations | done | `MvTarget.FactorEigenOnKeptStorageIsAFreshSolve`; the only edited expectation of an existing test is S-5's list (`strategy_mv_aim_rule_test.cpp:250`) |
| COVMV-BYTES (b) every hunk classified; none touches arithmetic | done | section 2: no hunk touches an arithmetic expression of the kernel, the tilt, the gross rescale, the dollar-neutral step or a recorded figure |
| COVMV-BYTES (c) the gate's flag-absent identity | pending by rule | deferred to the gate (TEST-1) |
| the S-2 teeth (the re-reviewer's one build) | done: CAUGHT | section 4 |

**S-1 (allocation).**

- (a) The workspace is kept across decisions. `FactorEigen` (`mv_target.hpp:95-108`) gains
  `std::unique_ptr<Workspace>`; `Workspace` (`mv_target.cpp:278-281`) holds F's forecast block (`MatX`) and an
  `Eigen::SelfAdjointEigenSolver<MatX>`. The first non-empty call makes it (`:305`). `w.block.resize` and
  `solver.compute` reallocate only when the block's size changes. `reserve_exact` reserves the three output
  vectors once, at k and k x k (`:298-300`); later `push_back` / `resize` stay within that capacity. The
  `FactorEigen` is the Rule's member `eigen_` (`strategy_mv_aim.hpp:154`), so it lives as long as the Rule. The
  header stays free of Eigen (PIMPL, house style section 6). Moves are `noexcept`, copies deleted, and the
  special members are defined in the .cpp where `Workspace` is complete.
- (b) Same bits as before. The pre-round code called `symmetric_eig(block)` (`decompose.hpp:127-136`). The new
  code runs its guard, `detail::is_symmetric_within(block, detail::kSymmetryTol)`, then `compute(block)` with the
  default options, which is what the `es(A)` constructor runs. The messages and codes are the pre-round ones
  (`"mv-aim-v1: factor_eigen: symmetric_eig: A must be square and symmetric"`, InvalidArgument;
  `"... eigensolver did not converge"`, Internal). Eigen 3.4.0's `compute` starts with
  `mat = matrix.template triangularView<Lower>()` into `m_eivec` (`SelfAdjointEigenSolver.h:449-452`, read in
  `C:/atx-cache/deps/eigen3-src`), so nothing from an earlier call reaches the result. The test below proves the
  bits.
- (c) The residual allocation is stated and real. `mv_target.hpp:89-94` says Eigen's `compute` still makes one
  block-sized vector per call. I checked it in the Eigen source: `tridiagonalization_inplace_selector::run`
  assigns the `HouseholderSequence` to `mat`, and `HouseholderSequence::evalTo(dst)` builds
  `workspace(rows())` (`HouseholderSequence.h:279-284`). That is one K-vector on the heap per decision. The
  header also says that a change of the block's size between decisions resizes the block and the solver (Eigen
  keeps no spare capacity).
- (d) The reserve value is unchanged. The expression `4U * k * k * f + 4U * k * f + 2U * k * sizeof(usize) + k * f`
  (`strategy_mv_aim.cpp:93`) is byte-equal to BASE; hunk 13 changes the comment only. `book/nav_lockstep.cpp` is
  not in the diff. So `nav_workspace_reserve_bytes` is unchanged with and without the flag, and
  `MvAimRule.WorkspaceReserveTermOnlyWithTheRule` is unchanged and green. The bound still covers the storage:
  kept f64 storage is now three K x K (block, `m_eivec`, `out.vectors`) plus about five K-vectors, which is
  <= 4K^2 + 4K for every K >= 1. Before the round the peak held four K x K.
- Test: `MvTarget.FactorEigenOnKeptStorageIsAFreshSolve` (`book_mv_target_test.cpp:442-509`). One `FactorEigen`
  runs seven 62-factor cases in a row: the same block again, a smaller one (31), a larger one, a refusal (an
  asymmetric block, InvalidArgument naming "symmetric", `factors` / `values` left empty), one factor, 40
  factors, then the full block. Every value and vector is compared bit for bit (`std::bit_cast<u64>`) with a
  fresh `linalg::symmetric_eig` of the same block. It tests the contract ("a fresh solve"), not pinned numbers.
  The lane's log: 12 / 12 (`i-mvtarget-debug.log`).

**S-2 (failure handling).**

- (e) The decision proceeds. `apply`'s tail (`strategy_mv_aim.cpp:128-131`) reads
  `if (auto printed = add_eigen_shares(); !printed) note_unavailable(printed.error()); return co::Ok();`. The
  priced block of `desired` has already been written by `tilt` (`:217`). `record` (the specific-vol ratio and the
  special-tier negatives) runs before the print, so the book and the other figures do not depend on it.
- (f) The failure is counted. `add_eigen_shares` (`:267-280`) gives both shares or neither: `mv` is held in a
  local until the second `factor_exposure` succeeds. `note_unavailable` (`:282-285`) counts the failure and keeps
  the first error's text.
- (g) The figure is written as null. `summary_json` (`:315-329`): with no failure, the `eigen_00_02_share` block
  is written at the same point with the same keys and text as at BASE. On any failure, `"eigen_00_02_share":
  null` is written, plus `"eigen_00_02_share_unavailable": {"decisions", "first_error"}`, a key that exists only
  on failure.
- (h) Flag absent: the Rule exists only with `--target-rule mv-aim-v1`. Without the flag no `mv_aim` block is
  written, and S-2 touches nothing outside the Rule. `FlagAbsentIdentity` and `SummaryBlockOnlyWhenOn` are
  unchanged and green. With the flag and no failing print, the bytes are BASE's by (g).
- Test: `MvAimRule.PrintFailureLeavesTheBookAndWritesNull` (`strategy_mv_aim_rule_test.cpp:669-736`; helper
  `plant_asymmetric_f` `:204-217`).
  - The plant moves F_{0,8} by 1e-4 on every row. Factor 8 is industry slot 7, which no fixture name has, so
    the kernel's inputs stay bit-equal. The test does not assume this: it checks every in-process target at
    d <= 30 bit for bit against the clean store's.
  - It also checks: the applied counts are equal; `share_unavailable == applied`; `share_mv.n ==
    share_desired.n == 0`; the error names "symmetric"; the clean store's block is an object.
  - Command line: both runs exit 0 and publish the same file list. Every file except `recipe.json` /
    `summary.json` is byte-equal. Those two are equal once the store pin, `recipe_sha256` and the two eigen keys
    are removed, and the planted run's summary carries null and `{decisions = applied, first_error}`.
  - This is the right contract: the book is unchanged and the figure is null. The lane's log: 16 / 16
    (`i-mvaimrule-debug.log`).

**S-6 (check timing; COVMV-S6a).**

- (i) `check_input` (`strategy_mv_aim.cpp:145-150`) calls `store_->check_axes` only while `axes_checked_` is
  false. The flag is false at construction and reset by `begin_run` (`:100`). It is set only after the check
  passes, so a refusal still stops the run at the run's first decision.
- The per-decision `assert(store_->check_axes(...) && "...")` is Debug only. `equity-rel` is
  `CMAKE_BUILD_TYPE` Release, whose CMake default flags carry `/DNDEBUG`; the one preset that overrides
  `CMAKE_CXX_FLAGS_RELEASE`, `equity-asan`, keeps `/DNDEBUG`. So a Release cell never re-checks per decision.
- "At store open" was not possible, because `RiskStore::open` runs before the role is loaded. COVMV-S6a accepts
  "once per run".
- Each `ScopedNavExtension` makes its own Rule (`strategy_nav_v7.cpp:64`), and `begin_run` is called once per
  run, so a calibrate pass reads the same input it checked.

**S-3, S-4, S-5, S-7.**

- (j) S-3. `strategy_nav_v7.hpp:125-127` forward-declares `mv_aim::Rule` in place of the include, and
  `strategy_nav_v7.cpp:25` includes it. Grep at HEAD: every user of `mv_aim::` (`nav_rules.cpp`,
  `strategy_nav_v7.cpp`, the rule and its test) includes `strategy_mv_aim.hpp` itself, and the header uses the
  type only as `const mv_aim::Rule*`. `atx-impl/pch.hpp` holds only Eigen, nlohmann/json and std headers, so the
  PCH masks no project include. The library compiled whole in `p9-covmv-i` (lane) and `p9-covmv-j` (mine).
- (k) S-4. `apply` is 27 lines (`:106-132`) and calls `check_input`, `begin_decision`, `priced_view`, `tilt`,
  `pass_through`, `count_applied`, `record`, `add_eigen_shares` and `note_unavailable`. I compared it with
  BASE's single function (`e9e794b3:atx-impl/src/strategy_mv_aim.cpp:101-195`) statement by statement:
  - the side effects run in the same order: store check, geometry, axes, `fresh` / `formed_` / `last_`,
    `locate_in_aim`, store read, S11, the reserves, the view, the kernel, the rescale, the counts, the record;
  - the S11 refusal still precedes every reserve;
  - the flat-tilt Unavailable still leaves `desired` untouched (`ATX_TRY_VOID` returns before the rescale);
  - `counts_.priced.add(index_.size())` gets BASE's `priced` (it was `const usize priced = index_.size()`, and
    nothing between touches `index_`).
  The two moves in `record` / `add_eigen_shares` are discussed in section 2 (hunk 18).
- (l) S-5. `nav_rules.cpp:106-110` names `two-speed-v1` (per-name-v1's row at `:53` names it the same way). The
  only reader of a NAV row's `incompatible[]` is `row_json` (`nav_rules.cpp:125-134`, print only). No production
  Python reads it (grep over `scripts/` and `atx-impl/tools/`, tests excluded). The registration and template
  are not in the diff. The `--list-rules` row text of mv-aim-v1 now reads `["aim-partial-v6", "spo-v1",
  "spo-v2", "spo-v3", "two-speed-v1", "adv-hold-v1", "hold-band-v1"]`. `ListRulesRowIsAClosedTargetRow`
  asserts it.
- (m) S-7. `covmv-logs/tools/run-gtests.py` hashes the exe before and after each run, compares both with the
  receipt, and skips the run on a mismatch. Each `i-*-debug.log` shows `equal=True` twice. `i-receipt.json` is
  byte-equal (`cmp`) to `build-equity/mega-p9-covmv-i-receipt.json`.

## 2. Hunk classification (COVMV-BYTES (b))

Hunk numbers are those of `git diff e9e794b3 cc63c38f` (default context; 30 hunks). That diff is byte-equal to
the range's diff over the same 9 files.

| # | file (BASE line) | change | class |
|---|---|---|---|
| 1 | `mv_target.hpp` -27 | header note "FactorEigen keeps its storage across calls"; `#include <memory>` | text / allocation |
| 2 | `mv_target.hpp` -83 | `FactorEigen`: special members, `Workspace` PIMPL; contract text (the residual) | allocation |
| 3 | `mv_target.cpp` -6 | `#include <memory>` | allocation |
| 4 | `mv_target.cpp` -36 | `reserve_exact` factored out of `sized`; `sized` = `reserve_exact` + the same `assign` (same capacity rule, same fill) | allocation |
| 5 | `mv_target.cpp` -269 | `FactorEigen::Workspace`; defaulted special members | allocation |
| 6 | `mv_target.cpp` -278 | `factor_eigen` body: reserves, the kept block / solver in place of `symmetric_eig`, its guard and messages inline; the copy-out loop unchanged in form | allocation (the print's storage; not the kernel) |
| 7, 8, 9 | `book_mv_target_test.cpp` | suite comment; `#include decompose.hpp`; the new S-1 test | test |
| 10 | `nav_rules.cpp` -103 | `two-speed-nav-v1` -> `two-speed-v1` + a comment | text (registry row) |
| 11 | `strategy_mv_aim.cpp` -5 | `#include <cassert>` | check timing |
| 12 | `strategy_mv_aim.cpp` -37 | `invalid` returns the Err proxy (`auto`) so a `Result<usize>` can return it; a comment | structure |
| 13 | `strategy_mv_aim.cpp` -84 | `workspace_bytes` comment only; the expression byte-equal | text |
| 14 | `strategy_mv_aim.cpp` -93 | `begin_run` resets `axes_checked_` | check timing |
| 15 | `strategy_mv_aim.cpp` -100 | `apply` as a dispatcher; `check_input` (S-6); the start of `begin_decision`; the S-2 tail | structure / check timing / failure handling |
| 16 | `strategy_mv_aim.cpp` -113 | `scored` into `Visit`; the lambda `pass_through` removed (now a member); the store read moved to `apply`; `priced_view` opens | structure |
| 17 | `strategy_mv_aim.cpp` -164 | `priced < 2` moved to `apply`; `tilt` (view, `out_.assign`, `ATX_TRY_VOID(mv_aim)`) with the three rescale lines as unchanged CONTEXT; `pass_through` / `count_applied` members; `priced.add(index_.size())` (BASE's `priced` alias, same value); `record` returns `void` | structure |
| 18 | `strategy_mv_aim.cpp` -209 | the negatives block moved from after the eigen print to before it (same reads: `no_short`, the rewritten `desired`); `add_eigen_shares` (the eigen lines; `share_mv.add(mv)` now after the second `factor_exposure`, `mv` the same expression on the same `eigen_` / `exposure_` state); `note_unavailable` | failure handling / structure |
| 19 | `strategy_mv_aim.cpp` -255 | `summary_json`: the BASE block when no print failed; null + `_unavailable` otherwise | failure handling |
| 20, 21 | `strategy_mv_aim.hpp` -19, -35 | contract text (S-6, S-2, S-1) | text |
| 22 | `strategy_mv_aim.hpp` -77 | `Counts::share_unavailable`, `share_error` | failure handling |
| 23 | `strategy_mv_aim.hpp` -100 | the private declarations (`Visit`, the helpers), `axes_checked_` | structure / check timing |
| 24 | `strategy_mv_aim.hpp` -114 | a comment on `eigen_` | text |
| 25 | `strategy_nav_v7.cpp` -22 | `#include "strategy_mv_aim.hpp"` | structure (S-3) |
| 26 | `strategy_nav_v7.hpp` -114 | the include replaced by a forward declaration | structure (S-3) |
| 27-30 | `strategy_mv_aim_rule_test.cpp` | suite comment, `<cstring>`, `plant_asymmetric_f`, S-5's expected list, the new S-2 test | test |
| M | `atx-impl/CMakeLists.txt` (merge `1df31bcc`) | keep-both of two tail `target_sources` appends | structure (build) |
| R | 112 + 226 files | root's bytes through the three merges | root (plain merges; not the lane's) |
| D | 16 sprint-dir files | records, logs, tools | text |

Arithmetic check:

- `mv_target.cpp`'s solve and tilt (BASE `:98-250`) are outside every hunk; hunk 4's `sized` is the scratch
  allocator only.
- The gross rescale (`gross += |s|`, `desired[index_[k]] = gross * out_[k]`), the record's
  `weights_ = gross * out_`, the ratio sums, and the eigen-share and negative-count expressions are unchanged in
  text.
- The only moved evaluations are `share_mv.add` (same argument value) and the negatives block (same inputs).
- No hunk touches an arithmetic expression of the kernel, the tilt, the gross rescale, the dollar-neutral step or
  a recorded figure. No Required finding under COVMV-BYTES.

## 3. Quality verdict: good

- **Modular / reusable.**
  - `apply` is now a readable dispatcher over single-purpose helpers (house style section 3).
  - `FactorEigen` owns its storage behind a PIMPL, so the engine header stays Eigen-free.
  - The v7 header no longer drags the engine kernel into its includers (S-3).
  - One reuse gap: `factor_eigen` copies `symmetric_eig`'s guard and messages and reaches into
    `atx::core::linalg::detail`, instead of a workspace-taking eig in atx-core (Suggested S-a).
- **Performant.**
  - Per applied decision the print now allocates one K-vector inside Eigen (stated), plus a block and solver
    resize only when the forecast block's size changes (stated). Before the round it allocated about four
    K x K matrices.
  - The axes check is O(dates) once per run in Release.
  - No allocation was added to the kernel path.
- **Lock-free hot path.** No mutex, atomic, `thread_local` or function-static is added; the rule runs on the
  calling thread (one Rule per `ScopedNavExtension`).
- **Errors.**
  - The print's failure is counted and surfaced, never silent.
  - The decision's own refusals (store, geometry, axes, S11, kernel) still stop the run.
  - The new helpers that cannot fail are `noexcept` (`Stats::add` is `noexcept`).
  - No `[[nodiscard]]` result is dropped: `add_eigen_shares`' Status is consumed by the `if`.
- **`/W4 /WX`.** 0 warning lines in `mega-p9-covmv-i-build.log` (UTF-16, decoded). My `mega-p9-covmv-j-build.log`
  (351 TUs, so the round's TUs were compiled anew) has 0 first-party warnings. Its 11 warning lines are clang-cl's
  `/MP` driver note (7) and databento-cpp's two `getenv` deprecations with their two "1 warning generated"
  lines.
- **100 columns.** No added line over 100 (awk over every `+` line of the 9 files), and no line over 100 in any
  of the 9 files at HEAD.
- **Tests.** Both new tests check contracts (a fresh solve's bits; the clean store's book and bytes), not the
  implementation's numbers. The S-2 test has teeth (section 4).

## 4. Teeth (my one build, HOST-4 / HOST-5, TEETH-RESTORE)

**The tool.** I read the lane's `covmv-logs/tools/teeth-s2.py` first and ran it unmodified
(`PYTHONDONTWRITEBYTECODE=1 python teeth-s2.py 180`).

- It refuses to start unless `git status --porcelain` is empty, `git hash-object` equals HEAD's blob, and the
  mutation line occurs exactly once.
- It keeps the pre-mutation bytes itself, in memory and in a backup copy.
- It waits under HOST-5's test (only a build tool that names `C:\atx-wt\pool-*`, or a `research-build`
  process, counts; a foreign build is ignored), then a 90 s grace window. A memory-admission refusal is restored
  and retried.
- It mutates `atx-impl/src/strategy_mv_aim.cpp:130` from `... !printed) note_unavailable(printed.error());` to
  `... !printed) return printed;`, the pre-round behaviour: a failed print stops the decision.
- It runs `scripts/research-build.ps1 -Tag p9-covmv-j -Targets atx-impl-strategy-target-tests -Preset
  equity-dev` once.
- It restores the bytes in a `finally` block before anything else, then asserts porcelain empty,
  `hash-object` = HEAD's blob and raw SHA-256 = the saved one (exit 2 if not).
- It runs no test itself. The run came after the restore, with `run-gtests.py p9-covmv-j j mvaimrule`, which
  hashes the exe against the receipt before and after.

**Before the build.** The lane had deleted its tree, so I first ran
`powershell -NoProfile -File scripts/atx-build.ps1 configure -Preset equity-dev` in pool-31: exit 0,
`engine_git_sha 942f499b` (`covmv-logs/j-configure-dev.log`).

**Host.** At my check, root's `p9-2x` (pool-2) was building, so the tool waited.

- 21:26:38: no pool build running, free 4,893 MiB; grace 90 s.
- 21:28:10: the grace held; launch at free 2,819 MiB.

**`covmv-logs/j-teeth.log`.**

- start: HEAD `942f499b`, porcelain empty, `hash-object` `22dd0eea` = `HEAD:atx-impl/src/strategy_mv_aim.cpp`,
  sha256 `6d0b9ca2...94d6`.
- 21:28:10: mutated, sha256 `c832de4e...ccfb`.
- 21:34:29: restored. Porcelain empty, `hash-object` `22dd0eea` = HEAD, sha256 `6d0b9ca2...94d6`,
  `equal_saved=True ok=True`; build exit 0.
- Checked again by hand after the tool: porcelain empty; `git hash-object` = `git rev-parse HEAD:<path>` =
  `22dd0eea`; `sha256sum` `6d0b9ca2...94d6`. The tool's backup copy was byte-equal, then deleted by its literal
  path.
- No `git checkout` / `restore` / `stash` was used.

**Receipt `build-equity/mega-p9-covmv-j-receipt.json`.** Source `942f499b`, DirtyEntries 1 (the mutation),
exit 0, 351 TUs, 13 links, 373 s, Jobs 3. Exe `atx-impl-strategy-target-tests` `2e544681...a089`; it was hashed
before and after the run, both equal to the receipt. 0 first-party warnings (section 3).

**Result: CAUGHT.** `atx-impl-strategy-target-tests.exe --gtest_filter=MvAimRule.*` exited 1: 16 ran, 15
passed, and exactly `MvAimRule.PrintFailureLeavesTheBookAndWritesNull` FAILED, with 35 failed assertions
(`covmv-logs/j-mvaimrule-debug.log`):

- `strategy_mv_aim_rule_test.cpp:110` x 31: the in-process `targets` helper. At every decision 0..30, `apply`
  returned "InvalidArgument: mv-aim-v1: factor_eigen: symmetric_eig: A must be square and symmetric", so the
  print stopped the decision.
- `:689`: `share_unavailable` 0 instead of `applied`.
- `:692`: no `share_error`.
- `:695`: `eigen_00_02_share` not null.
- `:704`: the planted command-line run exited 1 instead of publishing.

The other 15 passed on this build of HEAD `942f499b` itself, the one line aside. They include `FlagAbsentIdentity`,
`SummaryBlockOnlyWhenOn`, `BookWorkersParity`, `WorkspaceReserveTermOnlyWithTheRule`,
`CalibrateGrossCountsEachDecisionOnce` and the four refusal tests. So the mutation is isolated, and the S-2 test is
the one guard on the failure path.

**After.** The object tree of `build-equity` was deleted by name: `CMakeFiles/`, `atx-core/`, `atx-engine/`,
`atx-impl/`, `atx-tsdb/`, `bin/`, `lib/`, `tests/`, the ninja / CMake / CPack files, `compile_commands.json` and
`spdlog.pc`. That freed about 2.6 GB (C: 47 -> 50 GB free). Kept: the tracked `audits/` and
`v8-interim3-pitch-render-run2/`, the `mega-p9-covmv-{f,h,i,j}-*` receipts and logs, and
`vcpkg-manifest-install.log`. The `covmv-logs/j-*` logs are git-ignored and NOT committed (the dispatch commits
only this file); their key lines are quoted above.

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `atx-engine/src/book/mv_target.cpp:312-323` | Suggested (S-a) | `factor_eigen` re-implements `atx::core::linalg::symmetric_eig` "step for step": it copies the guard through `la::detail::is_symmetric_within` / `kSymmetryTol` (a `detail` reach-in from atx-engine) and copies both messages. If `decompose.hpp` changes its guard or text, `factor_eigen` silently diverges. The S-1 test would catch a solver change, but only one skew case of the guard. | P10: a workspace-taking eig in atx-core (for example `symmetric_eig(const MatX&, EigWorkspace&)`, with `symmetric_eig(A)` calling it), used by `factor_eigen`; no `detail::` use outside atx-core. The fix round's file scope did not allow atx-core. |
| `scripts/specs/p9/rules/mv-aim-v1.json` (`printed_only.eigen_00_02_share`) | Suggested (S-b) | The registration describes the figure but not S-2's form: null on any print failure, plus `eigen_00_02_share_unavailable {decisions, first_error}`. The first review asked that the registration say so. The lane rightly did not edit a file whose SHA-256 the prereg cites. | PRE (round 2b-ii), or root when it re-pins the registration (PRE-29): add one clause, and read null as "not read", never 0. |
| `atx-impl/src/strategy_mv_aim.cpp:145-150` | Suggested (S-c) | S-6 moved a refusal's timing, but no `MvAimRule` test drives a store whose session axis is not the replay's into `apply`, before or after the round. A later edit that skips or never resets `axes_checked_` would go unseen. | One test: a mismatched axis is refused at the first decision; after `begin_run()` it is refused again. |
| `atx-impl/src/strategy_mv_aim.cpp:89-93` | Suggested (S-d) | The comment lists five K-vectors (the solver's eigenvalues, subdiagonal and Householder coefficients, the per-call vector, the result's values) against a term of `4U * k * f`. The bound holds only because the "fourth K x K of headroom" covers the fifth (K^2 >= K). | Wording: say that the headroom also covers the fifth K-vector. The value stays (S-1). |

No Required finding.

## 5. Evidence: my own runs vs the lane's logs

- **Mine, in pool-31.**
  - The diff, the merge recomputes, the root-blob loop, the 100-column scan, and the Eigen 3.4.0 source reads
    (section 1 (b), (c)).
  - `validate_specs.py` (`covmv-logs/validate_specs.py`, the first review's tool) with `PYTHONDONTWRITEBYTECODE=1`:
    - against the lane's `covmv-logs/d-nav-list-rules.json`: exit 0, `rule_cell_problems: []`,
      `registration_problems: []`, `rule_flags problems: []`, derived flags exactly `--risk-model`,
      `--risk-model-sha256 ea58691f...f0f3`, `--target-rule mv-aim-v1`; template SHA `83391f2c...d870`,
      registration SHA `4b4af6e2...80b4` (both as the first review);
    - against a copy in my scratch dir with S-5 applied to the mv-aim-v1 row (`two-speed-nav-v1` ->
      `two-speed-v1`): the same output, exit 0.
  - T1 guards `scripts/tests/test_no_python_mirror.py` and `test_no_versioned_scripts.py` (`-p no:cacheprovider`,
    my own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`): exit 0, 11 passed (G-P5: 14 functions in 6 rows left, root's
    count after B2 / C3b).
  - The teeth build `p9-covmv-j` and its run (section 4): `MvAimRule.*` 16 ran, 15 passed, exactly the expected
    red.
- **From the lane's logs only (EXE-CUR).**
  - Clean `MvTarget.*` 12 / 12, `MvAimRule.*` 16 / 16 and the named filter 100 / 100 (1 disabled) come from
    `covmv-logs/i-{mvtarget,mvaimrule,named}-debug.log`.
  - Build `p9-covmv-i`: Source `19e967d0`, DirtyEntries 0, exit 0, 165 TUs, 0 warning lines.
  - Each log heads with the exe's SHA-256 before and after the run, both equal to the receipt
    (`atx-impl-strategy-target-tests` `4c9894aa...0d34`, `atx-engine-book-tests` `a7b568a8...231f`).
  - The receipt copy `i-receipt.json` is byte-equal to `build-equity/mega-p9-covmv-i-receipt.json`. The exes are
    deleted.
  - Note: `19e967d0` predates the merges of D3W and ABS. Between it and HEAD, C++ of `atx-impl-strategy` /
    `atx-engine` changed (the IC exe, composition and eval code), but no file of the NAV, book, risk-store or
    mv-aim path. The PM's dispatch takes these logs as the clean evidence, with no clean rebuild.
  - My `p9-covmv-j` is a build of HEAD `942f499b` itself (one line mutated). Its 15 non-target `MvAimRule` passes
    are evidence for the merged head's rule tests.
- **Blind.** I opened no NAV / IC / Sharpe output, no 2020-2023 statistic, nothing dated 2024-01-01 or later, and
  no byte of the pinned store (the validator only prints its path string). Root history was read as one-line
  subjects.

## 6. What root must check at merge

1. **Conflicts (W3-9).** Root is now at `858a4fca` (AL-CLOCK `e9b3a323` merged). I ran
   `git merge-tree --write-tree --name-only 858a4fca 942f499b`; it gives exactly ONE conflict, at the tail of
   `atx-impl/tests/CMakeLists.txt`.
   - Keep both: AL-CLOCK's block FIRST (its blank line, its comment,
     `target_sources(atx-impl-strategy-target-tests PRIVATE strategy_nav_info_clock_test.cpp)`), then COV-MV's
     (comment, `... strategy_mv_aim_rule_test.cpp`).
   - Auto-merged: `nav_decide.cpp`, `nav_lockstep.cpp`, `nav_rules.{cpp,hpp}`, `strategy_nav_v7.cpp`.
   - The `atx-impl/CMakeLists.txt` tail merges clean, in the order the lane resolved it: ABS's
     `strategy_ic_absent.cpp` row, then COV-MV's `strategy_mv_aim.cpp` row.
   - The PM's root note ("AL-CLOCK first, then COV-MV, then ABS's row") reads correctly as two files:
     - `tests/CMakeLists.txt`: AL-CLOCK, then COV-MV;
     - `atx-impl/CMakeLists.txt`: ABS, then COV-MV.
   - If AL-CLOCK's fix-1 head is merged later, it adds no line to either tail.
2. **`nav_workspace_reserve_bytes`.** The merged body has exactly AL-CLOCK's `clock` term inside the names
   product and COV-MV's trailing `+ v7::extension_workspace_bytes(names)`; I checked this on the recompute
   `846141ca`. The fix round changed neither term's value. Re-run `MvAimRule.WorkspaceReserveTermOnlyWithTheRule`
   and AL-CLOCK's reserve test.
3. **`nav --list-rules --json`** of the merged NAV exe:
   - the mv-aim-v1 row is the last row and the only target row;
   - its `incompatible[]` is `["aim-partial-v6", "spo-v1", "spo-v2", "spo-v3", "two-speed-v1", "adv-hold-v1",
     "hold-band-v1"]` (S-5: `two-speed-v1`, not `two-speed-nav-v1`);
   - no other row changes against root's pre-merge exe, except AL-CLOCK's row, which is already in root.
4. **The two new tests** in ROOT-1a's Debug build of `atx-impl-strategy-target-tests,atx-engine-book-tests`:
   - `MvTarget.FactorEigenOnKeptStorageIsAFreshSolve` (`MvTarget.*` = 12);
   - `MvAimRule.PrintFailureLeavesTheBookAndWritesNull` (`MvAimRule.*` = 16);
   - the named filter `Cov*:NavBookRule.*:VolTarget.*:Spo*:NavSpec.*:NavRules.*:CostLawRegistry.*:TradeRateRegistry.*:TargetRegistry.*:NavAnnMeanGross.*`
     (100 at the lane's source, with C3b's `NavSpec` test);
   - AL-CLOCK's `InfoClock*`.
   This build is the first clean build of a head that holds D3W, ABS and AL-CLOCK beside this round.
5. **Flag-absent identity unchanged.** No code path without `--target-rule mv-aim-v1` changed: the reserve
   value did not move; S-3 is an include; S-5 is the lane's own row text.
   - With the flag and no failing print, `summary.json`'s `mv_aim` block is the BASE block (the
     `_unavailable` key appears only on a failure).
   - The X-5 / Y-F0 rule-absent run identity stays the gate's (TEST-1; COVMV-BYTES (c)).
6. **SQL2-CLS.** The round adds no `atx.<name>/v<n>` literal. The summary key `eigen_00_02_share_unavailable` is
   a block key, not a schema.
7. **The pins are unchanged:** store `ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3`,
   registration SHA-256 `4b4af6e2...80b4`, template `83391f2c...d870`. The registration and template are not in
   the diff. S-b (the registration's sentence on the null form) is for PRE or root's re-pin, not a lane commit.
8. **Merge the right branch:** `feat/p9-covmv2-20261007` at its APPROVE SHA (this review's commit; code
   `cc63c38f`), never pool-24's branch.
