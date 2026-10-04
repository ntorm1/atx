# Task S2R report: Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (M2c-RED, Ruling M2c-RED-1)

Lane S2R, pool-18, branch `feat/p9-s2-20261003`. Base `8314fb59` + merge of root `f6444417` (`f686080a`, no conflict;
code tree = root's code tree). 2026-10-04.

**STATUS: BLOCKED (stop under hard constraint (d)).** The red is fully attributed, but no fix can meet the contract.
Making Release produce the Debug bits from source is impossible, and every build-invariant fix moves a Debug digest.
The options below need a PM ruling. No pin, golden or expected value was edited. No kernel source changed. The only
code change is a disabled diagnostic test that counts in no suite total.

**Round 2 (Ruling M2c-RED-2, then S2R-1): DONE_WITH_CONCERNS.** The test is split, Debug and Release are green, and
the S2R-1 list is in. Release alpha: 780 passed / 2 skipped / 0 failed, with the named lit-golden skip. No pin was
edited. Sections 11-13 below; sections 1-10 are round 1 and are unchanged.

## 1. Cause (one line)

Debug (`/MDd`) and Release (`/MD`) call two different C-runtime DLLs. `std::pow` with run-time operands resolves to
`ucrtbased.dll` 10.0.22000.832 in Debug (Windows SDK copy) and to `ucrtbase.dll` 10.0.26100.7623 in Release (OS copy,
serviced by Windows Update). Their `pow` implementations differ in the last bit on about 0.2 % of arguments.

This is **not** a compiler transformation: no constant fold, `pow` rewrite, `a*b+c` contraction, vectorized libm or
reassociation is involved. It is **not** a real bug either (no uninitialized read, nothing order-dependent):
- the inputs are byte-identical between the builds (9 / 9 input-column digests equal);
- VM and oracle agree cell for cell inside each build;
- every cell of the moved rows equals the respective DLL's `pow` on the exact input bits (section 3).

## 2. Per-opcode table (from the new per-row dump, Debug p9-s2r-b vs Release p9-s2r-c)

81 of the 84 battery rows are byte-identical in all three paths. The three moved rows, identical in vm audit-exact (ae),
vm research-fast (rf) and oracle:

| row | expression | opcode (kernel) | modes | Debug digest (= pin share) | Release before | Release after | moved cells |
|---|---|---|---|---|---|---|---|
| 05 | `power(close, 0.5)` | Pow | ae, rf, oracle | `0x039b6c4543db7590` | `0x09a2ac7a73e0e54f` | not fixed | 1 (cell 407, -1 ULP) |
| 06 | `signedpower(returns, 2)` | Spow | ae, rf, oracle | `0xd0b7c7c81809e1c4` | `0x1ac423675703ed2c` | not fixed | 2 (cells 186, 201, +1 ULP magnitude) |
| 83 | `close ^ 3` | Pow | ae, rf, oracle | `0x333e60d273875211` | `0x08df520382234cfe` | not fixed | 1 (cell 296, -1 ULP) |

The battery fold: Debug `0x971c3da60ca89aa3` / `0xe9e7128fd5359900` / `0x971c3da60ca89aa3` (= pins); Release
`0xeeae40c4a6d7d93b` / `0x24c97b5bfeab6518` / `0xeeae40c4a6d7d93b` (= root's M2c prints, reproduced).

Expression per path. Each one is the plain call `std::pow(x, y)` on run-time operands, so it resolves to the
build's CRT DLL:
- Pow: VM `atx-engine/include/atx/engine/alpha/vm.hpp:259`; oracle `atx-engine/include/atx/engine/alpha/oracle.hpp:539`.
  Same call in the streaming engine `streaming_engine.hpp:717` and the parser's literal fold `parser.hpp:309`.
  `x ^ 2` and `power(x, 2)` with a literal 2 are strength-reduced to `Mul` (`dag.hpp:301`) and are not affected,
  which is why row 82 `close ^ 2` did not move.
- Spow: VM `vm.hpp:153` (`vm_spow`); oracle `oracle.hpp:172` (`op_spow`): `sign(a) * std::pow(|a|, e)`.
- Same class, latent (0 moved cells on this battery, affected on other inputs):
  - Tanh: `vm.hpp:244`, `oracle.hpp:513`, `streaming_engine.hpp:744`.
  - `ts_decay_exp` weights `std::pow(p0, k)`: `ts_ops.hpp:1183`, `ts_sliding.hpp:351,355`, `oracle.cpp:845`.

## 3. Evidence that the DLL, not the compiler, decides (`s2r-logs/`)

- Imports (`llvm-objdump -p`): Debug exe `ucrtbased.dll!pow`; Release exe `api-ms-win-crt-math-l1-1-0.dll!pow`
  (resolves to `ucrtbase.dll`).
- **Full-cell check** (`s2r-full-cell-attribution.txt`, `tools/s2r_full.py`). Inputs were read back from the
  `delay(close, 3)` and `ts_backfill(returns, 5)` rows; ctypes called each DLL's `pow`:
  - Debug == `ucrtbased!pow` on 706 / 733 / 706 cells of rows 5 / 6 / 83 (every recoverable cell).
  - Release == `ucrtbase!pow` on 706 / 733 / 706.
  - The moved cells: `s2r-moved-cells-vs-crt.txt`.
- **Neither DLL is correctly rounded.**
  - Debug is 1 ULP off the correctly rounded value at row 5 cell 593 (`sqrt`), row 6 cells 186, 201 and 255 (`x*x`),
    and row 83 cell 296 (`x^3`).
  - Release is 1 ULP off at row 5 cells 407 and 593, and row 6 cell 255.
- **The Debug pins are themselves host-dependent.** With `_set_FMA3_enable(0)`, `ucrtbased!pow` returns the Release
  bits for the row-5 and row-83 cells. A non-FMA3 CPU, or a different SDK `ucrtbased.dll`, fails the Debug pin too.
- **CRT scan** (`s2r-crt-scan.txt`; 200 000 random arguments per function, ucrtbase vs ucrtbased):

  | function | differing, FMA3 on (default) | differing, FMA3 off |
  |---|---|---|
  | `pow` (exponents 0.5, 2, 3, 0.3, 1.5, U(-3, 3)) | 366-401 | 316-404 |
  | `tanh` | 571 (all at abs(x) in [0.25, 2.03]) | 0 |
  | `exp`, `log`, `sqrt`, `log1p`, `expm1`, `log2`, `cbrt` | 0 | 0 |

  `pow(x, 1)` == x and `pow(x, 0)` == 1 hold exactly in both DLLs (200 000 / 200 000; `pow(NaN, 0)` = 1).

## 4. Why every fix is blocked by (d)

- **Meet the Debug pins from Release:** impossible from source. Release would have to call the non-redistributable debug
  CRT, or re-implement its closed FMA3-path `pow` bit for bit.
- **Any build-invariant form moves Debug.** Debug cells moved on the battery:

  | form | row 5 | row 6 | row 83 |
  |---|---|---|---|
  | `sqrt` / `x*x` / `(x*x)*x` | 1 | 3 | 169 |
  | correctly rounded `pow` | 1 | 3 | 1 |

  `s2r-full-cell-attribution.txt` has the cell lists.
- **A per-function pragma or attribute cannot help.** The difference is in which library is linked, not in code
  generation.
- **A whole-target flag (Debug with `/MD`)** is forbidden by the brief. It is also outcome (B) below, with
  `_ITERATOR_DEBUG_LEVEL` ABI cost.

## 5. Options for the PM (each needs a ruling, because each moves at least one Debug digest)

- **(A) In-repo deterministic `pow` / `tanh`.** For example a port of CORE-MATH `cr_pow` / `cr_tanh` (MIT, correctly
  rounded), written contraction-free (C2-RED rule).
  - Used by Pow, Spow, Tanh, the `ts_decay_exp` weights, the streaming engine and the parser fold.
  - Bits become a function of source alone: build-, CRT-, OS- and CPU-invariant. This is the DEC-11 / G-P3 intent.
  - Cost: one ruled re-pin of the three lit-golden digests to one set for both builds. Battery Debug cells moved: rows
    5 / 6 / 83 = 1 / 3 / 1. Release cells moved: 2 / 1 / 0.
  - Cost: a `dsl_vm` source-digest re-pin plus a `dsl_vm_semantics_version` bump. Signal bits of Pow / Spow / Tanh
    candidates can move, so cached entries must miss cleanly. Then X-5 u / w re-proof at merge.
  - Cost: about 1-2k lines of numerics with their own test (against exact rationals / mpmath). Slower than the CRT on
    those opcodes only.
  - A narrower variant: exact-exponent fast paths only (0.5 -> `sqrt`, 1, 2 -> `x*x`, 3 -> correctly rounded cube by
    Dekker products), with the CRT left for other exponents. That fixes the battery and `res_mom_12_1`, but leaves
    `power(x, 0.3)`-style strings and `tanh` CRT-dependent.
- **(B) One CRT for both builds.** A small shim binds `pow` / `tanh` to the release ucrt (`api-ms-win-crt-math`) in
  Debug as well.
  - Debug digests move to the current Release prints (`0xeeae40c4...` / `0x24c97b5b...` / `0xeeae40c4...`).
  - Release bits do not move. The Debug caches of affected candidates go stale, so they need a Debug-flavour bump.
  - Bits stay host-dependent: a Windows Update to `ucrtbase.dll` can move them silently.
- **(C) Keep the Debug pins, or add Release pins beside them:** refused by M2c-RED-1 (i), and (C) also fails on
  another host (section 3).

Recommendation: (A), full form. It is the only option under which one set of bits exists, which is what G-P3 requires.
If cost or time rules it out, take (B) as the interim and log the OS-servicing caveat.

## 6. Registered P9 strings / book candidates that use an affected opcode (Ruling M2c-RED-1 (v))

From `s2r-logs/s2r-spec-scan.txt`, all at HEAD:
- **P9 registrations:** `scripts/specs/p9/candidates/russell_recon.json` (4 strings) and `scripts/specs/p9/blocked/*.json`
  (2): **none**. The `v9/mine-c1` spec and the mine grammar (`strategy_mine*.cpp`): none.
- **Book (`libraries/v8ysb.json`, 67 members resolved via `alphas/registry.json`): 3 use an affected opcode.**
  - `cbop_f49` and `op_rd` use `power(xrd_ttm, 1 - ts_count_nans(xrd_ttm, 1))`. The exponent is always 0 or 1, which
    is exact in both DLLs, so they are **not affected**.
  - **`res_mom_12_1`** uses `signedpower(abs(1 - corr^2), 0.5)`. That is the CRT `pow(x, 0.5)`, a 1-ULP build
    dependence on about 0.2 % of intermediate arguments.
    - The outer `rank(decay_linear(delay(...)))` masks it unless a 1-ULP change reorders two cells of one
      cross-section.
    - Root's real-data Debug-vs-Release identity is consistent with that masking, but it does not prove it for all
      data.
    - This is the one book candidate exposed. `sqrt(abs(...))` would make it CRT-free, but changing a registered string
      is not mine to do.
- `^` with an exponent other than 2: none in the P9 surface.

## 7. Evidence (commands, exit codes, receipts)

| step | command | result |
|---|---|---|
| merge | `git merge --no-ff f6444417` | `f686080a`, no conflict; diff vs `f6444417` = 1 doc (`task-S2-rereview-1.md`) |
| p9-s2r-a | `research-build.ps1 -Tag p9-s2r-a -Targets atx-engine-alpha-tests` | exit 1 in 5 s, 0 TUs: pool-18 had no configured tree (`could not load cache`); receipt kept |
| configure | `atx-build.ps1 configure -Preset equity-dev` / `-Preset equity-rel` | exit 0 (36 s) / exit 0 (91 s) |
| p9-s2r-b | Debug, `atx-engine-alpha-tests` | exit 0, 529 s, 269 TUs, 0 warnings; exe sha256 `991d7978...` |
| p9-s2r-c | Release (`equity-rel`), same target | exit 0, 616 s, 269 TUs, 0 warnings; exe sha256 `6cba10b8...`; started after root's pool-2 build ended |
| Debug filter | `--gtest_filter=AlphaLitGolden_ExistingOps.*` | 2 / 2 pass; prints the pins (`debug-b-litgolden.log`) |
| Release filter | same | 1 pass / 1 fail: the red, with root's digests (`release-c-litgolden.log`) |
| Debug whole | `atx-engine-alpha-tests.exe` | exit 0, **782 / 782**, 1 disabled (`debug-b-alpha-whole.log`) |
| Release whole | same | exit 1, **779 passed / 1 skipped / 1 failed** (only M2c-RED; skip `AlphaVm_ZeroAlloc.WarmDispatchLoop_*` as at root), 1 disabled (`release-c-alpha-whole.log`) |
| dump | `ATX_LIT_GOLDEN_CELLS=1 ... --gtest_also_run_disabled_tests --gtest_filter=AlphaLitGolden_ExistingOps.DISABLED_*` | per-row digests: `debug-b-perrow-dump.log`, `release-c-perrow-dump.log`; diff `s2r-perrow-diff-debug-vs-release.txt` |

Receipts: `build-equity/mega-p9-s2r-{a,b,c}-receipt.json` (+ `-build.log`, `-cache-stats.log`).
- Not run, because no kernel or header changed: the S2 filter, `atx-impl-strategy-ic-tests`, and mine golden
  `0x889874a3b9b29c55`.
- No source digest re-pinned: `dsl_vm` stays `5bcd55c7` and `ic_result` stays `dd3c6011`. The test file is in
  neither list.

## 8. Change committed

- `atx-engine/tests/alpha/alpha_lit_golden_test.cpp`: the new `AlphaLitGolden_ExistingOps.DISABLED_PerRowDigestDump`.
  - It prints one digest per input column and one per battery row for ae / rf / oracle. With
    `ATX_LIT_GOLDEN_CELLS=1` it also prints every cell's bits.
  - It is disabled, so suite totals stay 782 / 781. It reads the env through `_dupenv_s` (house rule).
  - It is the diagnostic that names which opcode moved: diff two builds' outputs.
- No pin, expected value or kernel changed.
- `s2r-logs/` holds the logs listed above plus `tools/` (the diff, attribution and CRT-scan scripts; they run against a
  fresh dump).

## 9. Root items

1. Ruling needed: option (A), (B) or other. Until then M2c-RED stays a provisional known red, per M2c-RED-1 (iv).
2. Outside the alpha VM, the same CRT split applies to every run-time `std::pow` / `std::tanh` in engine code.
   - Examples: `capacity.hpp:252`, `cost_aware.hpp:122`, `execution_sim.hpp:470`, `replay_cost.cpp:27` (delta not
     0.5), `cov_ewma.hpp:125`, `specific_risk.hpp:162,169`, `vol_regime.hpp:165`, `signal_combiner.cpp:319`,
     `hac.hpp:214`, `spread_estimators.cpp:38`.
   - X-5 NAV Release-vs-Debug identity held, so those paths are masked or exact today. The G-P3 audit should list them.
3. `ucrtbase.dll` is OS-serviced, so Release bits of these calls can move with a Windows Update (no rebuild).
   Consider recording the ucrt version in the build flavour / receipt.
4. A follow-up round needs both trees rebuilt (about 9-10 min each). Mine are deleted below, per the disk rule.

## 10. Disk

After the commit, deleted by name: `build-equity-rel/` and `deps/equity-rel/` (pool-18; both created by this lane's
configure), the Debug build objects under `build-equity/` (receipts, build logs and the pre-existing `audits/` and
`v8-interim3-pitch-render-run2/` kept), and the scratch dumps / scripts in the session scratchpad (by name).

## 11. Ruling M2c-RED-2

Round 2, after Ruling M2c-RED-2. Commits: `5c9a5162` (the split, WIP at the owner stop), `49029727` (handoff), and
the round-2 final commit (S2R-1 comment + this report + logs). The attribution in sections 1-6 stands as written.

### 11.1 What changed

Only `atx-engine/tests/alpha/alpha_lit_golden_test.cpp`. No kernel, pin, golden, expected value or source digest moved.
`dsl_vm` stays `5bcd55c7` and `ic_result` stays `dd3c6011`; the test file is in neither list.

- **New `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeVmMatchesOracle`.** It runs in every build and checks the
  correctness property behind the pins, row by row over the 84-row battery:
  - VM AuditExact == oracle, bit for bit. This is CRT-independent: inside one build, both call the same `pow`.
  - ResearchFast serial == ResearchFast on a 2-worker `DetPool`, bit for bit.
  - On the 66 rows without a ResearchFast tolerance op: ResearchFast == AuditExact, bit for bit.
    - The tolerance ops are a frozen list, `kResearchFastToleranceOps`: 15 opcodes, the reroutes at `vm.hpp:1838-1935`.
  - On the 18 tolerance rows (`kResearchFastToleranceRows`, asserted): finite cells must be within
    `1e-9 * (1 + |ae|)`, and an infinity must be the same infinity in both paths.
    - The NaN pattern is not compared on these rows (Ruling S2R-1). The comment at `tolerance_mismatch` names the
      ruling. This round's only code change is that comment.
  - Every row has a finite cell.
  - Teeth: a 1-ULP perturbation of a real output is reported as a mismatch.
- **`EveryPreW2OpcodeDigestIsPinned`** still prints the three digests in every build. It compares them to the pins only
  under `#if defined(_MSC_VER) && defined(_DEBUG) && defined(_DLL)`, i.e. `/MDd` = `ucrtbased.dll`, the CRT the pins
  were captured with.
  - Why these macros: `_DEBUG` (a debug CRT) and `_DLL` (the DLL CRT) are set by the CRT switch, whatever `NDEBUG`
    says. `_MSC_VER` follows the precedent at `alpha_vm_test.cpp:56`.
  - In every other build the test is `GTEST_SKIP`ped with the message in 11.4.
- **Pins unchanged:** `0x971c3da60ca89aa3` / `0xe9e7128fd5359900` / `0x971c3da60ca89aa3`.
- **`DISABLED_PerRowDigestDump`** is kept as the diagnostic.

### 11.2 CRT files on this host

Measured 2026-10-04 and re-measured at the Release run; unchanged.

| file | FileVersion | bytes | SHA-256 |
|---|---|---|---|
| `C:\Windows\System32\ucrtbase.dll` (Release, OS-serviced) | 10.0.26100.7623 | 1,373,280 | `2c08e77f86b0953cffa505bdbe64e87ecb7c088db65d5acb9a2eee65a32565ef` |
| `C:\Windows\System32\ucrtbased.dll` (Debug, SDK copy) | 10.0.22000.832 | 2,229,728 | `19e79e91a6aadda65191ea6ddda4cc106e26c78338b05e73092e20709399f48d` |

- The Release exe imports `pow` from the api-set `api-ms-win-crt-math-l1-1-0.dll`, which resolves to `ucrtbase.dll`.
- `downlevel\api-ms-win-crt-math-l1-1-0.dll` is 10.0.26100.1, SHA-256 `c0502b62...f78f`.

**CRT-dependent opcodes:**
- Now: the `pow` family.
  - Pow: `^` and `power` with an exponent other than a literal 2 (`vm.hpp:259`, `oracle.hpp:539`,
    `streaming_engine.hpp:717`, the parser fold `parser.hpp:309`).
  - Spow: `vm.hpp:153`, `oracle.hpp:172`.
- Latent:
  - Tanh: `vm.hpp:244`, `oracle.hpp:513`, `streaming_engine.hpp:744`. It differs on 0.29 % of arguments with FMA3 on.
  - The `ts_decay_exp` weights: `ts_ops.hpp:1183`, `ts_sliding.hpp:351,355`, `oracle.cpp:845`.
- Unaffected: exponents 0 and 1, which are exact in both DLLs.

### 11.3 Results

| build | tag | run | result | log (`s2r-logs/`) |
|---|---|---|---|---|
| Debug (`equity-dev`) | p9-s2r-e (alpha), p9-s2r-d (ic) | `AlphaLitGolden_ExistingOps.*` | 3 OK, 1 disabled; prints the pins | `debug-e-litgolden.log` |
| Debug | p9-s2r-e | `atx-engine-alpha-tests` whole | **783 passed / 0 skipped / 0 failed**, 1 disabled | `debug-e-alpha-whole.log` |
| Debug | p9-s2r-d | `atx-impl-strategy-ic-tests`, S2 filter | **28 / 28** | `debug-d-ic-s2filter.log` |
| Debug | p9-s2r-d | `atx-impl-strategy-ic-tests` whole | **204 / 204** | `debug-d-ic-whole.log` |
| Release (`equity-rel`) | p9-s2r-g | `AlphaLitGolden_ExistingOps.*` | **2 OK + 1 SKIPPED** (the pinned test, with the message), 1 disabled; prints `0xeeae40c4a6d7d93b` / `0x24c97b5bfeab6518` / `0xeeae40c4a6d7d93b` (= root's M2c prints) | `release-g-litgolden.log` |
| Release | p9-s2r-g | `atx-engine-alpha-tests` whole | **780 passed / 2 skipped / 0 failed**, 1 disabled | `release-g-alpha-whole.log` |
| Release | p9-s2r-g | `atx-impl-strategy-ic-tests`, S2 filter | **28 / 28** | `release-g-ic-s2filter.log` |
| Release | p9-s2r-g | `atx-impl-strategy-ic-tests` whole | **204 / 204** | `release-g-ic-whole.log` |

Every count matches the expectation, so no difference needs explaining.
- **Debug alpha:** 783 = round 1's 782 + the new test.
- **Release alpha:** 782 tests ran = 780 passed + 2 skipped. The 2 skips are:
  - `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (M2c-RED-2);
  - `AlphaVm_ZeroAlloc.WarmDispatchLoop_BoundedByOutputsAndSizeIndependent`, a Release-only skip that already existed
    (also at root and in round 1's p9-s2r-c).
- **Against round 1's Release** (779 / 1 skipped / 1 failed): +1 for the new test, which passes, and the failure is now
  the named skip.
- **Debug was not re-run this round.** The only change since p9-s2r-e is a comment: 4 inserted and 2 deleted lines, all
  `//`.

Build evidence:
- Configure `equity-rel`: exit 0 (63 s); log `release-f-configure.log`.
- **p9-s2r-f**: exit 1 after 582 s, from a build trap and not a code red. Receipt `build-equity/mega-p9-s2r-f-*` kept.
  - The first launch was refused by the memory gate (free 1509 / commit 1481 MiB). The refusal threw before any receipt
    was written, so the tag was not used up. A bounded wait loop then launched the build.
  - The failure: `alpha_lit_golden_test.cpp` (the only TU that really compiled, because its comment changed) hit
    `file '...\deps\equity-rel\eigen3-src\Eigen\Dense' has been modified since the precompiled header
    'atx-engine-test-pch.dir\cmake_pch.cxx.pch' was built: mtime changed (was 1791125831, now 1791131812)`.
  - Cause: ccache restored round 1's test PCH, but this round's configure had re-fetched `deps/equity-rel` with new
    file times.
  - Fix, per the ledger precedent (root p9-c2-i, `progress.md` BUILD TRAP note):
    - deleted the two outputs of that PCH step by name (`atx-engine-test-pch.dir\cmake_pch.cxx.{pch,obj}`);
    - rebuilt under the next tag, with `CCACHE_RECACHE=1` for that one invocation.
- **p9-s2r-g**: exit 0, 257 s, 79 TUs, 3 links, 0 warnings. Provenance `49029727-dirty`, where the dirt is the S2R-1
  comment committed in this round.
  - Before starting, the gate loop waited about 7 min while another build ran on the host. The gate was never
    bypassed.
  - Exe SHA-256: alpha `8144b9e8ac55abfb...`, ic `67acb6841f60605f...`.

### 11.4 What root must re-prove at merge

1. **Targets:** `atx-engine-alpha-tests` and `atx-impl-strategy-ic-tests`, in Debug (`equity-dev`) and Release
   (`equity-rel`).
2. **Expected counts** on this lane's tree (root `f6444417` + S2R). Root's later merges may add tests; the deltas are
   what this lane fixes.

   | build | alpha whole | lit-golden filter | ic S2 filter | ic whole |
   |---|---|---|---|---|
   | Debug | 783 passed / 0 skipped / 0 failed (1 disabled) | 3 OK; prints the pins | 28 / 28 | 204 / 204 |
   | Release | 780 passed / 2 skipped / 0 failed (1 disabled) | 2 OK + 1 SKIPPED | 28 / 28 | 204 / 204 |

   - The Release skips must be exactly `EveryPreW2OpcodeDigestIsPinned` and
     `AlphaVm_ZeroAlloc.WarmDispatchLoop_BoundedByOutputsAndSizeIndependent`.
   - `EveryPreW2OpcodeVmMatchesOracle` must PASS in both builds.
3. **The exact skip message**, verbatim from `release-g-litgolden.log`:

   > Ruling M2c-RED-2: the pins are a Debug-CRT golden on this host (ucrtbased.dll, /MDd). This build links another
   > CRT (Release: ucrtbase.dll), whose std::pow differs from ucrtbased.dll's by 1 ULP on ~0.2% of arguments (rows
   > power(close, 0.5), signedpower(returns, 2), close ^ 3). Correctness runs in every build:
   > EveryPreW2OpcodeVmMatchesOracle.

4. **Release prints on this host:** `0xeeae40c4a6d7d93b` / `0x24c97b5bfeab6518` / `0xeeae40c4a6d7d93b`, with
   `ucrtbase.dll` 10.0.26100.7623 (`2c08e77f...`).
   - A different Release print is a skip, not a failure, but it means the UCRT or the battery changed. Apply
     M2c-RED-2 (d).
5. **Gate wording** (M2c-RED-2 (c)): 0 failed, with one named Release skip.
6. **Build trap:** if a pool's `deps/<preset>` was re-fetched after ccache stored its PCHs, a changed PCH-consumer TU
   fails with "has been modified since the precompiled header". Use the fix in 11.3.

### 11.5 P10 notes

- **Streaming-path `ts_zscore` fix (Ruling S2R-1).** Give the ResearchFast Welford state the A-09 guard.
  - Where: `TsvWelfordState::emit`, `ts_ops.hpp:884-910`. The VM sweep (`vm.hpp:1931`) and the streaming engine
    (`streaming_engine.hpp:191,1058`) share it.
  - What: on a window that is flat by the `tsv_is_flat` rule (`sqrt(s/n) <= 1e-10*|m|`), Zscore -> NaN and
    Var / Std -> exactly 0.
  - Also give the tiny-window direct `rsquare` (`ts_sliding.hpp:599`, d <= 4, test `ss_tot == 0.0`) the relative
    guard.
  - Cost:
    - the ResearchFast payloads of every candidate whose `ts_zscore` / `stddev` / `ts_std` / `ts_var` sees a flat
      window move;
    - the P9-B0 ResearchFast references must be re-based;
    - a `dsl_vm` source-digest re-pin and a semantics bump;
    - S2's review reopens.
  - After the fix, the test can compare the NaN pattern one way on the tolerance rows: AuditExact NaN => ResearchFast
    NaN. It still cannot compare both ways, because of the documented sliding divergence (NaN where the batch value is
    roundoff, `ts_sliding.hpp` NUMERICS).
- **In-repo deterministic `pow` / `tanh`.** The owner's call (M2c-RED-2 (a), OD5-2); options as in section 5.
  - **(A) In-repo `pow` / `tanh`.**
    - The three lit-golden pins move to one set for both builds. Debug rows 5 / 6 / 83 move 1 / 3 / 1 cells; Release
      rows 5 / 6 / 83 move 2 / 1 / 0 cells.
    - No other test pins evaluated values of these ops (canary, mine and formulaic sha pins: none use them).
    - `dsl_vm` source-digest re-pin, and `dsl_vm_semantics_version` 1 -> 2. That version keys the cache dir, so every
      candidate cache entry misses once.
    - P9-B0 re-base: `res_mom_12_1` (`signedpower(.., 0.5)`) can move.
    - A correctly rounded `pow` means vendoring third-party source (e.g. CORE-MATH `cr_pow`, MIT, about 1.5k lines), a
      new third-party source and the owner's call. The alternative is an in-house implementation that is deterministic
      but not correctly rounded: IEEE basic ops only, contraction-free.
  - **(B) One CRT for both builds.**
    - Debug pins move to the Release prints above. Release bits do not move. The Debug cache flavour needs a bump. No
      third-party code.
    - The bits stay OS-serviced: a Windows Update of `ucrtbase.dll` can move them (M2c-RED-2 (d) guard).
- **Unverified deletion (disclosed):** in round 1 I deleted the session scratchpad's `.mypy_cache` by name.
  - It was probably created by the mypy hook on my scratch scripts, but it was not verified as mine.
  - It is a regenerable cache only.

## 12. Ruling S2R-1: book candidates and P9 strings that feed a flat-capable input to a flat-window-split op

This was done by source reading only: specs and kernels, no data, no statistic. Script `s2r-logs/tools/s2r1_scan.py`,
output `s2r-logs/s2r1-spec-scan.txt`.

### 12.1 Which ops split on a flat window (AuditExact batch with the A-09 guard vs the ResearchFast reroute)

The only ResearchFast mode branches are `vm.hpp:1838-1935`. A third, the `delta` SIMD subtraction at `vm.hpp:2033`,
is elementwise and therefore bit-exact. Every other op, every W2 op included, runs the same kernel in both modes.

| op (DSL names) | AuditExact on a flat window | ResearchFast path | ResearchFast on a flat window |
|---|---|---|---|
| `ts_zscore` | NaN (`ts_ops.hpp:1144`) | Welford `emit`, no guard (`ts_ops.hpp:904-905`) | `(x - m) / sqrt(clamped s / (n-1))`, which by source can be four things (below). **The ruled split.** |
| `stddev`, `ts_std`, `ts_var` | exactly 0 (`ts_ops.hpp:230-231`) | same Welford `emit` (`ts_ops.hpp:897-903`) | the clamped residual: 0 or a tiny positive value, inside the 1e-9 contract (below) |
| `rsquare` | NaN | TimeReg lane | d <= 4: the direct fit tests only `ss_tot == 0.0` (`ts_sliding.hpp:599`), so a finite roundoff ratio is possible; d > 4: `kCancelGuard` gives NaN (`ts_sliding.hpp:677`, matches) |
| `slope`, `resid` | exactly 0 | TimeReg lane (`ts_sliding.hpp:669-674`) | about 0 (roundoff); value-only |
| `correlation` / `ts_corr`, `ts_regression` | NaN | pair lane with `relative_flat = true` (`ts_sliding.hpp:245,307-312`) | NaN: **no split** |
| `covariance`, `ts_av_diff`, `decay_linear`, `wma`, `ts_sum`, `ts_mean`, `ts_decay_exp` | no flat guard in the batch kernel | rerouted | no flat-window split (tolerance only) |
| skew / kurt / `ts_skew` / `ts_kurt`, `ts_scale`, the other ts ops, every W2 op (`ts_std_mp`, `ts_zscore_mp`, `ts_beta_on`, `ts_topk_mean`, ...) | - | not rerouted | same kernel in both modes: **no split** |

Detail for the first two rows:
- **`ts_zscore`:** ResearchFast gives one of:
  - 0, when the compensated mean rounds back to `x` exactly. That is the battery's 204 cells.
  - ±O(1) noise (the pre-A-09 symptom), when it does not.
  - ±inf, when `s` clamps to 0 with `x != m`.
  - NaN, when both are 0.
  - So the ruling's "scores 0" is the case observed on the battery, not the only one the source allows.
- **`stddev` / `ts_std` / `ts_var`:** the residual also appears on an all-zero window reached by sliding. A Welford
  state is reset only when its last finite cell leaves.
  - The NaN pattern of the op itself is the same in both modes.
  - A downstream `x / stddev(..)` or `0 * log(stddev(..))` turns an AuditExact NaN into a finite ResearchFast value.

Both production consumers default to ResearchFast:
- the IC runner, unless `--audit-exact` (`strategy_ic_runner.cpp:296`);
- the NAV / targets runner (`strategy_runner.cpp:804`).

### 12.2 Where I looked

- **Book:** spec `scripts/specs/v8/lib-v8ysb-gm.json`. Its `inputs.library` is
  `atx-impl/strategies/fund_industry_ic_v8ysb.json`: 67 candidates with inline `dsl`.
  - Cross-checks: same ids as `libraries/v8ysb.json`; every `dsl` equals `alphas/registry.json`; the spec's
    `baseline_library` (`fund_industry_ic_v8x3b.json`) is 58 of the 67 with equal `dsl`.
- **P9:** every JSON under `scripts/specs/p9/`: `candidates/russell_recon.json`, `blocked/hf_crowd.json`,
  `blocked/lazy_prices.json`.
- **Match:** a word-bounded call of `ts_zscore`, `stddev`, `ts_std`, `ts_var`, `slope`, `rsquare` or `resid` in any
  string. The parser has no macro that lowers to these opcodes; only the registry names in `registry.cpp:76-101` do.

### 12.3 The list

- **`ts_zscore` (the ruled split): 0 book candidates, 0 P9 strings.**
- **`ts_var`, `slope`, `rsquare`, `resid`: 0 / 0.** `qmj_safety` matches "slope" only in its `notes.deviation` prose.
- **P9 strings: 0.** The P9 DSL strings are `rank(russell_recon)`, `rank(lazy_prices)` and `rank(hf_crowd)`
  (blocked, no field); none uses a time-series op. [Root M2f, slip S-3 / Ruling S2R-3: this sentence first said
  "`hf_crowd` has no DSL yet"; `scripts/specs/p9/blocked/hf_crowd.json:3` carries `"dsl": "rank(hf_crowd)"`. The
  conclusion is unchanged.]
- **`stddev` (the TsStd form of the same Welford cause): 5 book candidates.**

| id | fragment | input | can the input be flat over the window? | effect if flat |
|---|---|---|---|---|
| `smax` | `ts_max(ret, 21) / stddev(ret, 252) + 0 * log(stddev(ret, 252))`, with `ret = close / delay(close, 1) - 1` | daily return | only as an all-zero window: the role close carried unchanged (not NaN) for 253 sessions | **can split like S2R-1.** AuditExact: stddev 0, so `0 * log(0)` = NaN; that idiom is the author's zero-std guard, and the name drops. ResearchFast: a tiny positive residual gives `0 / tiny + 0 * log(tiny)` = 0, so the name scores 0 (mid-book). If the residual clamps to 0, both give NaN |
| `smax5` | `ts_topk_mean(ret, 21, 5) / stddev(ret, 21) + 0 * log(stddev(ret, 21))` | daily return | all-zero window: 22 sessions of unchanged close (about a one-month halt with a carried close) | **can split like S2R-1**, as `smax` |
| `iv_rv_spread` | `ts_backfill(iv_atm_21d + ..., 5) - stddev(ret, 21) * 15.8745` | daily return | 22 sessions of unchanged close | value only: AuditExact subtracts 0, ResearchFast at most about 1e-8. The rank is unchanged unless two names tie to that precision |
| `qmj_safety` | `rank(decay_linear(-1 * stddev(ni_q / be_lag1q + 0 * log(be_lag1q), 252), 21))` | as-of quarterly ROE, forward-filled between filings (A-09's own case) | yes, if no new quarterly value enters for 252 sessions (fundamentals stale for a year while the name stays in the universe) | value only: AuditExact -0 exactly, ResearchFast -tiny. Only the order among the tied flat names can change |
| `res_mom_12_1` | `ts_sum(ret, 231) / stddev(ret, 231)`, `stddev(mkt_ret, 231)` | daily return; market return | `ret`: all-zero for 232 sessions only. `mkt_ret`: no | **masked**: the same expression's `correlation(ret, mkt_ret, 231)` has a flat side, which is NaN in both modes (pair-lane guard). The NaN propagates through the `-`, so both modes give NaN |

Whether any universe name actually has 22 or 253 sessions of carried, unchanged, non-NaN close, or a year of stale
ROE, is a data question. It is not answered here; the lane is blind.

For the prereg (S2R-1): no P9 string and no book candidate uses `ts_zscore`. The same-cause `stddev` form reaches the
book through `smax` and `smax5`. By PATH-1, each cell's eval mode equals its reference's, so a flat-window name
behaves the same way on both sides of a pair.

## 13. Disk (round 2)

After the round-2 commit, by name:
- `build-equity-rel/` and `deps/equity-rel/`, both created by this round's configure; then `deps/` if empty.
- My scratch scripts in the session scratchpad: `s2r-f-wait-build.ps1`, `s2r-g-wait-build.ps1`, `s2r1_scan.py`.

Kept: the receipts and logs `build-equity/mega-p9-s2r-{f,g}-*` and `s2r-logs/`.
