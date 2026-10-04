// atx::engine::alpha — an evaluated program never depends on what its Engine ran before
// (platform v8 lane ENG-SLOT).
//
// Engine::evaluate reuses its SlotPool across calls (vm.hpp: `pool_`, grown by ensure_pool). Only
// a NEW pool is zero-filled (panel.hpp, SlotPool's constructor); a reused one still holds the
// previous program's values. So a result is independent of the Engine's history only if every op
// writes EVERY cell of its destination slot, on every date and name, the cells it leaves NaN
// included. This file pins that property for every op the DSL can name:
//
//   * the ops: every row of the catalogue (alpha::detail::builtin_ops() and literature_ops(),
//     iterated, never listed), every infix and prefix operator the unparser can spell
//     (binary_op_text / unary_op_text over every OpCode value) and the ternary;
//   * the variants: each operand position is probed with a numeric field, a Group classifier, a
//     mask and a literal. Every probe the type checker accepts is expanded over the literal values
//     {1, 2, 3, 5, 20, 0.5} (windows of one bar, shorter than, equal to and beyond the 12-date
//     history, hparams) and over rotated or identical vector fields. So each arity, window and
//     group variant the catalogue declares valid is evaluated, and a new op is covered unedited;
//   * the arms: a fresh Engine is the reference. The same program then runs on Engines first
//     dirtied by a finite-constant poison and by a NaN / +-inf poison sized to the target (the pool
//     is reused, it does not grow), by the widest NaN poison (reused, larger), and by a one-slot
//     poison (the pool grows), and on one long-lived Engine that has run every earlier variant.
//     Every alpha must be byte-equal to the reference, NaN payloads included;
//   * the configs: AuditExact and ResearchFast (different Ts kernels), each with and without a
//     cross-section mask.
//
// Each target carries a second root `keep = x + y + z + u`. It holds the fields live past the op,
// so the op's destination is a slot the target has not written before: a cell the op skipped reads
// 0.0 on the fresh Engine and a poison value (no poison leaf is 0.0) on a dirty one.
//
// Naming: Subject_Condition_ExpectedResult.

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/unparse.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atx_test_alpha_vm_slot_reuse {

namespace alpha = atx::engine::alpha;
using alpha::Engine;
using alpha::OpCode;
using alpha::Panel;
using alpha::Program;
using alpha::SignalSet;
using atx::f64;
using atx::u64;
using atx::u8;
using atx::usize;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr usize kDates = 12;
constexpr usize kNames = 7;

// Operand probes. A vector, group or mask probe at position k uses entry k mod size, so two
// operands of one call read different inputs. Between them the two literal probes satisfy every
// literal rail of the catalogue (a window or count needs 5; kalman's delta needs a value in (0,1)).
constexpr std::array<std::string_view, 4> kVectors{{"x", "y", "z", "u"}};
constexpr std::array<std::string_view, 2> kGroups{{"grp_a", "grp_b"}};
constexpr std::array<std::string_view, 3> kMasks{{"(x > y)", "(z <= u)", "(y >= z)"}};
constexpr std::array<std::string_view, 2> kLiteralProbes{{"5", "0.5"}};
constexpr usize kProbeKinds = 3 + kLiteralProbes.size();
// The values each literal operand of an accepted probe is expanded over.
constexpr std::array<std::string_view, 6> kLiteralValues{{"1", "2", "3", "5", "20", "0.5"}};
// Every value an OpCode (u8 underlying) can hold.
constexpr usize kOpCodeValues = usize{std::numeric_limits<u8>::max()} + 1;

// The poison programs reach this many slots beyond the widest target.
constexpr usize kPoisonMargin = 4;
constexpr usize kMaxPoisonLeaves = 256;
// Detailed failures reported before the rest are only counted.
constexpr usize kMaxReported = 25;

constexpr std::string_view kSharedArm = "one Engine that ran every earlier variant";

[[nodiscard]] const alpha::Library &lib() {
  static const alpha::Library l;
  return l;
}

// ---- the panel ---------------------------------------------------------------------------------

// x: a five-step ladder in [-2, 2] (ties across names and dates, zeros), a flat run (name 3,
// dates 0-5), a hole (name 1, date 4), an all-NaN name (5) and a name whose history starts at
// date 8 (6), shorter than a 5-bar window.
[[nodiscard]] f64 field_x(usize t, usize j) noexcept {
  if (j == 5 || (j == 6 && t < 8) || (j == 1 && t == 4)) {
    return kNaN;
  }
  if (j == 3 && t < 6) {
    return 1.0;
  }
  return static_cast<f64>((t * 7 + j * 3) % 5) - 2.0;
}

// y: half steps in [-1, 2] with zeros and ties, the all-NaN name 5 and a hole (name 2, date 6).
[[nodiscard]] f64 field_y(usize t, usize j) noexcept {
  if (j == 5 || (j == 2 && t == 6)) {
    return kNaN;
  }
  return 0.5 * static_cast<f64>((t * 3 + j * 5) % 7) - 1.0;
}

// z: positive and trending (log is defined), defined for name 5, one hole (name 4, date 10).
[[nodiscard]] f64 field_z(usize t, usize j) noexcept {
  if (j == 4 && t == 10) {
    return kNaN;
  }
  return 1.0 + 0.25 * static_cast<f64>(t) + 0.5 * static_cast<f64>(j) +
         0.125 * static_cast<f64>((t * j) % 3);
}

// u: {-1, 0, 1}, one hole (name 0, date 2).
[[nodiscard]] f64 field_u(usize t, usize j) noexcept {
  if (j == 0 && t == 2) {
    return kNaN;
  }
  return static_cast<f64>((t + 2 * j) % 3) - 1.0;
}

// grp_a: three labels, NaN for name 5 and for name 2 on date 3; name 0 changes group at date 6.
[[nodiscard]] f64 field_grp_a(usize t, usize j) noexcept {
  if (j == 5 || (j == 2 && t == 3)) {
    return kNaN;
  }
  if (j == 0 && t >= 6) {
    return 3.0;
  }
  return static_cast<f64>(1 + j % 3);
}

// grp_b: two labels, NaN for name 6 on date 7.
[[nodiscard]] f64 field_grp_b(usize t, usize j) noexcept {
  if (j == 6 && t == 7) {
    return kNaN;
  }
  return static_cast<f64>(1 + j % 2);
}

// pz: the NaN poison's input. Every third cell is NaN, the rest positive. pz - c is never 0 or 1
// for a poison constant c (3(t - i) + 9j is never 4 or 8), so no poison leaf is exactly 0.0.
[[nodiscard]] f64 field_pz(usize t, usize j) noexcept {
  if ((t * kNames + j) % 3 == 0) {
    return kNaN;
  }
  return 1.5 + 0.75 * static_cast<f64>(t) + 2.25 * static_cast<f64>(j);
}

struct FieldDef {
  std::string_view name;
  f64 (*value)(usize, usize) noexcept;
};

constexpr std::array<FieldDef, 7> kFields{{{"x", &field_x},
                                           {"y", &field_y},
                                           {"z", &field_z},
                                           {"u", &field_u},
                                           {"grp_a", &field_grp_a},
                                           {"grp_b", &field_grp_b},
                                           {"pz", &field_pz}}};

// The fields above, with name 4 listed from date 3 and name 2 out of the universe on date 9.
[[nodiscard]] Panel make_panel() {
  std::vector<std::string> names;
  std::vector<std::vector<f64>> cols;
  for (const FieldDef &f : kFields) {
    names.emplace_back(f.name);
    std::vector<f64> col(kDates * kNames, 0.0);
    for (usize t = 0; t < kDates; ++t) {
      for (usize j = 0; j < kNames; ++j) {
        col[t * kNames + j] = f.value(t, j);
      }
    }
    cols.push_back(std::move(col));
  }
  std::vector<std::uint8_t> universe(kDates * kNames, std::uint8_t{1});
  for (usize t = 0; t < kDates; ++t) {
    for (usize j = 0; j < kNames; ++j) {
      if ((j == 4 && t < 3) || (j == 2 && t == 9)) {
        universe[t * kNames + j] = std::uint8_t{0};
      }
    }
  }
  auto p = Panel::create(kDates, kNames, std::move(names), std::move(cols), std::move(universe));
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  if (p) {
    return std::move(*p);
  }
  return Panel::create(0, 0, {}, {}, {}).value();
}

// The cross-section eligibility of the masked configs: name 3 on even dates, name 0 on date 1 and
// name 6 from date 10 are excluded.
[[nodiscard]] std::vector<u8> make_cs_mask() {
  std::vector<u8> mask(kDates * kNames, u8{1});
  for (usize t = 0; t < kDates; ++t) {
    for (usize j = 0; j < kNames; ++j) {
      if ((j == 3 && t % 2 == 0) || (j == 0 && t == 1) || (j == 6 && t >= 10)) {
        mask[t * kNames + j] = u8{0};
      }
    }
  }
  return mask;
}

// ---- text helpers ------------------------------------------------------------------------------

// A finite literal as DSL source (fixed notation).
[[nodiscard]] std::string literal_text(f64 v) {
  std::array<char, 64> buf{};
  const std::to_chars_result r =
      std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed);
  return r.ec == std::errc{} ? std::string(buf.data(), r.ptr) : std::string{"1"};
}

// A cell value for a failure message (shortest form; nan / inf spelled out).
[[nodiscard]] std::string value_text(f64 v) {
  std::array<char, 64> buf{};
  const std::to_chars_result r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  return r.ec == std::errc{} ? std::string(buf.data(), r.ptr) : std::string{"?"};
}

// A cell's bit pattern in hex, so NaN payloads and signed zeros are visible.
[[nodiscard]] std::string bits_text(f64 v) {
  std::array<char, 32> buf{};
  const std::to_chars_result r =
      std::to_chars(buf.data(), buf.data() + buf.size(), std::bit_cast<u64>(v), 16);
  return "0x" + (r.ec == std::errc{} ? std::string(buf.data(), r.ptr) : std::string{"?"});
}

[[nodiscard]] std::string join(const std::vector<std::string> &parts) {
  std::string s;
  for (const std::string &p : parts) {
    if (!s.empty()) {
      s += "; ";
    }
    s += p;
  }
  return s;
}

// "" when every alpha of `got` is byte-equal to `want` (NaN payloads included), else where the
// first difference is.
[[nodiscard]] std::string first_difference(const SignalSet &want, const SignalSet &got) {
  if (want.dates != got.dates || want.instruments != got.instruments ||
      want.alphas.size() != got.alphas.size()) {
    return "the signal set has a different shape";
  }
  const usize names = want.instruments == 0 ? usize{1} : want.instruments;
  for (usize a = 0; a < want.alphas.size(); ++a) {
    const std::vector<f64> &w = want.alphas[a].values;
    const std::vector<f64> &g = got.alphas[a].values;
    if (want.alphas[a].name != got.alphas[a].name || w.size() != g.size()) {
      return "alpha " + std::to_string(a) + " has a different name or size";
    }
    if (w.empty() || std::memcmp(w.data(), g.data(), w.size() * sizeof(f64)) == 0) {
      continue;
    }
    for (usize i = 0; i < w.size(); ++i) {
      if (std::bit_cast<u64>(w[i]) != std::bit_cast<u64>(g[i])) {
        return "alpha '" + want.alphas[a].name + "' date " + std::to_string(i / names) +
               " name " + std::to_string(i % names) + ": fresh Engine " + value_text(w[i]) +
               " (" + bits_text(w[i]) + "), dirty Engine " + value_text(g[i]) + " (" +
               bits_text(g[i]) + ")";
      }
    }
  }
  return {};
}

// ---- the catalogue as call forms ---------------------------------------------------------------

enum class FormKind : u8 { Call, Infix, Prefix, Ternary };

// One way the DSL spells an op: a named catalogue row `name(a, ...)`, an infix `(a OP b)`, a
// prefix `OP(a)` or the ternary `(c ? a : b)`.
struct Form {
  std::string label;
  FormKind kind{FormKind::Call};
  std::string text;
  usize min_args{0};
  usize max_args{0};
  std::span<const alpha::PinSig> pins{}; // a record op: one root per pin
  OpCode opcode{OpCode::Const};
  bool row{false}; // a row of the registry's catalogue
};

// Every catalogue row (built-ins, then the literature rows), every operator spelling, the ternary.
[[nodiscard]] std::vector<Form> catalogue_forms() {
  std::vector<Form> forms;
  const auto add_rows = [&forms](std::span<const alpha::OpSig> rows) {
    for (const alpha::OpSig &sig : rows) {
      forms.push_back(Form{std::string{sig.name}, FormKind::Call, std::string{sig.name},
                           sig.min_arity, sig.max_arity, sig.pins, sig.opcode, true});
    }
  };
  add_rows(alpha::detail::builtin_ops());
  add_rows(alpha::detail::literature_ops());
  for (usize v = 0; v < kOpCodeValues; ++v) {
    const auto op = static_cast<OpCode>(static_cast<u8>(v));
    const std::string_view bin = alpha::detail::binary_op_text(op);
    if (!bin.empty()) {
      forms.push_back(Form{"infix " + std::string{bin}, FormKind::Infix, std::string{bin}, 2, 2,
                           {}, op, false});
    }
    const std::string_view pre = alpha::detail::unary_op_text(op);
    if (!pre.empty()) {
      forms.push_back(Form{"prefix " + std::string{pre}, FormKind::Prefix, std::string{pre}, 1,
                           1, {}, op, false});
    }
  }
  forms.push_back(Form{"ternary ?:", FormKind::Ternary, "?:", 3, 3, {}, OpCode::Select, false});
  return forms;
}

[[nodiscard]] std::string call_text(const Form &form, const std::vector<std::string> &args) {
  switch (form.kind) {
  case FormKind::Call: {
    std::string s = form.text + "(";
    for (usize k = 0; k < args.size(); ++k) {
      if (k != 0) {
        s += ", ";
      }
      s += args[k];
    }
    return s + ")";
  }
  case FormKind::Infix:
    return "(" + args.at(0) + " " + form.text + " " + args.at(1) + ")";
  case FormKind::Prefix:
    return form.text + "(" + args.at(0) + ")";
  case FormKind::Ternary:
    return "(" + args.at(0) + " ? " + args.at(1) + " : " + args.at(2) + ")";
  }
  return {};
}

// The roots of one call: `out0 = expr`, or `outK = expr.pinK` for each pin of a record op.
[[nodiscard]] std::string roots_text(const Form &form, const std::string &expr) {
  if (form.pins.empty()) {
    return "out0 = " + expr + "\n";
  }
  std::string s;
  for (usize k = 0; k < form.pins.size(); ++k) {
    s += "out" + std::to_string(k) + " = " + expr + "." + std::string{form.pins[k].name} + "\n";
  }
  return s;
}

// The root that keeps every vector field live past the op under test (see the file comment).
[[nodiscard]] std::string keep_root() {
  std::string s = "keep = ";
  for (usize k = 0; k < kVectors.size(); ++k) {
    if (k != 0) {
      s += " + ";
    }
    s += kVectors[k];
  }
  return s + "\n";
}

// Parse, type-check and compile `src`. nullopt when the parser or the type checker refuses it (not
// a valid program); a program that is analyzed but does not compile is recorded as a problem.
[[nodiscard]] std::optional<Program> compile_program(std::string_view src,
                                                     std::vector<std::string> &problems) {
  auto ast = alpha::parse_program(src, lib());
  if (!ast) {
    return std::nullopt;
  }
  auto ana = alpha::analyze(*ast);
  if (!ana) {
    return std::nullopt;
  }
  auto prog = alpha::compile(*ast, *ana);
  if (!prog) {
    problems.push_back(std::string{src} + ": analyzed but not compiled: " +
                       prog.error().message());
    return std::nullopt;
  }
  return std::move(*prog);
}

[[nodiscard]] bool uses_opcode(const Program &p, OpCode op) {
  return std::any_of(p.code.begin(), p.code.end(),
                     [op](const alpha::Instr &in) { return in.op == op; });
}

// ---- operand probes and their expansions -------------------------------------------------------

enum class ArgKind : u8 { Vector, Group, Mask, Literal };

struct Arg {
  ArgKind kind{ArgKind::Vector};
  std::string text;
};

// Probe `kind` (0 vector, 1 group, 2 mask, then the literal probes) at operand `position`.
[[nodiscard]] Arg probe_arg(usize position, usize kind) {
  switch (kind) {
  case 0:
    return Arg{ArgKind::Vector, std::string{kVectors[position % kVectors.size()]}};
  case 1:
    return Arg{ArgKind::Group, std::string{kGroups[position % kGroups.size()]}};
  case 2:
    return Arg{ArgKind::Mask, std::string{kMasks[position % kMasks.size()]}};
  default:
    return Arg{ArgKind::Literal, std::string{kLiteralProbes[(kind - 3) % kLiteralProbes.size()]}};
  }
}

// The probe tuple numbered `code` (base kProbeKinds, position 0 least significant).
[[nodiscard]] std::vector<Arg> probe_tuple(usize arity, usize code) {
  std::vector<Arg> args;
  args.reserve(arity);
  usize rest = code;
  for (usize k = 0; k < arity; ++k) {
    args.push_back(probe_arg(k, rest % kProbeKinds));
    rest /= kProbeKinds;
  }
  return args;
}

[[nodiscard]] std::vector<std::string> arg_texts(const std::vector<Arg> &args) {
  std::vector<std::string> out;
  out.reserve(args.size());
  for (const Arg &a : args) {
    out.push_back(a.text);
  }
  return out;
}

// The argument lists one accepted probe stands for: each literal position over kLiteralValues
// (a call of literals only is a scalar program and is kept as probed), and the vector positions
// either rotated across fields or all `x` (ties, identical and collinear inputs).
[[nodiscard]] std::vector<std::vector<std::string>> expansions(const std::vector<Arg> &probe) {
  usize vectors = 0;
  usize literals = 0;
  for (const Arg &a : probe) {
    vectors += a.kind == ArgKind::Vector ? usize{1} : usize{0};
    literals += a.kind == ArgKind::Literal ? usize{1} : usize{0};
  }
  const bool expand = literals < probe.size();
  usize combos = 1;
  if (expand) {
    for (usize k = 0; k < literals; ++k) {
      combos *= kLiteralValues.size();
    }
  }
  const usize vector_modes = vectors >= 2 ? usize{2} : usize{1};
  std::vector<std::vector<std::string>> out;
  for (usize mode = 0; mode < vector_modes; ++mode) {
    for (usize code = 0; code < combos; ++code) {
      usize digit = code;
      std::vector<std::string> args;
      args.reserve(probe.size());
      for (const Arg &a : probe) {
        if (expand && a.kind == ArgKind::Literal) {
          args.emplace_back(kLiteralValues[digit % kLiteralValues.size()]);
          digit /= kLiteralValues.size();
        } else if (mode == 1 && a.kind == ArgKind::Vector) {
          args.emplace_back(kVectors.front());
        } else {
          args.push_back(a.text);
        }
      }
      out.push_back(std::move(args));
    }
  }
  return out;
}

// ---- the fixture -------------------------------------------------------------------------------

struct Variant {
  std::string label; // the call as written
  Program prog;      // its roots plus the keep root
};

struct Coverage {
  std::string form;
  OpCode opcode{OpCode::Const};
  bool row{false};
  usize variants{0}; // accepted variants whose code runs `opcode`
};

struct Poison {
  Program prog;
  usize slots{0};
};

struct Fixture {
  Panel panel;
  std::vector<u8> cs_mask;
  std::vector<Variant> variants;
  std::vector<Coverage> coverage;
  std::vector<Poison> finite_poisons; // by leaf count 1, 2, ...: finite constants only
  std::vector<Poison> nan_poisons;    // by leaf count: NaN (both signs), +-inf, finite
  usize max_target_slots{0};
  std::vector<std::string> problems;
};

// Every accepted variant of `form` with `arity` operands, appended to fx.variants. Returns how
// many run the form's opcode (a variant the parser folded or strength-reduced away is dropped).
usize add_variants(const Form &form, usize arity, std::set<std::string> &tried, Fixture &fx) {
  usize probes = 1;
  for (usize k = 0; k < arity; ++k) {
    probes *= kProbeKinds;
  }
  usize made = 0;
  for (usize code = 0; code < probes; ++code) {
    const std::vector<Arg> probe = probe_tuple(arity, code);
    if (!compile_program(roots_text(form, call_text(form, arg_texts(probe))), fx.problems)) {
      continue;
    }
    for (const std::vector<std::string> &args : expansions(probe)) {
      const std::string expr = call_text(form, args);
      if (!tried.insert(expr).second) {
        continue;
      }
      std::optional<Program> prog =
          compile_program(roots_text(form, expr) + keep_root(), fx.problems);
      if (!prog || !uses_opcode(*prog, form.opcode)) {
        continue;
      }
      fx.variants.push_back(Variant{expr, std::move(*prog)});
      ++made;
    }
  }
  return made;
}

// Finite poison leaf i: a distinct constant (a `max` call is not folded, so each stays a slot).
[[nodiscard]] std::string finite_leaf(usize i) {
  return literal_text(7919.25 + 104.5 * static_cast<f64>(i));
}

// NaN poison leaf i over pz: finite positive, finite negative with sign-flipped NaN, +-inf with
// NaN, or log (finite, NaN) -- by i mod 4.
[[nodiscard]] std::string nan_leaf(usize i) {
  const std::string c = literal_text(2.5 + 0.75 * static_cast<f64>(i));
  switch (i % 4) {
  case 0:
    return "(pz * " + c + ")";
  case 1:
    return "(-(pz * " + c + "))";
  case 2:
    return "((pz - " + c + ") / (pz - pz))";
  default:
    return "log(pz - " + c + ")";
  }
}

// Leaves bound as roots l1..ln (readable in the output), then `root` = a right-nested `max`
// (finite) or `+` (NaN) of them. Every leaf stays live until the last one is built, so n leaves
// fill about n slots, each written in full.
[[nodiscard]] std::string poison_text(std::string_view root, usize leaves, bool finite) {
  std::string src;
  for (usize i = 1; i <= leaves; ++i) {
    src += "l" + std::to_string(i) + " = " + (finite ? finite_leaf(i) : nan_leaf(i)) + "\n";
  }
  std::string expr = "l" + std::to_string(leaves);
  for (usize i = leaves; i-- > 1;) {
    const std::string li = "l" + std::to_string(i);
    expr = finite ? "max(" + li + ", " + expr + ")" : li + " + (" + expr + ")";
  }
  return src + std::string{root} + " = " + expr + "\n";
}

// Poisons of 1, 2, ... leaves until both families reach the widest target plus kPoisonMargin.
void build_poisons(Fixture &fx) {
  const usize want = fx.max_target_slots + kPoisonMargin;
  for (usize n = 1; n <= kMaxPoisonLeaves; ++n) {
    std::optional<Program> fin = compile_program(poison_text("pa", n, true), fx.problems);
    std::optional<Program> nanp = compile_program(poison_text("pb", n, false), fx.problems);
    if (!fin || !nanp) {
      fx.problems.push_back("the poison programs of " + std::to_string(n) +
                            " leaves do not compile");
      return;
    }
    const usize fin_slots = fin->num_slots;
    const usize nan_slots = nanp->num_slots;
    fx.finite_poisons.push_back(Poison{std::move(*fin), fin_slots});
    fx.nan_poisons.push_back(Poison{std::move(*nanp), nan_slots});
    if (fin_slots >= want && nan_slots >= want) {
      return;
    }
  }
  fx.problems.push_back("no poison program reaches " + std::to_string(want) + " slots");
}

[[nodiscard]] Fixture build_fixture() {
  Fixture fx{make_panel(), make_cs_mask(), {}, {}, {}, {}, 0, {}};
  std::set<std::string> tried;
  for (const Form &form : catalogue_forms()) {
    usize made = 0;
    for (usize arity = form.min_args; arity <= form.max_args; ++arity) {
      made += add_variants(form, arity, tried, fx);
    }
    fx.coverage.push_back(Coverage{form.label, form.opcode, form.row, made});
  }
  for (const Variant &v : fx.variants) {
    fx.max_target_slots = std::max<usize>(fx.max_target_slots, v.prog.num_slots);
  }
  build_poisons(fx);
  return fx;
}

[[nodiscard]] const Fixture &fixture() {
  static const Fixture fx = build_fixture();
  return fx;
}

// The first poison with at least `slots` slots: the target then reuses the pool without growth
// and every slot it touches holds poison. Precondition: `poisons` is not empty.
[[nodiscard]] const Poison &tightest(const std::vector<Poison> &poisons, usize slots) {
  for (const Poison &p : poisons) {
    if (p.slots >= slots) {
      return p;
    }
  }
  return poisons.back();
}

// ---- the arms ----------------------------------------------------------------------------------

struct Config {
  alpha::EvalMode mode{alpha::EvalMode::AuditExact};
  bool masked{false};
};

// Apply `cfg` to a new Engine; "" on success.
[[nodiscard]] std::string configure(Engine &engine, const Config &cfg, const Fixture &fx) {
  engine.set_eval_mode(cfg.mode);
  if (!cfg.masked) {
    return {};
  }
  const atx::core::Status s = engine.set_cross_section_mask(fx.cs_mask);
  return s ? std::string{} : s.error().message();
}

struct ArmRun {
  std::string error; // empty on success
  SignalSet signals;
  usize capacity_before{0}; // pool capacity after the poison
  usize capacity_after{0};  // pool capacity after the target
};

// On one new Engine: `poison` (when not null), then `target`.
[[nodiscard]] ArmRun run_after(const Fixture &fx, const Config &cfg, const Program *poison,
                               const Program &target) {
  ArmRun run;
  Engine engine{fx.panel};
  run.error = configure(engine, cfg, fx);
  if (!run.error.empty()) {
    return run;
  }
  if (poison != nullptr) {
    auto dirt = engine.evaluate(*poison);
    if (!dirt) {
      run.error = "poison: " + dirt.error().message();
      return run;
    }
  }
  run.capacity_before = engine.pool_capacity();
  auto out = engine.evaluate(target);
  if (!out) {
    run.error = out.error().message();
    return run;
  }
  run.capacity_after = engine.pool_capacity();
  run.signals = std::move(*out);
  return run;
}

struct Arm {
  std::string_view name;
  const Program *poison;
  bool grows; // the target needs more slots than the poison left
};

class Reporter {
public:
  void fail(const Variant &v, std::string_view arm, std::string_view what) {
    ++failures_;
    if (failures_ <= kMaxReported) {
      ADD_FAILURE() << v.label << " [" << arm << "]: " << what;
    }
  }
  void compare(const Variant &v, std::string_view arm, const SignalSet &want,
               const SignalSet &got) {
    const std::string diff = first_difference(want, got);
    if (!diff.empty()) {
      fail(v, arm, diff);
    }
  }
  [[nodiscard]] usize failures() const noexcept { return failures_; }

private:
  usize failures_{0};
};

// A new Engine configured by `cfg` and dirtied by the widest NaN poison; null after ADD_FAILURE.
[[nodiscard]] std::unique_ptr<Engine> dirty_engine(const Fixture &fx, const Config &cfg) {
  auto engine = std::make_unique<Engine>(fx.panel);
  std::string error = configure(*engine, cfg, fx);
  if (error.empty()) {
    auto dirt = engine->evaluate(fx.nan_poisons.back().prog);
    if (!dirt) {
      error = dirt.error().message();
    }
  }
  if (!error.empty()) {
    ADD_FAILURE() << "the shared Engine: " << error;
    return nullptr;
  }
  return engine;
}

// The property: every variant, on every arm, is byte-equal to a fresh Engine's result.
void expect_independent_of_prior_programs(const Config &cfg) {
  const Fixture &fx = fixture();
  ASSERT_TRUE(fx.problems.empty()) << join(fx.problems);
  ASSERT_FALSE(fx.variants.empty());
  ASSERT_FALSE(fx.finite_poisons.empty());
  ASSERT_FALSE(fx.nan_poisons.empty());
  Reporter rep;
  std::unique_ptr<Engine> shared;
  for (const Variant &v : fx.variants) {
    if (shared == nullptr) {
      shared = dirty_engine(fx, cfg);
      ASSERT_TRUE(shared != nullptr);
    }
    const ArmRun ref = run_after(fx, cfg, nullptr, v.prog);
    if (!ref.error.empty()) {
      rep.fail(v, "fresh Engine", ref.error);
      continue;
    }
    const usize slots = v.prog.num_slots;
    const std::array<Arm, 4> arms{{
        {"finite poison, pool reused", &tightest(fx.finite_poisons, slots).prog, false},
        {"NaN poison, pool reused", &tightest(fx.nan_poisons, slots).prog, false},
        {"widest NaN poison, pool reused", &fx.nan_poisons.back().prog, false},
        {"one-slot poison, pool grows", &fx.finite_poisons.front().prog, true},
    }};
    for (const Arm &arm : arms) {
      const ArmRun got = run_after(fx, cfg, arm.poison, v.prog);
      if (!got.error.empty()) {
        rep.fail(v, arm.name, got.error);
        continue;
      }
      if ((got.capacity_after > got.capacity_before) != arm.grows) {
        rep.fail(v, arm.name, arm.grows ? "the pool did not grow" : "the pool grew");
      }
      rep.compare(v, arm.name, ref.signals, got.signals);
    }
    auto again = shared->evaluate(v.prog);
    if (!again) {
      rep.fail(v, kSharedArm, again.error().message());
      shared.reset(); // a failed evaluate may leave slots acquired: start a new Engine
      continue;
    }
    rep.compare(v, kSharedArm, ref.signals, *again);
  }
  EXPECT_EQ(rep.failures(), usize{0}) << "failures over " << fx.variants.size() << " variants";
}

// Cell census of every alpha of a signal set.
struct Census {
  usize nan_cells{0};
  usize pos_inf{0};
  usize neg_inf{0};
  usize finite_cells{0};
  usize zero_cells{0};
};

[[nodiscard]] Census census(const SignalSet &s) {
  Census c;
  for (const SignalSet::Alpha &a : s.alphas) {
    for (const f64 v : a.values) {
      if (std::isnan(v)) {
        ++c.nan_cells;
      } else if (std::isinf(v)) {
        ++(v > 0.0 ? c.pos_inf : c.neg_inf);
      } else {
        ++c.finite_cells;
      }
      c.zero_cells += v == 0.0 ? usize{1} : usize{0};
    }
  }
  return c;
}

// ---- tests -------------------------------------------------------------------------------------

// Every catalogue row and operator yields at least one evaluated variant (a record op may instead
// be reached through its consumers, e.g. pack2 through ts_resid_on), so the property tests below
// cover every op without a hand-kept list.
TEST(AlphaVmSlotReuse, Catalogue_EveryRowAndOperator_HasAnEvaluatedVariant) {
  const Fixture &fx = fixture();
  EXPECT_TRUE(fx.problems.empty()) << join(fx.problems);
  std::array<bool, kOpCodeValues> seen{};
  for (const Variant &v : fx.variants) {
    for (const alpha::Instr &in : v.prog.code) {
      seen[static_cast<u8>(in.op)] = true;
    }
  }
  usize rows = 0;
  std::vector<std::string> uncovered;
  for (const Coverage &c : fx.coverage) {
    rows += c.row ? usize{1} : usize{0};
    if (c.variants == 0 && !seen[static_cast<u8>(c.opcode)]) {
      uncovered.push_back(c.form);
    }
  }
  EXPECT_EQ(rows, alpha::detail::builtin_ops().size() + alpha::detail::literature_ops().size());
  EXPECT_TRUE(uncovered.empty()) << "no evaluated variant runs: " << join(uncovered);
  EXPECT_GT(fx.variants.size(), fx.coverage.size());
  RecordProperty("forms", std::to_string(fx.coverage.size()));
  RecordProperty("variants", std::to_string(fx.variants.size()));
  RecordProperty("max_target_slots", std::to_string(fx.max_target_slots));
}

// The poisons do what the arms rely on: a one-slot program (the growth arm), programs at least
// as wide as every target (the reuse arms), finite non-zero constants, and NaN / +inf / -inf /
// finite cells that are never 0.0 (the value a fresh pool holds).
TEST(AlphaVmSlotReuse, Poisons_SpanEveryTargetSlot_DistinctFromAFreshPool) {
  const Fixture &fx = fixture();
  ASSERT_TRUE(fx.problems.empty()) << join(fx.problems);
  ASSERT_FALSE(fx.finite_poisons.empty());
  ASSERT_FALSE(fx.nan_poisons.empty());
  EXPECT_EQ(fx.finite_poisons.front().slots, usize{1});
  EXPECT_GE(fx.finite_poisons.back().slots, fx.max_target_slots + kPoisonMargin);
  EXPECT_GE(fx.nan_poisons.back().slots, fx.max_target_slots + kPoisonMargin);

  Engine engine{fx.panel};
  auto fin_out = engine.evaluate(fx.finite_poisons.back().prog);
  ASSERT_TRUE(fin_out.has_value()) << fin_out.error().message();
  const Census fc = census(*fin_out);
  EXPECT_GT(fc.finite_cells, usize{0});
  EXPECT_EQ(fc.nan_cells + fc.pos_inf + fc.neg_inf + fc.zero_cells, usize{0});

  auto nan_out = engine.evaluate(fx.nan_poisons.back().prog);
  ASSERT_TRUE(nan_out.has_value()) << nan_out.error().message();
  const Census nc = census(*nan_out);
  EXPECT_GT(nc.nan_cells, usize{0});
  EXPECT_GT(nc.pos_inf, usize{0});
  EXPECT_GT(nc.neg_inf, usize{0});
  EXPECT_GT(nc.finite_cells, usize{0});
  EXPECT_EQ(nc.zero_cells, usize{0});
}

TEST(AlphaVmSlotReuse, EveryOp_DirtyPoolAuditExact_ByteEqualToAFreshEngine) {
  expect_independent_of_prior_programs(Config{alpha::EvalMode::AuditExact, false});
}

TEST(AlphaVmSlotReuse, EveryOp_DirtyPoolResearchFast_ByteEqualToAFreshEngine) {
  expect_independent_of_prior_programs(Config{alpha::EvalMode::ResearchFast, false});
}

TEST(AlphaVmSlotReuse, EveryOp_DirtyPoolMaskedAuditExact_ByteEqualToAFreshEngine) {
  expect_independent_of_prior_programs(Config{alpha::EvalMode::AuditExact, true});
}

TEST(AlphaVmSlotReuse, EveryOp_DirtyPoolMaskedResearchFast_ByteEqualToAFreshEngine) {
  expect_independent_of_prior_programs(Config{alpha::EvalMode::ResearchFast, true});
}

} // namespace atx_test_alpha_vm_slot_reuse
