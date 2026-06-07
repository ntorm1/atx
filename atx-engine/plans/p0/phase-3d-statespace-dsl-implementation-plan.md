# Phase 3d — State-Space & Mean-Reversion DSL Nodes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Kalman (scalar local-level + Chan 2-state regression) and OU (recurrence filter + rolling-fit family) operators to the alpha DSL, plus the language machinery they need — local let-bindings, records, member-access (`.pin`), and a true multi-output IR.

**Architecture:** Extend the existing `lex → parse → analyze → build_dag → linearize → VM ‖ oracle` pipeline. Multi-output is SSA-style: a compute node writes K contiguous slots, `Pin` nodes project one column each; hash-consing makes "compute once, many pins" automatic. Filter hyperparameters are compile-time immediates (not operands), keeping operand arity ≤ 3. Every op gets a fast VM kernel and an independent oracle reference, proven bit-for-bit equal by the differential harness.

**Tech Stack:** C++20 header-only (`atx::engine::alpha`), GoogleTest, CMake + Ninja + clang-cl + vcpkg. Build/test per `.agents/atx-engine/agent.md`. Design spec: `phase-3d-statespace-dsl-design.md`.

---

## Conventions for every task

- **Build:** `cmake --build build --target atx-engine-tests` (from repo root of THIS worktree, `C:/Users/natha/atx-wt/alpha-statespace-nodes`). Configure once if `build/` missing: `cmake --preset ninja -DATX_BUILD_TESTS=ON`. Run from a VS Developer shell with `VCPKG_ROOT` set.
- **Run one suite:** `ctest --test-dir build -R <RegexOfTestSuite> --output-on-failure`.
- **Tests auto-globbed** (`tests/*_test.cpp`, CONFIGURE_DEPENDS). Do NOT edit `CMakeLists.txt`. New test file → reconfigure is automatic on next build; if a brand-new file isn't picked up, re-run the `cmake --preset` line.
- **Gates:** `/W4 /permissive- /WX` (any warning fails). `clang-format -i <file>` before commit. Do NOT run clang-tidy (disabled repo-wide).
- **Commits:** explicit pathspecs (never `git add -A`). Message `feat(p3d-N): …` or `test(p3d-N): …`, trailer `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. This is an isolated worktree branch (`feat/alpha-statespace-nodes`) — no shared-branch race, but keep pathspecs explicit.
- **C++ rules** (`.agents/cpp/agent.md`): no UB, `const`/`constexpr`/`noexcept`/`[[nodiscard]]`, `Result<T>`/`Status` not exceptions, exhaustive enum switches (no `default` over `OpCode`), functions ≤ 60 lines, zero hot-path alloc.
- **After each new `OpCode`:** EVERY exhaustive switch over `OpCode` must add the case or the build breaks. Known switch sites: `vm.hpp::dispatch`, `oracle.hpp` dispatch, `typecheck.hpp::is_rolling_ts` (and any sibling family predicate), `parser.hpp::infix_bp`/`binary_opcode` (only if a TokenKind is added). Grep `case OpCode::` to find them all.

---

## File Structure

| File | Responsibility | Phases |
|------|----------------|--------|
| `include/atx/engine/alpha/lexer.hpp` | tokenizer; add `Dot` | B |
| `include/atx/engine/alpha/parser.hpp` | Pratt parse; binding table, diagnostics, member parselet, hparam peel | A,B |
| `include/atx/engine/alpha/registry.hpp` | OpCode/OpSig/Library; `n_hparams`, `pins`, new ops | B,C,D,E |
| `include/atx/engine/alpha/typecheck.hpp` | shape/dtype/lookback; record TypeInfo, `analyze_member`, op pinning | A,B,C,D,E |
| `include/atx/engine/alpha/dag.hpp` | hash-cons; `n_out`, `hparams`, `imm_bits`, Member→Pin lowering | B,C,D |
| `include/atx/engine/alpha/bytecode.hpp` | linearize; `Instr.imm[2]`, `n_out`, slot blocks, Pin emit | B |
| `include/atx/engine/alpha/state_ops.hpp` | recurrence step kernels (kalman_level, kalman_reg, ou_filter) | C,D |
| `include/atx/engine/alpha/ts_ops.hpp` | rolling AR(1) OLS kernels (OU family) | E |
| `include/atx/engine/alpha/vm.hpp` | fast VM: Pin, strided state, recurrence/windowed dispatch | B,C,D,E |
| `include/atx/engine/alpha/oracle.hpp` | independent reference for all of the above | B,C,D,E |
| `tests/alpha_*_test.cpp` | unit + differential + known-value | all |

---

# PHASE A — Local bindings + references

**Outcome:** `m = ts_mean(close,5); a = m / close` works (`m` referenced, not a field). Shadowing a field warns. Record-valued bindings deferred to Phase B (no records yet, so every binding is single-signal). No new ops.

## Task A1: Diagnostic channel on the parser

**Files:**
- Modify: `include/atx/engine/alpha/parser.hpp`
- Test: `tests/alpha_bindings_test.cpp` (create)

- [ ] **Step 1: Write the failing test** (`tests/alpha_bindings_test.cpp`)

```cpp
#include <gtest/gtest.h>
#include "atx/engine/alpha/parser.hpp"

using namespace atx::engine::alpha;

TEST(AlphaBindings, ReferenceReusesBinding) {
  Library lib;
  // `m` is bound, then referenced; the reference must NOT become a Field load.
  auto ast = parse_program("m = ts_mean(close, 5)\na = m\n", lib);
  ASSERT_TRUE(ast) << ast.error().message();
  // Two roots (m, a). `a`'s root expr must point at the SAME node `m` points at,
  // i.e. `a` is not a fresh Field("m").
  const auto roots = ast.value().roots();
  ASSERT_EQ(roots.size(), 2u);
  const ExprId m_root = roots[0].root;
  const ExprId a_root = roots[1].root;
  EXPECT_EQ(m_root, a_root); // `a = m` reuses m's ExprId
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-engine-tests; ctest --test-dir build -R AlphaBindings --output-on-failure`
Expected: FAIL — currently `a = m` parses `m` as `Field("m")`, so `a_root != m_root`.

- [ ] **Step 3: Implement binding table + binding-first resolution**

In `parser.hpp`, add to `struct Parser` (after `atx::usize pos{0};`):

```cpp
  // Local bindings (Phase 3d-A): name -> the bound expression's ExprId. A bare
  // identifier resolves binding-first, field-fallback. Declaration order; a later
  // binding may shadow an earlier one (last wins) and shadow a panel field (warned).
  struct Binding {
    std::string name;
    ExprId id{kNoExpr};
  };
  std::vector<Binding> bindings;
  std::vector<std::string> warnings; // advisory diagnostics (non-fatal)

  [[nodiscard]] ExprId lookup_binding(std::string_view name) const noexcept {
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
      if (it->name == name) {
        return it->id; // last binding wins (shadowing)
      }
    }
    return kNoExpr;
  }
```

In `parse_prefix`, the `TokenKind::Ident` arm — BEFORE building a `Field`, check bindings (only when the ident is NOT a call, i.e. next token is not `(`):

```cpp
  case TokenKind::Ident: {
    p.advance();
    if (p.peek_kind() == TokenKind::LParen) {
      return parse_call(p, p.text(tok), tok); // cursor on '('
    }
    if (const ExprId bound = p.lookup_binding(p.text(tok)); bound != kNoExpr) {
      return atx::core::Ok(bound); // reference an earlier binding (reuse its ExprId)
    }
    Expr e;
    e.kind = Expr::Kind::Field;
    e.name_id = p.ast->intern(p.text(tok));
    return atx::core::Ok(p.ast->add(e));
  }
```

In `parse_program`, after `ATX_TRY(const ExprId root, …)` and BEFORE `ast.add_root(...)`, register the binding:

```cpp
    ATX_TRY(const ExprId root, detail::parse_precedence(p, detail::kBpTernary));
    p.bindings.push_back(detail::Parser::Binding{std::string{p.text(name_tok)}, root});
    ast.add_root(std::string{p.text(name_tok)}, root);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --test-dir build -R AlphaBindings --output-on-failure`
Expected: PASS.

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i include/atx/engine/alpha/parser.hpp tests/alpha_bindings_test.cpp
git add include/atx/engine/alpha/parser.hpp tests/alpha_bindings_test.cpp
git commit -m "feat(p3d-A1): local bindings — binding-first identifier resolution"
```

## Task A2: Forward references are NOT allowed; self-reference is a field

**Files:**
- Test: `tests/alpha_bindings_test.cpp` (append)

- [ ] **Step 1: Write the test** (append)

```cpp
TEST(AlphaBindings, ForwardReferenceIsFieldFallback) {
  Library lib;
  // `b` used before it is bound -> field-fallback (Field("b")), not a reference.
  auto ast = parse_program("a = b\nb = close\n", lib);
  ASSERT_TRUE(ast) << ast.error().message();
  const auto& nodes = ast.value().nodes();
  const ExprId a_root = ast.value().roots()[0].root;
  EXPECT_EQ(nodes[a_root].kind, Expr::Kind::Field); // `b` was not yet bound
}

TEST(AlphaBindings, SelfReferenceOnRhsIsField) {
  Library lib;
  // `x = x + 1`: the RHS `x` is resolved BEFORE x is registered -> Field("x").
  auto ast = parse_program("x = close\ny = x + 1\n", lib);
  ASSERT_TRUE(ast) << ast.error().message();
  // y's RHS `x` references the binding from line 1 (close), so y_root is a Binary
  // whose left child equals x_root.
  const auto& nodes = ast.value().nodes();
  const ExprId x_root = ast.value().roots()[0].root;
  const ExprId y_root = ast.value().roots()[1].root;
  ASSERT_EQ(nodes[y_root].kind, Expr::Kind::Binary);
  EXPECT_EQ(nodes[y_root].a, x_root);
}
```

- [ ] **Step 2: Run — expected PASS already** (the binding is registered AFTER its RHS is parsed, so forward/self refs fall back to fields by construction).

Run: `ctest --test-dir build -R AlphaBindings --output-on-failure`
Expected: PASS. If FAIL, the registration in A1 happened before RHS parse — fix ordering (register after `parse_precedence`).

- [ ] **Step 3: Commit**

```bash
git add tests/alpha_bindings_test.cpp
git commit -m "test(p3d-A2): pin forward/self-reference field-fallback semantics"
```

## Task A3: Shadow warning when a binding name is later used as a bare identifier

**Files:**
- Modify: `include/atx/engine/alpha/parser.hpp` (emit warning), public API to surface warnings
- Test: `tests/alpha_bindings_test.cpp` (append)

> **Design note:** the parser has no panel field list, so "shadows a field" cannot be detected precisely. We warn on the observable event the design specifies: a bare identifier resolved to a binding (i.e. it *could* otherwise have been a field). This is advisory. We surface warnings via a new optional out-param overload so existing call sites are unchanged.

- [ ] **Step 1: Write the test** (append)

```cpp
TEST(AlphaBindings, BindingReferenceEmitsShadowWarning) {
  Library lib;
  std::vector<std::string> warnings;
  auto ast = parse_program("m = ts_mean(close, 5)\na = m + close\n", lib, &warnings);
  ASSERT_TRUE(ast) << ast.error().message();
  // `m` on line 2 resolved to the binding -> one advisory warning mentioning m.
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("m"), std::string::npos);
}

TEST(AlphaBindings, NoBindingUseNoWarning) {
  Library lib;
  std::vector<std::string> warnings;
  auto ast = parse_program("a = close + open\n", lib, &warnings);
  ASSERT_TRUE(ast) << ast.error().message();
  EXPECT_TRUE(warnings.empty());
}
```

- [ ] **Step 2: Run to verify it fails** (no `warnings` param exists yet → compile error).

- [ ] **Step 3: Implement** — emit warning at the resolution site, add the overload.

In `parse_prefix`'s Ident arm, when resolving a binding:

```cpp
    if (const ExprId bound = p.lookup_binding(p.text(tok)); bound != kNoExpr) {
      p.warnings.push_back(std::string{"binding '"} + std::string{p.text(tok)} +
                           "' shadows a possible panel field of the same name");
      return atx::core::Ok(bound);
    }
```

Add an out-param overload of `parse_program` (keep the existing 2-arg one delegating with `nullptr`):

```cpp
[[nodiscard]] inline atx::core::Result<Ast>
parse_program(std::string_view source, const Library &lib, std::vector<std::string> *warnings) {
  ATX_TRY(auto toks, detail::lex_checked(source));
  Ast ast;
  detail::Parser p{std::span<const Token>{toks}, source, &lib, &ast, 0};
  while (p.peek_kind() != TokenKind::End) {
    if (p.peek_kind() != TokenKind::Ident) {
      return atx::core::Err(detail::parse_error("expected assignment target (IDENT)", p.peek()));
    }
    const Token name_tok = p.peek();
    p.advance();
    if (p.peek_kind() != TokenKind::Assign) {
      return atx::core::Err(detail::parse_error("expected '=' after assignment target", p.peek()));
    }
    p.advance();
    ATX_TRY(const ExprId root, detail::parse_precedence(p, detail::kBpTernary));
    p.bindings.push_back(detail::Parser::Binding{std::string{p.text(name_tok)}, root});
    ast.add_root(std::string{p.text(name_tok)}, root);
  }
  if (warnings != nullptr) {
    *warnings = std::move(p.warnings);
  }
  return atx::core::Ok(std::move(ast));
}

[[nodiscard]] inline atx::core::Result<Ast> parse_program(std::string_view source,
                                                          const Library &lib) {
  return parse_program(source, lib, nullptr);
}
```

- [ ] **Step 4: Run to verify PASS.**

Run: `ctest --test-dir build -R AlphaBindings --output-on-failure`

- [ ] **Step 5: Backward-compat sweep — run the full alpha suite.**

Run: `ctest --test-dir build -R "Alpha|Parser|Typecheck|Dag|Bytecode|Vm|Oracle|Differential" --output-on-failure`
Expected: all green (binding-first is a superset; no existing program collides).

- [ ] **Step 6: clang-format + commit**

```bash
clang-format -i include/atx/engine/alpha/parser.hpp tests/alpha_bindings_test.cpp
git add include/atx/engine/alpha/parser.hpp tests/alpha_bindings_test.cpp
git commit -m "feat(p3d-A3): shadow warning + warnings out-param on parse_program"
```

---

# PHASE B — Multi-output IR + records + member access

**Outcome:** A synthetic test-only op `split2(x) → {hi, lo}` (hi=x, lo=-x) proves the whole multi-output path: lexer `Dot`, `Member` AST, record TypeInfo, `Pin` opcode, slot blocks, `imm[2]`, `NodeKey.imm_bits`, registry `pins`/`n_hparams`. Differential test green on `split2`.

## Task B1: `Instr.imm` → `array<f64,2>`; `Node.hparams`; `NodeKey.imm_bits`

**Files:**
- Modify: `include/atx/engine/alpha/bytecode.hpp` (`Instr`), `include/atx/engine/alpha/dag.hpp` (`Node`, `NodeKey`, hash, `make_node`), `include/atx/engine/alpha/vm.hpp` + `oracle.hpp` (Const reads `imm[0]`)
- Test: `tests/alpha_immediates_test.cpp` (create)

- [ ] **Step 1: Write the failing test** — a Const program still evaluates (imm[0] path).

```cpp
#include <gtest/gtest.h>
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
// Reuse the suite's panel-builder helper if one exists; otherwise build a 2x2 panel.

using namespace atx::engine::alpha;

TEST(AlphaImmediates, ConstStillWorksWithImmArray) {
  Library lib;
  auto ast = parse_program("a = 3.5 + close\n", lib);
  ASSERT_TRUE(ast);
  auto an = analyze(ast.value()); ASSERT_TRUE(an);
  auto prog = compile(ast.value(), an.value()); ASSERT_TRUE(prog);
  // Panel: 1 date, 1 instrument, close=2.0.
  auto panel = /* build via existing test helper: close=2.0 */;
  Engine eng(panel);
  auto out = eng.evaluate(prog.value());
  ASSERT_TRUE(out);
  EXPECT_DOUBLE_EQ(out.value().alphas[0].values[0], 5.5);
}
```

> Use the existing panel test helper from the current alpha test suite (grep `Panel` in `tests/alpha_*_test.cpp` for the builder). If none, construct via `Panel::create`.

- [ ] **Step 2: Run to verify FAIL** (compile error once you change `imm`, or pre-change it passes — this test is the regression guard for the refactor).

- [ ] **Step 3: Implement the type changes.**

`bytecode.hpp` `Instr`:

```cpp
struct Instr {
  OpCode op{};
  SlotId dst{kNoSlot};
  std::array<SlotId, 3> src{kNoSlot, kNoSlot, kNoSlot};
  atx::u32 param{};                    // LoadField id / Ts window / StoreAlpha out / Pin index
  atx::u8 n_out{1};                    // output pins (>=2 for record compute nodes)
  std::array<atx::f64, 2> imm{};       // Const: imm[0]; filter hyperparams: imm[0],imm[1]
};
```

`dag.hpp` `Node`:

```cpp
struct Node {
  OpCode op{};
  Shape shape{};
  DType dtype{};
  atx::u16 lookback{};
  std::array<NodeId, 3> in{kNoNode, kNoNode, kNoNode};
  atx::f64 value{};                    // Const literal
  std::array<atx::f64, 2> hparams{};   // filter hyperparameters (immediates)
  atx::u32 param{};                    // LoadField id / Pin index
  atx::u8 n_out{1};                    // output pins
  atx::u32 refcount{};
};
```

`dag.hpp` `NodeKey` + hash:

```cpp
struct NodeKey {
  OpCode op{};
  atx::u64 param{};
  std::array<NodeId, 3> children{kNoNode, kNoNode, kNoNode};
  std::array<atx::u64, 2> imm_bits{};  // bit_cast of hparams (so distinct hparams don't CSE)
  [[nodiscard]] bool operator==(const NodeKey &) const noexcept = default;
};
// NodeKeyHash: fold imm_bits[0], imm_bits[1] into hash_combine alongside the existing fields.
```

Update `detail::make_node` to copy `n.n_out` / `n.hparams` from a proto (add params), and the `Const` reader in `vm.hpp::eval_const` (`c = in.imm[0];`) and `oracle.hpp` Const. In `bytecode.hpp::linearize`, set `instr.imm[0] = n.value` for Const (replacing `instr.imm = n.value`), and copy `instr.n_out = n.n_out`.

- [ ] **Step 4: Run to verify PASS** + full alpha suite green (refactor must not regress).

Run: `ctest --test-dir build -R "Alpha|Vm|Oracle|Differential" --output-on-failure`

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i include/atx/engine/alpha/bytecode.hpp include/atx/engine/alpha/dag.hpp include/atx/engine/alpha/vm.hpp include/atx/engine/alpha/oracle.hpp tests/alpha_immediates_test.cpp
git add include/atx/engine/alpha/bytecode.hpp include/atx/engine/alpha/dag.hpp include/atx/engine/alpha/vm.hpp include/atx/engine/alpha/oracle.hpp tests/alpha_immediates_test.cpp
git commit -m "feat(p3d-B1): imm[2] + Node.hparams/n_out + NodeKey.imm_bits"
```

## Task B2: `SlotPool::acquire_block` / `release_block`

**Files:**
- Modify: `include/atx/engine/alpha/bytecode.hpp` (`detail::SlotPool`)
- Test: `tests/alpha_slotpool_test.cpp` (create)

- [ ] **Step 1: Write the failing test**

```cpp
#include <gtest/gtest.h>
#include "atx/engine/alpha/bytecode.hpp"
using namespace atx::engine::alpha::detail;

TEST(SlotPoolBlock, ContiguousBlockThenRelease) {
  SlotPool pool;
  const SlotId base = pool.acquire_block(3);
  EXPECT_EQ(pool.peak(), 3u);
  // Block is contiguous: base, base+1, base+2.
  pool.release_block(base, 3);
  // A same-size block reuses the freed slots (peak does not grow).
  const SlotId reused = pool.acquire_block(3);
  EXPECT_EQ(reused, base);
  EXPECT_EQ(pool.peak(), 3u);
}

TEST(SlotPoolBlock, SingleAcquireUnaffected) {
  SlotPool pool;
  const SlotId a = pool.acquire();
  const SlotId b = pool.acquire();
  EXPECT_NE(a, b);
}
```

- [ ] **Step 2: Run to verify FAIL** (`acquire_block` undefined).

- [ ] **Step 3: Implement** in `SlotPool`:

```cpp
  // Acquire `k` CONTIGUOUS slots; returns the first. Reuses a freed same-size
  // block if available, else grows the high-water counter by k. k>=1.
  [[nodiscard]] SlotId acquire_block(atx::u32 k) {
    if (k == 1) {
      return acquire();
    }
    if (auto it = free_blocks_.find(k); it != free_blocks_.end() && !it->second.empty()) {
      const SlotId s = it->second.back();
      it->second.pop_back();
      return s;
    }
    const SlotId s = next_slot_;
    next_slot_ += k;
    return s;
  }
  void release_block(SlotId first, atx::u32 k) {
    if (k == 1) {
      release(first);
      return;
    }
    free_blocks_[k].push_back(first);
  }
```

Add member `std::unordered_map<atx::u32, std::vector<SlotId>> free_blocks_;` (cold path — alloc fine). Include `<unordered_map>`.

- [ ] **Step 4: Run to verify PASS.**

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i include/atx/engine/alpha/bytecode.hpp tests/alpha_slotpool_test.cpp
git add include/atx/engine/alpha/bytecode.hpp tests/alpha_slotpool_test.cpp
git commit -m "feat(p3d-B2): SlotPool contiguous block acquire/release"
```

## Task B3: Registry — `PinSig`, `OpSig.pins`/`n_hparams`, `OpCode::Pin`, synthetic `Split2`

**Files:**
- Modify: `include/atx/engine/alpha/fwd.hpp` (add `Pin`, `Split2` to OpCode if enumerated there), `include/atx/engine/alpha/registry.hpp`
- Test: `tests/alpha_registry_test.cpp` (append, or create)

- [ ] **Step 1: Write the failing test**

```cpp
TEST(AlphaRegistry, Split2IsRecordWithTwoPins) {
  Library lib;
  const OpSig* s = lib.find("split2");
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->min_arity, 1u);
  ASSERT_EQ(s->pins.size(), 2u);
  EXPECT_EQ(s->pins[0].name, "hi");
  EXPECT_EQ(s->pins[1].name, "lo");
}
```

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement.**

Add to `OpCode` enum (registry.hpp, near the store/free section, BEFORE `StoreAlpha`): `Pin,` and `Split2,`. Update every exhaustive `OpCode` switch (grep `case OpCode::`) with these two cases (Pin handled specially in VM/oracle; Split2 in dispatch).

Add `PinSig` + extend `OpSig` (registry.hpp):

```cpp
struct PinSig {
  std::string_view name;
  DType dtype{DType::F64};
};

struct OpSig {
  std::string_view name;
  atx::u8 min_arity{};
  atx::u8 max_arity{};
  OpCode opcode{OpCode::Const};
  DType out_dtype{DType::F64};
  bool lookahead_safe{true};
  std::array<atx::f64, kMaxDefaults> defaults{};
  Shape (*shape_of)(std::span<const Shape> args){nullptr};
  atx::u8 n_hparams{0};            // trailing args parsed as constant-literal immediates
  std::span<const PinSig> pins{};  // empty = single output; else record pin table
};
```

Add the `split2` built-in row. First a static pin table near `builtin_ops()`:

```cpp
inline constexpr std::array<PinSig, 2> kSplit2Pins = {{{"hi", DType::F64}, {"lo", DType::F64}}};
```

Then in `kOps` (bump the array size by 1): a row with `pins` set. Positional aggregate init now needs the two trailing fields:

```cpp
      {"split2", 1, 1, OpCode::Split2, DType::F64, true, {}, &shape_panel, 0,
       std::span<const PinSig>{kSplit2Pins}},
```

(All other rows get `, 0, {}` appended for the two new trailing fields, OR rely on default member initializers — confirm aggregate init still compiles; if the existing rows use full positional init, append `0, {}` to each. Prefer: leave existing rows as-is if designated/partial aggregate init fills the rest with defaults; `n_hparams{0}` and `pins{}` have member-initializers so trailing omission is legal in aggregate init.)

`register_op` validation additions:

```cpp
  if (sig.n_hparams > sig.max_arity) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          std::string{"register_op: n_hparams exceeds arity for '"} +
                              std::string{sig.name} + "'");
  }
  if (!sig.pins.empty() && sig.pins.size() < 2) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          std::string{"register_op: record op needs >=2 pins for '"} +
                              std::string{sig.name} + "'");
  }
```

- [ ] **Step 4: Run to verify PASS.**

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i include/atx/engine/alpha/registry.hpp include/atx/engine/alpha/fwd.hpp tests/alpha_registry_test.cpp
git add include/atx/engine/alpha/registry.hpp include/atx/engine/alpha/fwd.hpp tests/alpha_registry_test.cpp
git commit -m "feat(p3d-B3): PinSig + OpSig.pins/n_hparams + Pin/Split2 opcodes + split2 builtin"
```

## Task B4: Lexer `Dot` token

**Files:**
- Modify: `include/atx/engine/alpha/lexer.hpp`
- Test: `tests/alpha_lexer_test.cpp` (append)

- [ ] **Step 1: Write the failing test**

```cpp
TEST(AlphaLexer, DotToken) {
  auto toks = lex("kf.beta");
  ASSERT_TRUE(toks);
  // Ident("kf"), Dot, Ident("beta"), End
  ASSERT_GE(toks.value().size(), 4u);
  EXPECT_EQ(toks.value()[0].kind, TokenKind::Ident);
  EXPECT_EQ(toks.value()[1].kind, TokenKind::Dot);
  EXPECT_EQ(toks.value()[2].kind, TokenKind::Ident);
}
```

- [ ] **Step 2: Run to verify FAIL** (`TokenKind::Dot` undefined). Note: ensure the lexer does not treat `.` as part of a number when not preceded/followed by a digit — `kf.beta` must split. A `.` adjacent to digits (`3.5`) stays a number; a `.` after an identifier char is a Dot.

- [ ] **Step 3: Implement** — add `Dot` to `TokenKind`; in the lexer's main switch, emit `Dot` for `.` when not part of a numeric literal. Update every exhaustive `TokenKind` switch (`parser.hpp::infix_bp`, `binary_opcode`) with a `case TokenKind::Dot:` returning `kBpNone` / `OpCode::Const` (Dot is handled as a postfix in the Pratt loop, not via these tables).

- [ ] **Step 4: Run to verify PASS.**

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i include/atx/engine/alpha/lexer.hpp include/atx/engine/alpha/parser.hpp tests/alpha_lexer_test.cpp
git add include/atx/engine/alpha/lexer.hpp include/atx/engine/alpha/parser.hpp tests/alpha_lexer_test.cpp
git commit -m "feat(p3d-B4): lexer Dot token"
```

## Task B5: Parser — `Expr::Kind::Member` + postfix `.IDENT` + hparam peel

**Files:**
- Modify: `include/atx/engine/alpha/parser.hpp`
- Test: `tests/alpha_member_test.cpp` (create)

- [ ] **Step 1: Write the failing test**

```cpp
#include <gtest/gtest.h>
#include "atx/engine/alpha/parser.hpp"
using namespace atx::engine::alpha;

TEST(AlphaMember, ParsesMemberOnCall) {
  Library lib;
  auto ast = parse_program("b = split2(close).hi\n", lib);
  ASSERT_TRUE(ast) << ast.error().message();
  const auto& nodes = ast.value().nodes();
  const ExprId root = ast.value().roots()[0].root;
  ASSERT_EQ(nodes[root].kind, Expr::Kind::Member);
  EXPECT_EQ(nodes[nodes[root].a].kind, Expr::Kind::Call); // member of a call
}

TEST(AlphaMember, MemberOnBinding) {
  Library lib;
  auto ast = parse_program("kf = split2(close)\nb = kf.lo\n", lib);
  ASSERT_TRUE(ast) << ast.error().message();
  const auto& nodes = ast.value().nodes();
  const ExprId b_root = ast.value().roots()[1].root;
  ASSERT_EQ(nodes[b_root].kind, Expr::Kind::Member);
  // member's record child is the split2 call bound to kf
  EXPECT_EQ(nodes[nodes[b_root].a].kind, Expr::Kind::Call);
}
```

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement.**

Add `Member` to `Expr::Kind` enum and document `a = record expr, name_id = pin name`. Add a postfix member parselet in `parse_precedence`'s loop — handle `TokenKind::Dot` with the highest precedence (check it BEFORE the infix-bp test, like a postfix):

```cpp
  for (;;) {
    if (p.peek_kind() == TokenKind::Dot) {
      p.advance();
      if (p.peek_kind() != TokenKind::Ident) {
        return atx::core::Err(parse_error("expected pin name after '.'", p.peek()));
      }
      const Token pin = p.peek();
      p.advance();
      Expr m;
      m.kind = Expr::Kind::Member;
      m.a = left;
      m.name_id = p.ast->intern(p.text(pin));
      left = p.ast->add(m);
      continue;
    }
    const TokenKind k = p.peek_kind();
    const atx::u8 lbp = detail::infix_bp(k);
    // … existing infix handling …
  }
```

Hparam peel in `parse_call`, AFTER `fill_default_args` and BEFORE building the Call Expr: peel the last `sig->n_hparams` args into the Call node's hparams. Since `Expr` stores children in `a/b/c`, and hparams are NOT children, store the peeled literal *values* on the Call Expr. Add to `Expr`: `std::array<atx::f64,2> hparams{};` and `atx::u8 n_hparams{0};`. Implementation:

```cpp
  // Peel trailing hyperparameter args (constant literals) off the operand list.
  Expr e;
  e.kind = Expr::Kind::Call;
  e.op = sig;
  e.opcode = sig->opcode;
  e.n_hparams = sig->n_hparams;
  const atx::usize n_oper = args.size() - sig->n_hparams;
  for (atx::usize k = 0; k < sig->n_hparams; ++k) {
    const Expr &h = p.ast->node(args[n_oper + k]);
    // Validated as a finite literal in analyze; here capture the folded value
    // (non-literal -> store NaN sentinel, analyze rejects it with a clear error).
    e.hparams[k] = (h.kind == Expr::Kind::Literal) ? h.value
                                                   : std::numeric_limits<atx::f64>::quiet_NaN();
  }
  e.a = n_oper > 0 ? args[0] : kNoExpr;
  e.b = n_oper > 1 ? args[1] : kNoExpr;
  e.c = n_oper > 2 ? args[2] : kNoExpr;
  return atx::core::Ok(p.ast->add(e));
```

(`split2` has `n_hparams=0`, so this is a no-op for it; the path is exercised by Phase C/D filters. Add `#include <limits>` if absent.)

- [ ] **Step 4: Run to verify PASS.**

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i include/atx/engine/alpha/parser.hpp tests/alpha_member_test.cpp
git add include/atx/engine/alpha/parser.hpp tests/alpha_member_test.cpp
git commit -m "feat(p3d-B5): Member AST + postfix .pin parselet + hparam peel"
```

## Task B6: Typecheck — record TypeInfo + `analyze_member` + record misuse errors

**Files:**
- Modify: `include/atx/engine/alpha/typecheck.hpp`
- Test: `tests/alpha_member_test.cpp` (append)

- [ ] **Step 1: Write the failing test**

```cpp
#include "atx/engine/alpha/typecheck.hpp"

TEST(AlphaMember, PinTypechecks) {
  Library lib;
  auto ast = parse_program("b = split2(close).hi\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value());
  ASSERT_TRUE(an) << an.error().message();
  const ExprId root = ast.value().roots()[0].root;
  EXPECT_EQ(an.value().info(root).shape, Shape::Panel);
  EXPECT_EQ(an.value().info(root).dtype, DType::F64);
}

TEST(AlphaMember, UnknownPinRejected) {
  Library lib;
  auto ast = parse_program("b = split2(close).nope\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value());
  EXPECT_FALSE(an); // no pin 'nope'
}

TEST(AlphaMember, RecordUsedAsScalarRejected) {
  Library lib;
  auto ast = parse_program("b = split2(close) + 1\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value());
  EXPECT_FALSE(an); // record value used in arithmetic
}

TEST(AlphaMember, RecordRootRejected) {
  Library lib;
  auto ast = parse_program("b = split2(close)\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value());
  EXPECT_FALSE(an); // a record cannot be an alpha root (must be pinned)
}
```

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement.**

Extend `TypeInfo`:

```cpp
struct TypeInfo {
  Shape shape{Shape::Scalar};
  DType dtype{DType::F64};
  atx::u16 lookback{};
  bool is_record{false};
  std::span<const PinSig> pins{}; // valid iff is_record
};
```

In `analyze_call`, when `e.op->pins` is non-empty, return a record TypeInfo (shape from the op's `shape_of`, `is_record=true`, `pins=e.op->pins`, lookback as computed). Validate hparams here: for a Call with `e.n_hparams>0`, each `e.hparams[k]` must be finite (reject NaN sentinel → "hyperparameter must be a compile-time constant") and pass op-specific range checks (added per-op in C/D).

Add `analyze_member`:

```cpp
[[nodiscard]] inline atx::core::Result<TypeInfo>
analyze_member(std::span<const TypeInfo> out, const Ast &ast, const Expr &e) {
  const TypeInfo rec = out[e.a];
  if (!rec.is_record) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "member access '.' requires a record-valued operand");
  }
  const std::string_view pin = ast.field_name(e.name_id);
  for (const PinSig &p : rec.pins) {
    if (p.name == pin) {
      return atx::core::Ok(TypeInfo{rec.shape, p.dtype, rec.lookback, false, {}});
    }
  }
  return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                        std::string{"no pin '"} + std::string{pin} + "' on record");
}
```

Wire `Expr::Kind::Member` into `analyze_node`. Reject record misuse: in `analyze_binary`/`analyze_unary`/`analyze_select` and `analyze_call` operand checks, error if any operand `is_record`. In the root loop of `analyze`, error if a root's TypeInfo `is_record`.

- [ ] **Step 4: Run to verify PASS.**

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i include/atx/engine/alpha/typecheck.hpp tests/alpha_member_test.cpp
git add include/atx/engine/alpha/typecheck.hpp tests/alpha_member_test.cpp
git commit -m "feat(p3d-B6): record TypeInfo + analyze_member + record-misuse errors"
```

## Task B7: DAG — multi-output compute node + Member→Pin lowering

**Files:**
- Modify: `include/atx/engine/alpha/dag.hpp`
- Test: `tests/alpha_dag_test.cpp` (append)

- [ ] **Step 1: Write the failing test** — `split2(close).hi` + `split2(close).lo` produce ONE compute node + two Pin nodes.

```cpp
TEST(AlphaDag, SharedComputeOnePinPerProjection) {
  Library lib;
  auto ast = parse_program("a = split2(close).hi\nb = split2(close).lo\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value()); ASSERT_TRUE(an);
  auto dag = build_dag(ast.value(), an.value()); ASSERT_TRUE(dag);
  int compute = 0, pins = 0;
  for (const Node& n : dag.value().nodes()) {
    if (n.op == OpCode::Split2) ++compute;
    if (n.op == OpCode::Pin) ++pins;
  }
  EXPECT_EQ(compute, 1); // single shared compute (CSE on identical args+hparams)
  EXPECT_EQ(pins, 2);    // hi, lo
}
```

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement.**

In `build_dag`, handle `Expr::Kind::Member`: intern (or find) the compute child node, then intern a `Pin` node:

```cpp
    if (e.kind == Expr::Kind::Member) {
      const NodeId rec = ast_to_node[e.a];
      // pin index = position of the pin name in the compute op's pin table.
      const TypeInfo &cti = analysis.info(static_cast<ExprId>(e.a));
      atx::u32 pin_idx = 0;
      const std::string_view pin = ast.field_name(e.name_id);
      for (atx::usize k = 0; k < cti.pins.size(); ++k) {
        if (cti.pins[k].name == pin) { pin_idx = static_cast<atx::u32>(k); break; }
      }
      Node pn;
      pn.op = OpCode::Pin;
      pn.shape = ti.shape; pn.dtype = ti.dtype; pn.lookback = ti.lookback;
      pn.in = {rec, kNoNode, kNoNode};
      pn.param = pin_idx;
      NodeKey pk{OpCode::Pin, pin_idx, {rec, kNoNode, kNoNode}, {}};
      const atx::usize before = dag.nodes().size();
      const NodeId pid = dag.intern(pk, pn);
      ast_to_node[i] = pid;
      if (dag.nodes().size() != before) {
        dag.add_edge_refcount(rec);
      }
      continue; // skip the generic node-build path for this Expr
    }
```

For a multi-output Call, set `proto.n_out = e.op->pins.size()` and copy `proto.hparams = e.hparams` in `make_node` (extend `make_node` signature to take hparams + n_out, or set after construction). Fold `imm_bits` into the Call's `NodeKey`:

```cpp
    std::array<atx::u64, 2> imm_bits{std::bit_cast<atx::u64>(e.hparams[0]),
                                     std::bit_cast<atx::u64>(e.hparams[1])};
    NodeKey key{emit_op, key_param, emit_kids, imm_bits};
```

(For non-Call/non-filter nodes `hparams` is `{0,0}` → `imm_bits {0,0}`, unchanged keys.)

- [ ] **Step 4: Run to verify PASS.**

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i include/atx/engine/alpha/dag.hpp tests/alpha_dag_test.cpp
git add include/atx/engine/alpha/dag.hpp tests/alpha_dag_test.cpp
git commit -m "feat(p3d-B7): DAG multi-output compute + Member->Pin lowering + imm_bits key"
```

## Task B8: Linearize — block allocation for multi-output + Pin emit + block Free

**Files:**
- Modify: `include/atx/engine/alpha/bytecode.hpp::linearize`
- Test: `tests/alpha_bytecode_test.cpp` (append)

- [ ] **Step 1: Write the failing test** — program with split2 pins compiles; peak slots sane; one Split2 instr.

```cpp
TEST(AlphaBytecode, MultiOutputCompilesOneComputeInstr) {
  Library lib;
  auto ast = parse_program("a = split2(close).hi\nb = split2(close).lo\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value()); ASSERT_TRUE(an);
  auto prog = compile(ast.value(), an.value()); ASSERT_TRUE(prog) << prog.error().message();
  int split = 0, pin = 0;
  for (const Instr& in : prog.value().code) {
    if (in.op == OpCode::Split2) ++split;
    if (in.op == OpCode::Pin) ++pin;
  }
  EXPECT_EQ(split, 1);
  EXPECT_EQ(pin, 2);
}
```

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement.**

In `linearize`, when emitting a node with `n.n_out > 1`, acquire a block:

```cpp
    const atx::u8 nout = n.n_out;
    const SlotId dst = (nout > 1) ? pool.acquire_block(nout) : pool.acquire();
    slot[i] = dst;
    Instr instr;
    instr.op = n.op;
    instr.dst = dst;
    instr.n_out = nout;
    // … existing src wiring, param, Const imm[0] …
```

For `Pin` nodes, `src[0]` = the compute's slot (already wired by the generic child loop since Pin's child is the compute). `param` = pin index (copied from `n.param`).

Block Free: when a node's `remaining` hits zero, release the whole block. Modify `retire_consumer` (or the Free emission) to use `release_block(slot[child], nodes[child].n_out)` and emit a Free carrying `n_out` so the VM releases the block:

```cpp
  // in retire_consumer signature add `const std::vector<Node>& nodes` or pass n_out:
  Instr fr; fr.op = OpCode::Free; fr.dst = slot[child]; fr.n_out = nodes[child].n_out;
  code.push_back(fr);
  pool.release_block(slot[child], nodes[child].n_out);
```

(Thread `nodes` into `retire_consumer`, or compute `n_out` at the call sites.) The VM's `Free` handler becomes `pool_.release_block(in.dst, in.n_out)`.

- [ ] **Step 4: Run to verify PASS.**

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i include/atx/engine/alpha/bytecode.hpp tests/alpha_bytecode_test.cpp
git add include/atx/engine/alpha/bytecode.hpp tests/alpha_bytecode_test.cpp
git commit -m "feat(p3d-B8): linearize multi-output block alloc + Pin emit + block Free"
```

## Task B9: VM + oracle — `Split2` compute + `Pin` projection; differential test

**Files:**
- Modify: `include/atx/engine/alpha/vm.hpp` (SlotPool block-aware column access already fine; add dispatch for Split2 + Pin), `include/atx/engine/alpha/oracle.hpp`
- Test: `tests/alpha_differential_test.cpp` (append) or `tests/alpha_multioutput_test.cpp` (create)

> **Slot/column note:** `SlotPool::column(slot)` returns the `cells`-wide span for `slot`. A K-block occupies slots `dst..dst+K-1`; column `dst+k` is the k-th output. The VM's `dst_col(in)` returns `column(in.dst)` = output 0; write output k via `pool_.column(in.dst + k)`. Confirm `SlotPool` backing store is one contiguous buffer indexed by slot — it is (uniform `cells_per_slot`). Add a helper `out_col(in, k)` = `pool_.column(in.dst + k)`.

- [ ] **Step 1: Write the failing differential + value test**

```cpp
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/alpha/oracle.hpp"

TEST(AlphaMultiOutput, Split2ValuesAndDifferential) {
  Library lib;
  auto ast = parse_program("a = split2(close).hi\nb = split2(close).lo\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value()); ASSERT_TRUE(an);
  auto prog = compile(ast.value(), an.value()); ASSERT_TRUE(prog);
  auto panel = /* 2 dates x 2 instruments, close known values via helper */;
  Engine eng(panel);
  auto vm = eng.evaluate(prog.value()); ASSERT_TRUE(vm);
  auto orc = evaluate_reference(prog.value(), panel); ASSERT_TRUE(orc);
  // hi == close, lo == -close; and VM == oracle bit-for-bit.
  for (size_t i = 0; i < vm.value().alphas[0].values.size(); ++i) {
    EXPECT_EQ(vm.value().alphas[0].values[i], orc.value().alphas[0].values[i]); // hi
    EXPECT_EQ(vm.value().alphas[1].values[i], orc.value().alphas[1].values[i]); // lo
    EXPECT_EQ(vm.value().alphas[1].values[i], -vm.value().alphas[0].values[i]); // lo == -hi
  }
}
```

- [ ] **Step 2: Run to verify FAIL** (NotImplemented for Split2/Pin).

- [ ] **Step 3: Implement.**

VM `dispatch`: add `case OpCode::Split2: return eval_split2(in, cells);` and handle `Pin` in `evaluate()` alongside StoreAlpha/Free (Pin is a copy, not in `dispatch`), OR in dispatch. Simpler: handle Pin in `evaluate()`:

```cpp
      if (in.op == OpCode::Pin) {
        const std::span<const atx::f64> src = pool_.column(in.src[0] + in.param);
        const std::span<atx::f64> dst = pool_.column(in.dst);
        for (atx::usize i = 0; i < cells; ++i) dst[i] = src[i];
        (void)pool_.acquire(); // Pin produces a single-slot value (liveness parity)
        continue;
      }
```

`eval_split2` (test op: hi=x, lo=-x):

```cpp
  [[nodiscard]] atx::core::Status eval_split2(const Instr& in, atx::usize cells) {
    const std::span<const atx::f64> x = src_col(in, 0);
    const std::span<atx::f64> hi = pool_.column(in.dst + 0);
    const std::span<atx::f64> lo = pool_.column(in.dst + 1);
    for (atx::usize i = 0; i < cells; ++i) { hi[i] = x[i]; lo[i] = -x[i]; }
    return atx::core::Ok();
  }
```

> **Liveness parity caution:** the existing `evaluate()` calls `pool_.acquire()` once per dispatched compute instr to keep the pool's live-count assert in step with the linearizer. A multi-output compute consumed `acquire_block(n_out)` slots in the linearizer; mirror that here by acquiring `n_out` (e.g. loop `for k<in.n_out: pool_.acquire();`) for Split2-class ops, and one `acquire()` for Pin. Verify against the actual `SlotPool` assert semantics in vm.hpp; adjust so warm re-eval allocates nothing.

Oracle: mirror `Split2` and `Pin` in `evaluate_reference`'s dispatch with the identical scalar policy (hi=x, lo=-x; Pin copies column `src[0]+param`).

- [ ] **Step 4: Run to verify PASS.**

- [ ] **Step 5: Full alpha + differential suite green; clang-format + commit**

```bash
ctest --test-dir build -R "Alpha|Vm|Oracle|Differential|MultiOutput" --output-on-failure
clang-format -i include/atx/engine/alpha/vm.hpp include/atx/engine/alpha/oracle.hpp tests/alpha_multioutput_test.cpp
git add include/atx/engine/alpha/vm.hpp include/atx/engine/alpha/oracle.hpp tests/alpha_multioutput_test.cpp
git commit -m "feat(p3d-B9): VM+oracle Split2 compute + Pin projection (differential green)"
```

**Phase B gate:** multi-output IR proven end-to-end on `split2`. `split2` stays as a permanent test op (cheap, guards the IR).

---

# PHASE C — Strided recurrence state + `kalman_level` + `ou_filter`

**Outcome:** Two single-output recurrence ops land. Recurrence state generalized to a strided per-instrument buffer.

## Task C1: Generalize recurrence state to strided buffer

**Files:**
- Modify: `include/atx/engine/alpha/vm.hpp` (`state_` usage), `include/atx/engine/alpha/oracle.hpp`
- Test: covered by existing `trade_when`/`hump` differential tests (must stay green).

- [ ] **Step 1:** Add a stride accessor. Keep `state_` a `std::vector<f64>`; size it `instruments * kMaxStride` where `kMaxStride` = max over recurrence ops (set to 5 for kalman_reg; 1 for others). Index `state_[j * stride + s]`. For existing `trade_when`/`hump` (stride 1) the indexing `state_[j*1+0]` is identical to today.

```cpp
inline constexpr atx::usize kMaxRecurStride = 5; // kalman_reg needs 5; others <=2
```

Resize: `if (instruments * kMaxRecurStride > state_.size()) state_.resize(instruments * kMaxRecurStride);`

- [ ] **Step 2:** Refactor `eval_recurrence` (`trade_when`/`hump`) to use `state_[j * 1 + 0]` form (semantically identical). Run existing recurrence differential tests.

Run: `ctest --test-dir build -R "Recurrence|TradeWhen|Hump|Differential" --output-on-failure`
Expected: PASS (no behavior change).

- [ ] **Step 3: commit**

```bash
clang-format -i include/atx/engine/alpha/vm.hpp include/atx/engine/alpha/oracle.hpp
git add include/atx/engine/alpha/vm.hpp include/atx/engine/alpha/oracle.hpp
git commit -m "refactor(p3d-C1): strided recurrence state buffer (behavior-preserving)"
```

## Task C2: `kalman_level` step kernel in `state_ops.hpp`

**Files:**
- Modify: `include/atx/engine/alpha/state_ops.hpp`
- Test: `tests/alpha_state_ops_test.cpp` (append)

- [ ] **Step 1: Write the failing unit test** (pure kernel, no VM) — checks one step against hand math.

```cpp
#include "atx/engine/alpha/state_ops.hpp"
using namespace atx::engine::alpha::detail;

TEST(KalmanLevelStep, SeedAndOneUpdate) {
  // seed: x=z0, P=R. Q=0.1, R=1.0, z0=2.0 -> out 2.0
  KalmanLevelState s{};
  bool seeded = false;
  double o0 = kalman_level_step(s, seeded, 2.0, 0.1, 1.0); // seeds
  EXPECT_DOUBLE_EQ(o0, 2.0);
  EXPECT_TRUE(seeded);
  // step z1=4.0: P-=P+Q=1.1; K=1.1/2.1; x=2 + K*(4-2)
  double K = 1.1 / 2.1;
  double expect = 2.0 + K * 2.0;
  double o1 = kalman_level_step(s, seeded, 4.0, 0.1, 1.0);
  EXPECT_DOUBLE_EQ(o1, expect);
}

TEST(KalmanLevelStep, NanObsCarriesEstimate) {
  KalmanLevelState s{}; bool seeded = false;
  kalman_level_step(s, seeded, 2.0, 0.1, 1.0);
  double x_before = s.x;
  double o = kalman_level_step(s, seeded, std::numeric_limits<double>::quiet_NaN(), 0.1, 1.0);
  EXPECT_DOUBLE_EQ(o, x_before); // estimate carried, no update
}
```

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement** in `state_ops.hpp`:

```cpp
struct KalmanLevelState { atx::f64 x{0.0}; atx::f64 P{0.0}; };

// One step of the scalar local-level (random-walk + noise) filter for one
// instrument. `seeded` is false until the first finite observation. On the seed
// observation x=z, P=R, output z. Subsequent: predict P+=Q; if z finite, Kalman
// update; if z NaN, carry x with P+=Q. Returns the filtered level (NaN before seed).
// SAFETY: reads only the prior state `s` and the date-t observation `z`.
[[nodiscard]] inline atx::f64 kalman_level_step(KalmanLevelState &s, bool &seeded, atx::f64 z,
                                                atx::f64 Q, atx::f64 R) noexcept {
  if (!seeded) {
    if (state_is_nan(z)) {
      return kStateNaN; // still unseeded
    }
    s.x = z;
    s.P = R;
    seeded = true;
    return s.x;
  }
  s.P += Q; // predict
  if (!state_is_nan(z)) {
    const atx::f64 K = s.P / (s.P + R);
    s.x += K * (z - s.x);
    s.P = (1.0 - K) * s.P;
  }
  return s.x;
}
```

- [ ] **Step 4: Run to verify PASS.**

- [ ] **Step 5: clang-format + commit**

```bash
clang-format -i include/atx/engine/alpha/state_ops.hpp tests/alpha_state_ops_test.cpp
git add include/atx/engine/alpha/state_ops.hpp tests/alpha_state_ops_test.cpp
git commit -m "feat(p3d-C2): kalman_level scalar local-level step kernel"
```

## Task C3: `ou_filter` step kernel

**Files:**
- Modify: `include/atx/engine/alpha/state_ops.hpp`
- Test: `tests/alpha_state_ops_test.cpp` (append)

- [ ] **Step 1: Write the failing test**

```cpp
TEST(OuFilterStep, PullsTowardMu) {
  // theta=ln(2) -> phi=0.5. mu=10. seed x0=2 -> out 2.
  double theta = std::log(2.0), mu = 10.0;
  atx::f64 xhat = 0.0; bool seeded = false;
  double o0 = ou_filter_step(xhat, seeded, 2.0, theta, mu);
  EXPECT_DOUBLE_EQ(o0, 2.0);
  // step: xhat = mu + 0.5*(2-10) = 10 + 0.5*(-8) = 6
  double o1 = ou_filter_step(xhat, seeded, /*x ignored after seed*/ 99.0, theta, mu);
  EXPECT_DOUBLE_EQ(o1, 6.0);
}
```

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement**

```cpp
// One step of the OU AR(1) pull-to-mean smoother. phi=exp(-theta). Seeds on the
// first finite x (xhat=x). Subsequent steps pull toward mu independent of the new
// observation: xhat = mu + phi*(xhat-mu). Returns xhat (NaN before seed).
[[nodiscard]] inline atx::f64 ou_filter_step(atx::f64 &xhat, bool &seeded, atx::f64 x,
                                             atx::f64 theta, atx::f64 mu) noexcept {
  if (!seeded) {
    if (state_is_nan(x)) {
      return kStateNaN;
    }
    xhat = x;
    seeded = true;
    return xhat;
  }
  const atx::f64 phi = std::exp(-theta);
  xhat = mu + phi * (xhat - mu);
  return xhat;
}
```

- [ ] **Step 4: Run PASS. Step 5: commit**

```bash
clang-format -i include/atx/engine/alpha/state_ops.hpp tests/alpha_state_ops_test.cpp
git add include/atx/engine/alpha/state_ops.hpp tests/alpha_state_ops_test.cpp
git commit -m "feat(p3d-C3): ou_filter AR(1) pull-to-mean step kernel"
```

## Task C4: Registry rows + typecheck for `kalman_level` and `ou_filter`

**Files:**
- Modify: `include/atx/engine/alpha/registry.hpp` (OpCodes `KalmanLevel`, `OuFilter`; rows), `include/atx/engine/alpha/typecheck.hpp` (recurrence family + hparam range checks)
- Test: `tests/alpha_typecheck_test.cpp` (append)

- [ ] **Step 1: Write the failing test**

```cpp
TEST(AlphaTypecheck, KalmanLevelTypes) {
  Library lib;
  auto ast = parse_program("a = kalman_level(close, 0.1, 1.0)\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value()); ASSERT_TRUE(an) << an.error().message();
  EXPECT_EQ(an.value().info(ast.value().roots()[0].root).shape, Shape::Panel);
}
TEST(AlphaTypecheck, KalmanLevelRejectsNonConstHparam) {
  Library lib;
  auto ast = parse_program("a = kalman_level(close, close, 1.0)\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value());
  EXPECT_FALSE(an); // Q must be a constant literal
}
TEST(AlphaTypecheck, OuFilterTypes) {
  Library lib;
  auto ast = parse_program("a = ou_filter(close, 0.05, 100.0)\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value()); ASSERT_TRUE(an) << an.error().message();
}
```

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement.**

Registry OpCodes: add `KalmanLevel`, `OuFilter` (near recurrence section). Rows (`n_hparams=2`, single output, Panel shape, lookback handled by recurrence classification):

```cpp
      {"kalman_level", 3, 3, OpCode::KalmanLevel, DType::F64, true, {}, &shape_panel, 2, {}},
      {"ou_filter",    3, 3, OpCode::OuFilter,    DType::F64, true, {}, &shape_panel, 2, {}},
```

Typecheck: add a `is_recurrence(op)` predicate (returns true for `TradeWhen`, `Hump`, `KalmanLevel`, `OuFilter`, and later `KalmanReg`), used so recurrence ops require a non-scalar primary operand (like Ts) but add **0** window lookback (lookback = max child). In `analyze_call`, validate `e.n_hparams` hparams are finite (already in B6) and per-op: `kalman_level` needs `Q>=0, R>0`; `ou_filter` needs `theta>=0` and finite `mu`. Add these range checks keyed on `op`.

Update every exhaustive `OpCode` switch for the two new codes.

- [ ] **Step 4: Run PASS. Step 5: commit**

```bash
clang-format -i include/atx/engine/alpha/registry.hpp include/atx/engine/alpha/typecheck.hpp tests/alpha_typecheck_test.cpp
git add include/atx/engine/alpha/registry.hpp include/atx/engine/alpha/typecheck.hpp tests/alpha_typecheck_test.cpp
git commit -m "feat(p3d-C4): kalman_level + ou_filter registry rows + typecheck"
```

## Task C5: VM + oracle `eval_recurrence` branches for `kalman_level` + `ou_filter`; differential

**Files:**
- Modify: `include/atx/engine/alpha/vm.hpp`, `include/atx/engine/alpha/oracle.hpp`
- Test: `tests/alpha_recurrence_test.cpp` (append) + differential

- [ ] **Step 1: Write the failing differential + value test**

```cpp
TEST(AlphaKalmanLevel, VmMatchesOracleAndHandMath) {
  Library lib;
  auto ast = parse_program("a = kalman_level(close, 0.1, 1.0)\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value()); ASSERT_TRUE(an);
  auto prog = compile(ast.value(), an.value()); ASSERT_TRUE(prog);
  auto panel = /* dates=[t0,t1], 1 instrument, close=[2.0,4.0] */;
  Engine eng(panel);
  auto vm = eng.evaluate(prog.value()); ASSERT_TRUE(vm);
  auto orc = evaluate_reference(prog.value(), panel); ASSERT_TRUE(orc);
  EXPECT_DOUBLE_EQ(vm.value().alphas[0].values[0], 2.0);
  double K = 1.1 / 2.1; double t1 = 2.0 + K * 2.0;
  EXPECT_DOUBLE_EQ(vm.value().alphas[0].values[1], t1);
  for (size_t i = 0; i < vm.value().alphas[0].values.size(); ++i)
    EXPECT_EQ(vm.value().alphas[0].values[i], orc.value().alphas[0].values[i]);
}
```

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement.** In `vm.hpp::dispatch`, route `KalmanLevel`/`OuFilter` to `eval_recurrence`. In `eval_recurrence`, add branches reading hyperparams from `in.imm[0]`/`in.imm[1]`, per-instrument scan over the strided state (stride 1 for both: store `x` in `state_[j*kMaxRecurStride+0]`, and for kalman_level store `P` in `[...+1]`; a per-instrument `seeded` flag — use a parallel `std::vector<unsigned char> seeded_` sized `instruments`, cleared per call, or encode unseeded as NaN sentinel in state to avoid a second buffer). Recommended: encode unseeded via a `seeded_` byte buffer (clear at scan start). Mirror in oracle with an independent restatement.

```cpp
    if (in.op == OpCode::KalmanLevel) {
      const std::span<const atx::f64> z = src_col(in, 0);
      const atx::f64 Q = in.imm[0], R = in.imm[1];
      for (atx::usize j = 0; j < instruments; ++j) {
        detail::KalmanLevelState s{};
        bool seeded = false;
        for (atx::usize t = 0; t < dates; ++t) {
          const atx::usize i = t * instruments + j;
          out[i] = detail::kalman_level_step(s, seeded, z[i], Q, R);
        }
      }
      return atx::core::Ok();
    }
```

(Per-instrument local `s`/`seeded` — the scan is column-by-column, so no cross-instrument state buffer is even needed; the strided `state_` is only needed if scanning date-major. Since the kernel scans instrument-outer/date-inner, locals suffice and allocate nothing. This SIMPLIFIES C1 — keep C1's strided buffer only if a date-outer scan is required for SIMD; otherwise locals are cleaner. Decide in C1: prefer instrument-outer locals.) Mirror `OuFilter` similarly. Oracle: identical restatement.

> **Note for C1 revisit:** with instrument-outer scanning, recurrence state is a stack local per column — no `state_` growth needed at all. If you adopt this, C1 becomes a no-op/removed and `trade_when`/`hump` may be left as-is. Confirm the existing ops' scan order; align the new ops to it.

- [ ] **Step 4: Run PASS + full differential suite. Step 5: commit**

```bash
ctest --test-dir build -R "Kalman|OuFilter|Recurrence|Differential" --output-on-failure
clang-format -i include/atx/engine/alpha/vm.hpp include/atx/engine/alpha/oracle.hpp tests/alpha_recurrence_test.cpp
git add include/atx/engine/alpha/vm.hpp include/atx/engine/alpha/oracle.hpp tests/alpha_recurrence_test.cpp
git commit -m "feat(p3d-C5): VM+oracle kalman_level + ou_filter recurrence (differential green)"
```

---

# PHASE D — Chan 2-state Kalman regression (record {alpha, beta, resid})

**Outcome:** `kalman(y, x, delta, R)` record op; `.alpha`/`.beta`/`.resid` pins; single shared scan.

## Task D1: `kalman_reg_step` kernel (2-state, 2×2 covariance)

**Files:**
- Modify: `include/atx/engine/alpha/state_ops.hpp`
- Test: `tests/alpha_state_ops_test.cpp` (append)

- [ ] **Step 1: Write the failing test** — one step vs hand-computed Chan update.

```cpp
TEST(KalmanRegStep, SeedAndOneStep) {
  // seed t0: beta=[0,0], P=I. y0=1, x0=2, delta=0.5 -> W=(0.5/0.5)I=I, R=1.
  KalmanRegState s{}; bool seeded=false;
  KalmanRegOut o0 = kalman_reg_step(s, seeded, 1.0, 2.0, 0.5, 1.0);
  // At seed we still run the update from prior beta=[0,0], P=I (pinned policy).
  // P- = P + W = 2I. F=[1,2]. yhat=0. e=1. Q = F P- F^T + R = 2*(1)+2*(4)+1 = 2+8+1=11.
  // Kg = P- F^T / Q = [2*1, 2*2]/11 = [2/11, 4/11].
  // beta = [0,0] + Kg*e = [2/11, 4/11]. resid = e/sqrt(Q)=1/sqrt(11).
  EXPECT_NEAR(o0.alpha, 2.0/11.0, 1e-12);
  EXPECT_NEAR(o0.beta, 4.0/11.0, 1e-12);
  EXPECT_NEAR(o0.resid, 1.0/std::sqrt(11.0), 1e-12);
}
```

> **Seed policy decision (pinned):** the seed date runs a full Chan update from the prior `β=[0,0], P=I` (i.e. t=0 is not special-cased except for unseeded NaN handling). This matches a standard pykalman/Chan recursion where the prior is the diffuse `(β0,P0)` and every observed date updates. The known-value fixture in D4 must be generated with the SAME convention.

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement** in `state_ops.hpp`:

```cpp
struct KalmanRegState {
  atx::f64 a{0.0};   // intercept alpha
  atx::f64 b{0.0};   // slope beta
  atx::f64 P00{1.0}; // covariance (diffuse identity prior)
  atx::f64 P01{0.0};
  atx::f64 P11{1.0};
};
struct KalmanRegOut { atx::f64 alpha; atx::f64 beta; atx::f64 resid; };

// One step of the Chan 2-state time-varying regression of y on x for one
// instrument. delta in (0,1) sets process covariance W=(delta/(1-delta))I; R is
// observation noise. Seeds on the first finite (y,x). Incomplete obs -> predict-
// only (P+=W), outputs NaN. SAFETY: reads only prior state `s` and date-t (y,x).
[[nodiscard]] inline KalmanRegOut kalman_reg_step(KalmanRegState &s, bool &seeded, atx::f64 y,
                                                  atx::f64 x, atx::f64 delta, atx::f64 R) noexcept {
  const atx::f64 w = delta / (1.0 - delta); // W = w * I2
  if (state_is_nan(y) || state_is_nan(x)) {
    if (seeded) { s.P00 += w; s.P11 += w; } // predict-only
    return {kStateNaN, kStateNaN, kStateNaN};
  }
  // predict covariance P- = P + W
  const atx::f64 P00 = s.P00 + w, P01 = s.P01, P11 = s.P11 + w;
  // F = [1, x]; yhat = a + b*x; innovation e
  const atx::f64 yhat = s.a + s.b * x;
  const atx::f64 e = y - yhat;
  // P- F^T = [P00 + P01*x, P01 + P11*x]
  const atx::f64 pf0 = P00 + P01 * x;
  const atx::f64 pf1 = P01 + P11 * x;
  // Q = F P- F^T + R = pf0 + x*pf1 + R
  const atx::f64 Q = pf0 + x * pf1 + R;
  const atx::f64 k0 = pf0 / Q, k1 = pf1 / Q; // gain
  s.a += k0 * e;
  s.b += k1 * e;
  // P = P- - Kg (F P-);  F P- = [pf0, pf1] (row)
  s.P00 = P00 - k0 * pf0;
  s.P01 = P01 - k0 * pf1;
  s.P11 = P11 - k1 * pf1;
  seeded = true;
  return {s.a, s.b, e / std::sqrt(Q)};
}
```

- [ ] **Step 4: Run PASS. Step 5: commit**

```bash
clang-format -i include/atx/engine/alpha/state_ops.hpp tests/alpha_state_ops_test.cpp
git add include/atx/engine/alpha/state_ops.hpp tests/alpha_state_ops_test.cpp
git commit -m "feat(p3d-D1): Chan 2-state kalman_reg step kernel"
```

## Task D2: Registry — `kalman` record op + pins; typecheck

**Files:**
- Modify: `include/atx/engine/alpha/registry.hpp` (OpCode `KalmanReg`, pin table, row), `include/atx/engine/alpha/typecheck.hpp`
- Test: `tests/alpha_member_test.cpp` (append)

- [ ] **Step 1: Write the failing test**

```cpp
TEST(AlphaKalmanReg, PinsResolve) {
  Library lib;
  auto ast = parse_program("b = kalman(close, open, 0.0001, 0.001).beta\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value()); ASSERT_TRUE(an) << an.error().message();
  EXPECT_EQ(an.value().info(ast.value().roots()[0].root).shape, Shape::Panel);
}
TEST(AlphaKalmanReg, BadPinRejected) {
  Library lib;
  auto ast = parse_program("b = kalman(close, open, 0.0001, 0.001).gamma\n", lib); ASSERT_TRUE(ast);
  EXPECT_FALSE(analyze(ast.value()));
}
```

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement.** OpCode `KalmanReg`. Pin table + row:

```cpp
inline constexpr std::array<PinSig, 3> kKalmanRegPins = {
    {{"alpha", DType::F64}, {"beta", DType::F64}, {"resid", DType::F64}}};
// row (arity 4: y, x, delta, R; 2 operands + 2 hparams; record of 3 pins):
      {"kalman", 4, 4, OpCode::KalmanReg, DType::F64, true, {}, &shape_panel, 2,
       std::span<const PinSig>{kKalmanRegPins}},
```

Typecheck: add `KalmanReg` to `is_recurrence`; require operands `y`,`x` non-scalar F64; hparams `delta∈(0,1)`, `R>0`. The record TypeInfo path (B6) handles pins. lookback = max(child) (recurrence adds 0).

Update exhaustive `OpCode` switches.

- [ ] **Step 4: Run PASS. Step 5: commit**

```bash
clang-format -i include/atx/engine/alpha/registry.hpp include/atx/engine/alpha/typecheck.hpp tests/alpha_member_test.cpp
git add include/atx/engine/alpha/registry.hpp include/atx/engine/alpha/typecheck.hpp tests/alpha_member_test.cpp
git commit -m "feat(p3d-D2): kalman record op (alpha/beta/resid pins) + typecheck"
```

## Task D3: VM + oracle `KalmanReg` multi-output recurrence

**Files:**
- Modify: `include/atx/engine/alpha/vm.hpp`, `include/atx/engine/alpha/oracle.hpp`
- Test: `tests/alpha_recurrence_test.cpp` (append) + differential

- [ ] **Step 1: Write the failing differential test** — beta/alpha/resid VM==oracle, single compute instr.

```cpp
TEST(AlphaKalmanReg, VmMatchesOracle) {
  Library lib;
  auto ast = parse_program(
    "al = kalman(close, open, 0.0001, 0.001).alpha\n"
    "be = kalman(close, open, 0.0001, 0.001).beta\n"
    "re = kalman(close, open, 0.0001, 0.001).resid\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value()); ASSERT_TRUE(an);
  auto prog = compile(ast.value(), an.value()); ASSERT_TRUE(prog);
  int compute = 0; for (auto& in : prog.value().code) if (in.op==OpCode::KalmanReg) ++compute;
  EXPECT_EQ(compute, 1); // shared scan
  auto panel = /* dates=4, instruments=2, close/open random-ish known */;
  Engine eng(panel);
  auto vm = eng.evaluate(prog.value()); ASSERT_TRUE(vm);
  auto orc = evaluate_reference(prog.value(), panel); ASSERT_TRUE(orc);
  for (size_t a=0;a<3;++a) for (size_t i=0;i<vm.value().alphas[a].values.size();++i)
    EXPECT_EQ(vm.value().alphas[a].values[i], orc.value().alphas[a].values[i]);
}
```

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement.** `KalmanReg` writes 3 output columns. In `eval_recurrence` (or a dedicated `eval_kalman_reg`), per instrument run `kalman_reg_step`, writing `out_col(in,0)=alpha`, `out_col(in,1)=beta`, `out_col(in,2)=resid` (where `out_col(in,k)=pool_.column(in.dst+k)`):

```cpp
    if (in.op == OpCode::KalmanReg) {
      const std::span<const atx::f64> y = src_col(in, 0);
      const std::span<const atx::f64> x = src_col(in, 1);
      const atx::f64 delta = in.imm[0], R = in.imm[1];
      const std::span<atx::f64> oa = pool_.column(in.dst + 0);
      const std::span<atx::f64> ob = pool_.column(in.dst + 1);
      const std::span<atx::f64> orr = pool_.column(in.dst + 2);
      for (atx::usize j = 0; j < instruments; ++j) {
        detail::KalmanRegState s{}; bool seeded = false;
        for (atx::usize t = 0; t < dates; ++t) {
          const atx::usize i = t * instruments + j;
          const detail::KalmanRegOut o = detail::kalman_reg_step(s, seeded, y[i], x[i], delta, R);
          oa[i] = o.alpha; ob[i] = o.beta; orr[i] = o.resid;
        }
      }
      return atx::core::Ok();
    }
```

Route `KalmanReg` in `dispatch` to this. Mirror in oracle independently. Ensure liveness-parity acquire (`n_out=3` slots) as in B9.

- [ ] **Step 4: Run PASS + full differential. Step 5: commit**

```bash
ctest --test-dir build -R "KalmanReg|Recurrence|Differential|MultiOutput" --output-on-failure
clang-format -i include/atx/engine/alpha/vm.hpp include/atx/engine/alpha/oracle.hpp tests/alpha_recurrence_test.cpp
git add include/atx/engine/alpha/vm.hpp include/atx/engine/alpha/oracle.hpp tests/alpha_recurrence_test.cpp
git commit -m "feat(p3d-D3): VM+oracle Chan kalman multi-output recurrence (differential green)"
```

## Task D4: Known-value test vs Python reference

**Files:**
- Create: `tests/fixtures/kalman_reg_reference.hpp` (generated constants) + `scripts/gen_kalman_ref.py`
- Test: `tests/alpha_recurrence_test.cpp` (append)

- [ ] **Step 1: Write `scripts/gen_kalman_ref.py`** — a self-contained numpy implementation of the SAME recursion (seed β=[0,0], P=I, W=(δ/(1−δ))I, every observed date updates), printing a C++ header of input series + expected alpha/beta/resid arrays. Keep it tiny (one instrument, ~8 dates). Document the exact recursion in comments so it provably matches `kalman_reg_step`.

- [ ] **Step 2: Generate the fixture**

Run: `python scripts/gen_kalman_ref.py > tests/fixtures/kalman_reg_reference.hpp`

- [ ] **Step 3: Write the test** consuming the fixture: build a 1-instrument panel from the fixture inputs, evaluate `kalman(...).beta` etc., `EXPECT_NEAR` to the fixture arrays (tol 1e-9).

- [ ] **Step 4: Run PASS.** If mismatch, the C++ and Python recursions disagree — reconcile the seed/update convention (D1 note) until bit-close.

- [ ] **Step 5: commit**

```bash
git add scripts/gen_kalman_ref.py tests/fixtures/kalman_reg_reference.hpp tests/alpha_recurrence_test.cpp
git commit -m "test(p3d-D4): Chan kalman known-value fixture vs numpy reference"
```

---

# PHASE E — OU rolling-fit family

**Outcome:** `ou_theta`, `ou_halflife`, `ou_mean`, `ou_zscore` — windowed AR(1) OLS ops.

## Task E1: Rolling AR(1) OLS kernel in `ts_ops.hpp`

**Files:**
- Modify: `include/atx/engine/alpha/ts_ops.hpp`
- Test: `tests/alpha_ts_ops_test.cpp` (append)

- [ ] **Step 1: Write the failing test** — fit AR(1) on a known window, check a,b,resid-std.

```cpp
// Over window x = [1,2,3,4,5] (lagged pairs (1,2),(2,3),(3,4),(4,5)): perfect line
// x[s]=x[s-1]+1 -> b=1, a=1, residuals 0. theta=-ln(1)=0 -> NaN (b not in (0,1)).
TEST(OuAr1Fit, PerfectLineGivesUnitSlope) {
  std::array<double,5> w{1,2,3,4,5};
  OuAr1Fit f = ou_ar1_fit(std::span<const double>(w));
  EXPECT_NEAR(f.b, 1.0, 1e-12);
  EXPECT_NEAR(f.a, 1.0, 1e-12);
  EXPECT_NEAR(f.resid_std, 0.0, 1e-12);
}
```

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement** a window AR(1) OLS helper returning `{a, b, resid_std, n}` over the lagged pairs `(x[s-1] → x[s])` for `s` in the window, NaN-pairs excluded (a pair is valid iff both finite). Use the standard OLS closed form. Mirror the NaN/min-period handling of the existing `resid`/`slope` kernels (look at those in `ts_ops.hpp` for the exact valid-count and degenerate-variance policy, and match it).

```cpp
struct OuAr1Fit { atx::f64 a{kTsNaN}; atx::f64 b{kTsNaN}; atx::f64 resid_std{kTsNaN}; atx::usize n{0}; };

// OLS of x[s] on x[s-1] over a trailing window (the caller passes the window span,
// oldest..newest). Pairs with a NaN endpoint are skipped. Returns NaN fields when
// fewer than 2 valid pairs or zero predictor variance. (NaN/min-period policy
// MATCHES ts_ops' existing slope/resid kernels — verify against them.)
[[nodiscard]] inline OuAr1Fit ou_ar1_fit(std::span<const atx::f64> w) noexcept {
  atx::f64 sx = 0, sy = 0, sxx = 0, sxy = 0; atx::usize n = 0;
  for (atx::usize s = 1; s < w.size(); ++s) {
    const atx::f64 xp = w[s - 1], yc = w[s];
    if (ts_is_nan(xp) || ts_is_nan(yc)) continue;
    sx += xp; sy += yc; sxx += xp * xp; sxy += xp * yc; ++n;
  }
  if (n < 2) return {};
  const atx::f64 dn = static_cast<atx::f64>(n);
  const atx::f64 denom = sxx - sx * sx / dn;
  if (denom == 0.0) return {};
  const atx::f64 b = (sxy - sx * sy / dn) / denom;
  const atx::f64 a = (sy - b * sx) / dn;
  // residual std (population over the n pairs)
  atx::f64 ss = 0;
  for (atx::usize s = 1; s < w.size(); ++s) {
    const atx::f64 xp = w[s - 1], yc = w[s];
    if (ts_is_nan(xp) || ts_is_nan(yc)) continue;
    const atx::f64 r = yc - (a + b * xp); ss += r * r;
  }
  return {a, b, std::sqrt(ss / dn), n};
}
```

- [ ] **Step 4: Run PASS. Step 5: commit**

```bash
clang-format -i include/atx/engine/alpha/ts_ops.hpp tests/alpha_ts_ops_test.cpp
git add include/atx/engine/alpha/ts_ops.hpp tests/alpha_ts_ops_test.cpp
git commit -m "feat(p3d-E1): rolling AR(1) OLS fit helper"
```

## Task E2: Per-cell OU output kernels (theta/halflife/mean/zscore)

**Files:**
- Modify: `include/atx/engine/alpha/ts_ops.hpp`
- Test: `tests/alpha_ts_ops_test.cpp` (append)

- [ ] **Step 1: Write the failing tests** for each derived quantity from a fit with `b∈(0,1)` (e.g. mean-reverting synthetic window) — theta=-ln(b), halflife=ln2/theta, mean=a/(1-b), zscore=(x_last-mean)/(resid_std/sqrt(1-b*b)); and NaN when b∉(0,1).

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement** four `noexcept` mappers from `OuAr1Fit` (+ current value for zscore):

```cpp
[[nodiscard]] inline atx::f64 ou_theta_of(const OuAr1Fit& f) noexcept {
  return (f.b > 0.0 && f.b < 1.0) ? -std::log(f.b) : kTsNaN;
}
[[nodiscard]] inline atx::f64 ou_halflife_of(const OuAr1Fit& f) noexcept {
  const atx::f64 th = ou_theta_of(f); return ts_is_nan(th) ? kTsNaN : std::log(2.0) / th;
}
[[nodiscard]] inline atx::f64 ou_mean_of(const OuAr1Fit& f) noexcept {
  return (f.b < 1.0 && !ts_is_nan(f.b)) ? f.a / (1.0 - f.b) : kTsNaN;
}
[[nodiscard]] inline atx::f64 ou_zscore_of(const OuAr1Fit& f, atx::f64 x_last) noexcept {
  if (!(f.b > 0.0 && f.b < 1.0)) return kTsNaN;
  const atx::f64 sig = f.resid_std / std::sqrt(1.0 - f.b * f.b);
  if (sig == 0.0 || ts_is_nan(sig)) return kTsNaN;
  return (x_last - f.a / (1.0 - f.b)) / sig;
}
```

- [ ] **Step 4: Run PASS. Step 5: commit**

```bash
clang-format -i include/atx/engine/alpha/ts_ops.hpp tests/alpha_ts_ops_test.cpp
git add include/atx/engine/alpha/ts_ops.hpp tests/alpha_ts_ops_test.cpp
git commit -m "feat(p3d-E2): OU derived-quantity mappers (theta/halflife/mean/zscore)"
```

## Task E3: Registry rows + typecheck (rolling family)

**Files:**
- Modify: `include/atx/engine/alpha/registry.hpp`, `include/atx/engine/alpha/typecheck.hpp`
- Test: `tests/alpha_typecheck_test.cpp` (append)

- [ ] **Step 1: Write the failing test** — `ou_zscore(close, 60)` typechecks Panel, lookback `(60-1)+0`.

```cpp
TEST(AlphaTypecheck, OuZscoreRollingLookback) {
  Library lib;
  auto ast = parse_program("a = ou_zscore(close, 60)\n", lib); ASSERT_TRUE(ast);
  auto an = analyze(ast.value()); ASSERT_TRUE(an) << an.error().message();
  EXPECT_EQ(an.value().info(ast.value().roots()[0].root).lookback, 59);
}
```

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement.** OpCodes `OuTheta, OuHalflife, OuMean, OuZscore`. Rows (arity 2: series, window — window is an operand literal like `ts_mean`, NOT an hparam):

```cpp
      {"ou_theta",    2, 2, OpCode::OuTheta,    DType::F64, true, {}, &shape_panel, 0, {}},
      {"ou_halflife", 2, 2, OpCode::OuHalflife, DType::F64, true, {}, &shape_panel, 0, {}},
      {"ou_mean",     2, 2, OpCode::OuMean,     DType::F64, true, {}, &shape_panel, 0, {}},
      {"ou_zscore",   2, 2, OpCode::OuZscore,   DType::F64, true, {}, &shape_panel, 0, {}},
```

Typecheck: add all four to `is_rolling_ts` (so lookback = `(d-1)+child` and a non-scalar primary is required, and `window_value` is read from arg `b`). Update exhaustive switches.

- [ ] **Step 4: Run PASS. Step 5: commit**

```bash
clang-format -i include/atx/engine/alpha/registry.hpp include/atx/engine/alpha/typecheck.hpp tests/alpha_typecheck_test.cpp
git add include/atx/engine/alpha/registry.hpp include/atx/engine/alpha/typecheck.hpp tests/alpha_typecheck_test.cpp
git commit -m "feat(p3d-E3): OU rolling family registry rows + rolling lookback"
```

## Task E4: VM + oracle windowed dispatch; differential

**Files:**
- Modify: `include/atx/engine/alpha/vm.hpp`, `include/atx/engine/alpha/oracle.hpp`
- Test: `tests/alpha_ts_diff_test.cpp` (append) or differential suite

- [ ] **Step 1: Write the failing differential test** — `ou_halflife`/`ou_zscore` VM==oracle over a random panel + known-value on a crafted mean-reverting series.

- [ ] **Step 2: Run to verify FAIL.**

- [ ] **Step 3: Implement.** Route the four OU OpCodes through the existing windowed `eval_time_series` path. Add per-cell handling in the Ts window dispatch (`detail::ts_value_at` or its sibling): for each `(t, j)` slice the trailing window `[t-d+1, t]` (the existing rolling machinery already provides this slice), call `ou_ar1_fit(window)`, then the matching `ou_*_of` mapper (zscore uses `x[t]` as `x_last`). Mirror in oracle. Confirm window-slice indexing matches the existing rolling ops EXACTLY (reuse their slicing helper).

- [ ] **Step 4: Run PASS + full differential suite. Step 5: commit**

```bash
ctest --test-dir build -R "Ou|Differential|Ts" --output-on-failure
clang-format -i include/atx/engine/alpha/vm.hpp include/atx/engine/alpha/oracle.hpp tests/alpha_ts_diff_test.cpp
git add include/atx/engine/alpha/vm.hpp include/atx/engine/alpha/oracle.hpp tests/alpha_ts_diff_test.cpp
git commit -m "feat(p3d-E4): VM+oracle OU rolling family (differential green)"
```

---

# Final integration

## Task F1: End-to-end pairs example + causality regression

**Files:**
- Test: `tests/alpha_statespace_e2e_test.cpp` (create)

- [ ] **Step 1: Write the test** — the headline program compiles, evaluates, one Kalman scan:

```cpp
TEST(AlphaStatespaceE2E, KalmanOuPipeline) {
  Library lib;
  const char* src =
    "kf = kalman(ret, hedge, 0.0001, 0.001)\n"
    "beta = kf.beta\n"
    "spread = kf.resid\n"
    "hl = ou_halflife(spread, 20)\n"
    "sig = -ou_zscore(spread, 20)\n";
  auto ast = parse_program(src, lib); ASSERT_TRUE(ast) << ast.error().message();
  auto an = analyze(ast.value()); ASSERT_TRUE(an) << an.error().message();
  auto prog = compile(ast.value(), an.value()); ASSERT_TRUE(prog) << prog.error().message();
  int k = 0; for (auto& in : prog.value().code) if (in.op==OpCode::KalmanReg) ++k;
  EXPECT_EQ(k, 1); // beta + spread share ONE Kalman scan
  // 4 roots: beta, spread, hl, sig (kf is a record intermediate, not a root)
  EXPECT_EQ(prog.value().roots.size(), 4u);
  auto panel = /* fields ret, hedge; dates>=21, instruments=3 */;
  Engine eng(panel);
  auto vm = eng.evaluate(prog.value()); ASSERT_TRUE(vm);
  auto orc = evaluate_reference(prog.value(), panel); ASSERT_TRUE(orc);
  for (size_t a=0;a<vm.value().alphas.size();++a)
    for (size_t i=0;i<vm.value().alphas[a].values.size();++i)
      EXPECT_EQ(vm.value().alphas[a].values[i], orc.value().alphas[a].values[i]);
}

TEST(AlphaStatespaceE2E, NoLookAhead) {
  // Perturbing the LAST date's input must not change any earlier output cell of a
  // recurrence op. Build two panels differing only at t=last; compare kalman_level
  // outputs for t<last — must be identical.
  // (construct, evaluate both, assert equality for t<last)
}
```

- [ ] **Step 2: Run to verify FAIL/PASS**, implement panel construction, iterate to PASS.

- [ ] **Step 3: Full suite green**

Run: `ctest --test-dir build -R "Alpha|Vm|Oracle|Differential|Kalman|Ou|MultiOutput|Bindings|Member|Statespace" --output-on-failure`

- [ ] **Step 4: commit**

```bash
clang-format -i tests/alpha_statespace_e2e_test.cpp
git add tests/alpha_statespace_e2e_test.cpp
git commit -m "test(p3d-F1): end-to-end Kalman/OU pairs pipeline + no-look-ahead regression"
```

## Task F2: Docs — operator reference

**Files:**
- Modify: the DSL operator reference doc (grep `decay_linear` in `atx-engine/` docs to find it) — add the 7 new ops + `.pin`/bindings syntax with the headline example.

- [ ] **Step 1:** Add entries (signature, math, output, NaN policy) for `kalman_level`, `kalman` (record), `ou_filter`, `ou_theta`, `ou_halflife`, `ou_mean`, `ou_zscore`; document local bindings + `.pin`.
- [ ] **Step 2: commit**

```bash
git add <doc path>
git commit -m "docs(p3d-F2): document state-space/OU operators + bindings/.pin syntax"
```

---

# Self-review (planner checklist — completed)

**Spec coverage:** §3.1 bindings → A1–A3. §3.2 records/member → B4–B6. §3.3 multi-output IR → B1–B3,B7–B9. §3.4 registry → B3,C4,D2,E3. §4.1 kalman_level → C2,C4,C5. §4.2 kalman record → D1–D4. §4.3 ou_filter → C3,C4,C5. §4.4 OU family → E1–E4. §7 testing → differential per op + D4 known-value + F1 causality. All spec sections map to tasks.

**Open items resolved into tasks:** RecordInfo storage = span-from-registry (B6). state stride = instrument-outer locals (C5 note supersedes C1's buffer — C1 kept as guard/no-op). Diagnostic channel = out-param overload (A3). OU NaN/min-period = match existing slope/resid (E1, explicit). Python fixture = `scripts/gen_kalman_ref.py` + `tests/fixtures/` (D4).

**Known soft spots flagged for the executor (verify against live code, do not assume):**
1. `SlotPool` liveness-parity `acquire()` count for multi-output computes (B9 note) — align to the actual assert in `vm.hpp`.
2. Whether recurrence scanning is instrument-outer (locals suffice) or date-outer (needs C1 strided buffer) — confirm from existing `trade_when`/`hump` and align (C5 note).
3. Aggregate-init of `kOps` rows after adding two `OpSig` trailing fields — confirm omitted trailing members default-init (B3 note); if not, append `, 0, {}` to every existing row.
4. Existing rolling window-slice helper name in `ts_ops.hpp`/`vm.hpp` — reuse it for OU (E4), do not hand-roll slicing.
