# Task S2R report: Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (M2c-RED, Ruling M2c-RED-1)

Lane S2R, pool-18, branch `feat/p9-s2-20261003`. Base `8314fb59` + merge of root `f6444417` (`f686080a`, no conflict;
code tree = root's code tree). 2026-10-04.

**STATUS: BLOCKED (stop under hard constraint (d)).** The red is fully attributed, but no fix can meet the contract.
Making Release produce the Debug bits from source is impossible, and every build-invariant fix moves a Debug digest.
The options below need a PM ruling. No pin, golden or expected value was edited. No kernel source changed. The only
code change is a disabled diagnostic test that counts in no suite total.

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
