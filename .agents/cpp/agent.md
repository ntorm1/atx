# C++ Coding Agent — atx

Profile for an agent writing/reviewing C++ in this repo. Target: **safety-critical-grade quality** without sacrificing modern C++ ergonomics. Standard: **C++20**. Toolchain: **clang-cl 18 (VS 2022 LLVM) + CMake presets + Ninja + LLD**, ccache compiler cache. Test: GoogleTest (vcpkg).

Authoritative sources, in precedence order when they conflict:
1. This document.
2. **C++ Core Guidelines** (Stroustrup/Sutter) — default idiom.
3. **JPL Power of 10** — discipline for critical paths.
4. **MISRA C++:2023** / **AUTOSAR C++14** — restriction rationale.
5. **SEI CERT C++** — security/UB rules.

---

## Quick start — build, test, worktrees

Monorepo, layered: `atx-core` (vocab + IO) → `atx-tsdb` (shm store) → `atx-engine` (alpha / factory / learn / risk) → `atx-impl` (pipeline binary). Toolchain: **clang-cl + Ninja + CMake presets**; deps via vcpkg manifest + pinned FetchContent.

**Compiler: clang-cl 18 (VS 2022 LLVM), Debug `/Od /RTC1 -MDd /Z7` (embedded debug info — keeps objects cacheable), LLD linker, `/W4 /permissive- /WX`.** Ninja ships inside the VS install, not on PATH — use `scripts/atx-build.ps1` from any shell (sources vcvars, pins Ninja + SDK `mt.exe`, exports `CCACHE_BASEDIR`, forwards to cmake/ctest):

```powershell
powershell scripts\atx-build.ps1 configure                  # cmake --preset dev into build/ (-Preset dev-shared to flip)
powershell scripts\atx-build.ps1 build atx-vol-tests        # any target(s) — ALWAYS target-scoped, never bare all-targets
powershell scripts\atx-build.ps1 check atx-vol\src\foo.cpp  # single-TU compile, no link — seconds-level type-check loop
powershell scripts\atx-build.ps1 -Ctest -L atx_vol_fast     # ctest passthrough
cmake --preset dev -DATX_TEST_GROUPS="risk;data"            # compile only engine groups you touch
```

**Iterate loop discipline:** `check <file.cpp>` while shaping code (compiles just that
object), `build <owning-test-target>` + `-Ctest -R <Suite>` to go green, full suite only
at gate time. Never build the whole graph out of habit.

| Preset | Use |
|---|---|
| `dev` | **the** iterate preset (scripts target it): ccache + shared deps/vcpkg + PCH, unity OFF, static libs |
| `ninja` | alias of the same `_base` behavior; same `build/` dir |
| `dev-shared` | `dev` + atx libs as **DLLs** — smallest artifacts, fastest relinks. **Iteration only, NEVER the test gate**: header-inline instrumentation globals (`atx/vol/detail/counters.hpp` solve ledger) split per DLL image → SolveLedger/BacktestExec observer suites fail deterministically (verified 2026-07-22) |
| `rel` / `rel-avx2` | Release benchmarks (`build-rel*/`); `rel-avx2` adds global `/arch:AVX2`, never `/fp:fast` |
| `hygiene` | PCH **OFF** — strict per-TU includes; the include-clean gate, run by hand — there is no CI here (§8) — own `build-hygiene/` |
| `vs` | Visual Studio MSBuild generator (IDE escape hatch) |

**Worktree POOL first (2026-07-22): lease a warm tree, don't create a fresh one.**
Measured: fresh worktree builds `atx-vol-tests` in 132 s at only 38% cache hits (PCH-consumer
TUs structurally miss cross-worktree); the same warm tree no-ops in 5 s and rebuilds a
preset/branch flip with hot cache in 27 s. Pool trees keep `build/` warm across tasks.

```powershell
scripts\dev-setup.ps1                                # one-time per machine: ccache + caches (then NEW shell)
scripts\lease-worktree.ps1 -Branch feat/x            # DEFAULT: lease warm pool tree (auto-grows to 4)
scripts\lease-worktree.ps1 -Release pool-1           # done: detach branch, keep build/ warm
scripts\lease-worktree.ps1 -Status                   # who holds what
scripts\new-worktree.ps1 -Name s8 -Branch feat/s8 -Base main -Isolated   # only for deliberate isolation (deps churn, bench)
```

No build tree is copied; three shared caches soften what the pool doesn't cover (full mechanics + caveats: `scripts/dev-setup.md`):
- **ccache** (`C:\atx-cache\ccache`) — `CCACHE_BASEDIR` + `hash_dir=false` + prefix-map `ignore_options` + `include_file_mtime/ctime` sloppiness make the same TU hash identically in every worktree → cross-worktree cache **hits** (objects byte-identical). PCH-consumer test TUs are the exception: they recompile once per worktree at PCH speed — the pool exists precisely because that class never transfers. Check health with `ccache -s` — `Uncacheable` should stay ~0.
- **shared vcpkg** (`VCPKG_INSTALLED_DIR=C:\atx-cache\vcpkg_installed`) — one Arrow/Parquet/gtest payload, configure says "already installed" in seconds. Branch with a *different* `vcpkg.json`? Override per-worktree: `-DVCPKG_INSTALLED_DIR=build/vcpkg_installed`.
- **shared FetchContent** (`$ATX_DEPS_DIR` = `C:\atx-cache\deps`, all presets) — spdlog/Eigen/xsimd fetched once.

clangd works immediately (committed `.clangd` reads each worktree's own `build/compile_commands.json`).

**Gotchas that still bite:**

1. **Manual `git worktree add` skips submodules** — `atx-core/third-party/databento-cpp` arrives empty and configure dies. `new-worktree.ps1` handles it; otherwise run `git submodule update --init --recursive`.
2. **Don't build with raw `cmake --build`/ninja outside `atx-build.ps1` or `--preset`** — preset `environment` (the `CCACHE_*` keys) doesn't apply to raw invocations; the global ccache config covers most of it, but `CCACHE_BASEDIR` only comes from the preset or the script.
3. **`-DATX_UNITY_BUILD=ON` (opt-in; only worth it on a cold from-scratch build) collides on some test groups** — identically-named file-local helpers merge into one TU (`factory`, `parallel` groups). Unity is OFF in `dev`; leave it off for iteration — per-TU objects are what the cache keys on.
4. **ProcessExecutor tests need `atx-shm-worker` built beside the test exe** — building only `atx-engine-<group>-tests` omits the worker the multi-process executor spawns; `*SeqParallel` suites then fault (SEH `0xc000001d`). Build both: `... build atx-engine-<group>-tests atx-shm-worker`.
5. **`pwsh` is not always installed** — use `powershell` (5.1) for the `*.ps1` helpers.
6. **Pool leases are advisory** (`.atx-lease` marker, git-ignored) — one agent per leased tree; `-Release` refuses a dirty tree, so commit/stash before releasing. Release detaches at the base branch so your feature branch stays mergeable elsewhere while `build/` stays warm.

---

## 0. Prime directives

- **No undefined behavior. Ever.** UB is a correctness bug, not a style issue. If you cannot prove a construct is defined, do not write it.
- **Make illegal states unrepresentable.** Encode invariants in types, not comments. A function that cannot be called wrong beats one documented not to be.
- **Every line is read 100×, written once.** Optimize for the reader: explicit > clever.
- **Fail loud in debug, fail safe in release.** `assert` invariants; never let a violated invariant silently corrupt state.
- **If you can't test it, you can't ship it.** TDD: red → green → refactor. No production code without a failing test first.

---

## 1. Memory & ownership

| Rule | Do | Don't |
|------|-----|-------|
| Ownership | `unique_ptr` (sole), `shared_ptr` (shared, justify it), value types | raw `new`/`delete`, owning raw pointers |
| Allocation | RAII containers; prefer stack/`std::array` | manual buffer mgmt; `malloc`/`free` |
| Borrowing | `T&` / `const T&` / `std::span` / raw `T*` (non-owning, documented non-null) | passing ownership via raw pointer |
| Lifetime | one clear owner; no dangling | returning ref/`span`/`string_view` to a temporary or local |

- **Zero `new`/`delete` in application code.** Allocation lives inside containers/factories. `make_unique`/`make_shared` only.
- **Rule of Zero.** Define no special members; let RAII members handle it. If you must (Rule of Five), define all five or `= delete`/`= default` explicitly.
- **Critical/hot paths: no dynamic allocation after init.** Pre-size, `reserve`, use fixed-capacity types. Document the allocation budget.
- `string_view`/`span` are **non-owning** — never outlive the backing storage. Don't store one as a member unless the owner's lifetime is guaranteed to enclose it.

---

## 2. Types & const-correctness

- `const` by default. Mutable is the exception you justify. Mark every member function `const` that doesn't mutate; mark every parameter/local `const` that isn't reassigned.
- `constexpr`/`consteval` wherever the value is compile-time knowable. Push work to compile time.
- **No implicit narrowing.** Brace-init `{}` to catch it. No C-style casts — `static_cast`/`gsl::narrow`/`bit_cast`; `reinterpret_cast` only with written justification; never `const_cast` to mutate.
- `enum class`, never plain `enum`. Strong types over primitives for units/ids (`struct Meters { double v; };`) — kill parameter-swap bugs.
- `auto` when the type is obvious or noise; spell the type when it aids the reader. `[[nodiscard]]` on anything whose return must not be dropped (factories, error codes, `empty()`).
- Fixed-width integers (`std::int32_t`) at boundaries/ABI. Never assume `int` width. Watch signed/unsigned mixing — UB-adjacent and a classic CERT finding.

---

## 3. Control flow (JPL discipline)

- Functions **short, single-purpose** — aim ≤ ~60 lines, one screen. One reason to change.
- **Bounded loops.** Every loop has a statically obvious upper bound. No `while(true)` without a proven exit.
- **All branches handled.** `switch` on `enum class` covers every enumerator (no `default`, so the compiler flags new ones) or has an explicit `default` that `assert(false)`/`std::unreachable()`.
- Cyclomatic complexity low. Deep nesting → extract function. Early-return guard clauses over nested `if`.
- **Smallest scope.** Declare at first use, initialize on declaration. No uninitialized variables — ever.
- No `goto`. No recursion in critical paths unless depth is statically bounded.

---

## 4. Error handling

- **Exceptions** for truly exceptional, locally-unrecoverable errors at API boundaries. RAII guarantees cleanup on throw. Be **exception-safe**: state the *strong* or *basic* guarantee per function.
- **`std::expected<T, E>`** (until C++23 available, a vocabulary `Result<T,E>` or `optional` + error enum) for *expected* failures in normal flow (parse, lookup, I/O). Don't throw for control flow.
- **`noexcept`** on anything that genuinely can't throw — moves, swaps, destructors, leaf math. Destructors never throw.
- **Validate at the boundary.** Check all external inputs (args, files, network, env) once at entry; interior code assumes validated invariants.
- Never ignore an error. No empty `catch`. No discarded `[[nodiscard]]`. Handle, propagate, or `assert` impossibility with a comment why.
- **Critical-path profile:** error-code/`expected` returns only, exceptions disabled — when targeting it, say so and obey consistently.

---

## 5. Concurrency

- Prefer immutability and message passing to shared mutable state. Shared state is guilty until proven safe.
- Guard each shared mutable datum with one clearly-owned mutex; `lock_guard`/`scoped_lock` (RAII), never manual lock/unlock. Document lock order; acquire in a global order to prevent deadlock.
- `std::atomic` for lock-free counters/flags; default `seq_cst` until a relaxed ordering is *proven* correct and commented.
- No data races (UB). **There is no TSan build in this repo** (§8), so race-freedom rests on the ownership and lock discipline above plus review — write the argument down in a comment instead of deferring to a tool that will not run. `jthread`/`stop_token` over raw `thread`. Never detach without a lifetime argument.
- Don't hold a lock across a callback or blocking call.

---

## 6. API & class design

- Interfaces minimal, complete, hard to misuse. Take the **weakest sufficient type** (`span`/`string_view`/concept-constrained template over concrete containers).
- Single Responsibility. Composition over inheritance. Inherit only for runtime polymorphism behind an abstract interface; mark `override`, leaf classes/virtuals `final`. Public base destructors `virtual`.
- No raw pointers in interfaces for ownership — smart pointers or values. Non-owning observe = `T*`/`T&`/`span` with documented non-null.
- Free functions over members when they don't need private state (better encapsulation). Every path that touches data enforces its invariants.
- Keep headers clean: declarations, contracts, forward declarations, and minimal includes. Put implementation in `.cpp` files whenever possible; headers should not accumulate private helper code or heavy dependency "fill".
- Prioritize private PIMPL implementations for classes with non-trivial state, unstable dependencies, or expensive includes. Store the opaque `Impl` by `std::unique_ptr`; define `Impl` and member bodies in the source file.
- Templates: constrain with **concepts** (C++20). No SFINAE soup. `static_assert` intent with a message. Keep error messages human-readable.

---

## 7. Testing (GoogleTest, TDD)

- **Test first.** Write the failing test, watch it fail for the right reason, then implement.
- One behavior per `TEST`. Arrange-Act-Assert. Name `Subject_Condition_ExpectedResult`.
- Cover: happy path, **boundaries** (0, 1, max, empty, overflow edge), error paths, invariant violations. Test the contract, not the implementation.
- `EXPECT_*` for soft checks; `ASSERT_*` when later lines would crash/UB on failure. `EXPECT_DEATH`/assertion tests for documented preconditions.
- Deterministic, isolated, fast. No sleeps, no real network/clock — inject them. Fixtures (`TEST_F`) for shared setup.
- New bug → reproduce with a failing test *first*, then fix. Cover `atx-core`/`atx-engine` logic; don't chase 100% on trivial glue.
- **Engine tests are grouped by subsystem, one executable per group.** A new `*_test.cpp` goes in `atx-engine/tests/<group>/` — match the file prefix to the folder (`risk_*` → `risk/`); cross-cutting/misc lands in `core/`. **The group list lives in `ATX_ALL_TEST_GROUPS` at `atx-engine/tests/CMakeLists.txt` — read it there, and do not copy it back into this document.** A copy used to sit here and had already drifted (it was missing `quant`), which is worse than no list: an agent placing a `quant_*_test.cpp` would have consulted it, not found the group, and misfiled the test into `core/`. Reading the definition costs one file open and cannot go stale; and an unknown group name is a hard configure error, so a typo fails loudly rather than silently landing somewhere else. CMake auto-globs each dir (`CONFIGURE_DEPENDS` — no manual source list); the file builds into `atx-engine-<group>-tests`. Engine includes are absolute (`atx/...`), so a test compiles the same in any folder. Worktrees compile a slice via `-DATX_TEST_GROUPS="risk;data"`; master builds all. Touching one group relinks only that group — keep a test in the folder of the subsystem it exercises so the blast radius stays small.

---

## 8. Tooling & build (which gates are real, and which are not built)

- **Warnings are errors.** `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` (Clang/GCC) / `/W4 /permissive- /WX` (MSVC).
- **Sanitizers: NOT BUILT. Nothing here has ever been run under one — there is no sanitizer box to tick.** There is no CI in this repository at all (no `.github/`, `.gitlab-ci.yml`, `azure-pipelines.yml`, `.circleci/`, `Jenkinsfile`, or `.buildkite/`), and no sanitizer build: `CMakePresets.json` defines exactly `_base, ninja, dev, dev-counters, dev-shared, rel, rel-avx2, hygiene, vs`; root `CMakeLists.txt` declares no `option(ATX_*SAN*)`; `cmake/` holds only the install/version/config templates; `scripts/atx-build.ps1` has no sanitizer verb or flag. The `-fsanitize` wiring that does exist in the tree is the vendored `atx-core/third-party/databento-cpp/cmake/Sanitizers.cmake`, and it is inert — its `DATABENTO_ENABLE_{ASAN,TSAN,UBSAN}` options default OFF and nothing first-party sets them. This entry records an unimplemented intention so it stops reading as a gate that passed; it is not licence to write UB, which §0 forbids on its own authority.
- **If a sanitizer build is ever added, add exactly this** (costed and ruled 2026-08-14): one preset inheriting the **static** `dev` shape — never `dev-shared` — carrying `-fsanitize=float-cast-overflow,integer-divide-by-zero -fno-sanitize=vptr,alignment,function` through the `CFLAGS`/`CXXFLAGS` environment mechanism `rel-avx2` already uses, plus a per-worktree `-DFETCHCONTENT_BASE_DIR=...` that is **mandatory, not tidiness**: root `CMakeLists.txt:182-183` adopts `$ENV{ATX_DEPS_DIR}` as the FetchContent base, that cache is keyed by dependency and not by build configuration, every pool worktree shares it, and it has already produced two `_ITERATOR_DEBUG_LEVEL` ABI collisions — an instrumented build written into it breaks the other trees' links. `float-cast-overflow` earns its place because the shape is live here: `atx-vol/include/atx/vol/detail/strip_grid.hpp:211-216` caps `intervals` before a `ceil()`→`size_t` cast *because* the out-of-range cast is UB, and node counts get derived from doubles in more than one place. `float-divide-by-zero` is deliberately **excluded** — guard expressions here rely on `log(0) == -inf`, so it would fire on correct code. `alignment` and `vptr` are off because deliberate unaligned SIMD loads and RTTI-free numeric types trip them without finding anything.
- **ASan and full UBSan were declined on the evidence — and REVERSE THIS the day a defect lands that a sanitizer would have caught.** Read the reversal condition before the reasoning, because the decline rests on a prior, not a proof. The prior: the defect classes fixed on this branch do not qualify — NaN propagation, a stale memoized value, an inverted sign, a convergence-order collapse, a duplicated predicate and a missing map key are all **defined** behaviour, and no sanitizer has a NaN check. But that is evidence about the defects we *found*, and sanitizers earn their keep on the defects nobody found, so it lowers the prior rather than proving absence. On top of that prior: allocation is container/RAII-mediated (`alloca` appears zero times under `atx-vol/src` + `atx-vol/include`), so ASan has no manual buffer-arithmetic layer to protect; the 23 hand-written files under `atx-vol/src/simd/` use the masked-tail idiom that reads a vector width past the logical end, which ASan reports as `heap-buffer-overflow`, so the **expected** yield is benign findings each needing a human to adjudicate — an expectation, not a measurement, since no ASan run has ever happened here; and an instrumented `atx-vol` over uninstrumented `atx::core`/`atx::tsdb` cannot see overflows in the buffers those layers own, which is most of the container and SIMD machinery.
- **clang-tidy is disabled in this repo.** Do not run it or treat it as a gate unless the user explicitly re-enables it. The root `.clang-tidy` intentionally sets `Checks: '-*'` because the broad profile is prohibitively slow on umbrella-header translation units. **clang-format is not run either** — nothing in the build, the scripts, or any hook invokes it, and the standing instruction is to run neither tool unless the user explicitly asks. So formatting is governed by a named enforcer rather than a tool: the **100-column limit**, **matching the style of the surrounding file**, and **human review at the gate**. `.clang-format` is the reference for what that style is, not a check that runs — the rule stands, the automation does not exist.
- **Standalone static analysis: also NOT BUILT.** No clang-analyzer or cppcheck run is wired anywhere first-party; the `cppcheck` references in the tree amount to `atx-core/CMakeLists.txt:57` forcing the vendored `DATABENTO_ENABLE_CPPCHECK` OFF. With clang-tidy deliberately off as well (above), what actually runs on every build is the compiler's own `/W4 /permissive- /WX` (`ATX_WERROR`, ON by default at root `CMakeLists.txt:257`) — treat *its* findings as defects, and don't record a second analyser as enforced until one exists.
- **Build presets** (full table + worktree workflow in the Quick start above): `dev` is the canonical iterate preset (ccache + shared deps + PCH + LLD); `hygiene` (PCH OFF — strict per-TU include build) is the include-clean gate. PCH parses Eigen+gtest once but **hides missing/unused includes** — run `cmake --preset hygiene && cmake --build --preset hygiene` before claiming include-clean. Don't add volatile (frequently-edited) headers to `pch.hpp` — it invalidates the shared PCH and forces a full rebuild.
- Reproducible builds; dependencies are pinned via the **vcpkg manifest** (`vcpkg.json` + `builtin-baseline`), incl. GoogleTest — not rebuilt per build dir, because `_base` points every preset at one shared `VCPKG_INSTALLED_DIR` (`C:/atx-cache/vcpkg_installed`, `CMakePresets.json`). Behind that sits vcpkg's *own* default binary cache (`%LOCALAPPDATA%\vcpkg\archives`), which is vcpkg's default behaviour and not something this repo configures — there is no `VCPKG_BINARY_SOURCES` here. Header-only deps (Eigen, spdlog, xsimd) come via pinned `FetchContent` tags. No build-time network beyond those pinned fetches.

---

## 9. Documentation & comments

- Comment **why**, never **what**. The code says what. A comment that restates code → delete it.
- Document each function's **contract**: preconditions, postconditions, invariants, ownership transfer, error/throw behavior, thread-safety. Brief Doxygen `@param`/`@return`/`@throws` on public APIs.
- `// SAFETY:` comment on every justified deviation (a `reinterpret_cast`, a relaxed atomic, a disabled lint). No deviation without rationale.
- No commented-out code, no dead code — delete it; git remembers.

---

## 10. Review checklist (gate before "done")

- [ ] No UB; no narrowing; no uninitialized vars; no raw owning pointers.
- [ ] All inputs validated at boundary; all error paths handled or asserted.
- [ ] `const`/`constexpr`/`noexcept`/`[[nodiscard]]` applied where they hold.
- [ ] Ownership & lifetimes unambiguous; no dangling refs/spans; Rule of Zero/Five satisfied.
- [ ] Every `switch`/branch exhaustive; loops bounded; functions small.
- [ ] Tests written first, cover boundaries + failures, and pass under the `dev` preset. No sanitizer box — §8 records why none exists and what would have to be built.
- [ ] Warnings clean at `/W4 -Werror`; 100-column limit held and surrounding style matched. Run neither clang-tidy nor clang-format — §8 names what governs formatting instead.
- [ ] Includes verified under the `hygiene` preset (PCH-on default build masks missing/unused includes); new `*_test.cpp` placed in the correct `tests/<group>/` folder.
- [ ] Public API documents contract; deviations carry `// SAFETY:` rationale.

> When a rule and a deadline conflict, the rule wins. Slow-and-correct beats fast-and-undefined. If a guideline genuinely doesn't fit, deviate **explicitly** with a written reason — never silently.
