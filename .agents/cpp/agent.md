# C++ Coding Agent — atx

Profile for an agent writing/reviewing C++ in this repo. Target: **safety-critical-grade quality** without sacrificing modern C++ ergonomics. Standard: **C++20**. Toolchains: MSVC 19.4x, Clang. Build: CMake. Test: GoogleTest.

Authoritative sources, in precedence order when they conflict:
1. This document.
2. **C++ Core Guidelines** (Stroustrup/Sutter) — default idiom.
3. **JPL Power of 10** — discipline for critical paths.
4. **MISRA C++:2023** / **AUTOSAR C++14** — restriction rationale.
5. **SEI CERT C++** — security/UB rules.

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
- No data races (UB). Run **TSan**. `jthread`/`stop_token` over raw `thread`. Never detach without a lifetime argument.
- Don't hold a lock across a callback or blocking call.

---

## 6. API & class design

- Interfaces minimal, complete, hard to misuse. Take the **weakest sufficient type** (`span`/`string_view`/concept-constrained template over concrete containers).
- Single Responsibility. Composition over inheritance. Inherit only for runtime polymorphism behind an abstract interface; mark `override`, leaf classes/virtuals `final`. Public base destructors `virtual`.
- No raw pointers in interfaces for ownership — smart pointers or values. Non-owning observe = `T*`/`T&`/`span` with documented non-null.
- Free functions over members when they don't need private state (better encapsulation). Every path that touches data enforces its invariants.
- Templates: constrain with **concepts** (C++20). No SFINAE soup. `static_assert` intent with a message. Keep error messages human-readable.

---

## 7. Testing (GoogleTest, TDD)

- **Test first.** Write the failing test, watch it fail for the right reason, then implement.
- One behavior per `TEST`. Arrange-Act-Assert. Name `Subject_Condition_ExpectedResult`.
- Cover: happy path, **boundaries** (0, 1, max, empty, overflow edge), error paths, invariant violations. Test the contract, not the implementation.
- `EXPECT_*` for soft checks; `ASSERT_*` when later lines would crash/UB on failure. `EXPECT_DEATH`/assertion tests for documented preconditions.
- Deterministic, isolated, fast. No sleeps, no real network/clock — inject them. Fixtures (`TEST_F`) for shared setup.
- New bug → reproduce with a failing test *first*, then fix. Cover `atx-core`/`atx-engine` logic; don't chase 100% on trivial glue.

---

## 8. Tooling & build (non-negotiable gates)

- **Warnings are errors.** `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` (Clang/GCC) / `/W4 /permissive- /WX` (MSVC).
- **Sanitizers in CI:** ASan + UBSan on every test run; TSan for threaded code. A sanitizer hit fails the build.
- **clang-tidy** with `cppcoreguidelines-*`, `bugprone-*`, `cert-*`, `misc-*`, `performance-*`. **clang-format** enforced; formatting is not a review topic.
- Static analysis (clang-analyzer / cppcheck) in CI. Treat findings as defects.
- Reproducible builds; pin dependency versions (GoogleTest tag). No build-time network beyond pinned fetches.

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
- [ ] Tests written first, cover boundaries + failures, pass under ASan/UBSan/TSan.
- [ ] Warnings clean at `/W4 -Werror`; clang-tidy/format clean.
- [ ] Public API documents contract; deviations carry `// SAFETY:` rationale.

> When a rule and a deadline conflict, the rule wins. Slow-and-correct beats fast-and-undefined. If a guideline genuinely doesn't fit, deviate **explicitly** with a written reason — never silently.
