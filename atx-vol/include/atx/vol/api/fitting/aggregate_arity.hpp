#pragma once

// Compile-time aggregate field-count probe — the enforcement half of atx-vol's
// "designated initializers only" construction contract (v1 plan item 4.2).
//
// The contract it backs: several public aggregates (AlOpts, RunConfig,
// SessionInputs, SurfaceParityReport) used to be grown by APPENDING each new
// field at the very end, so that existing POSITIONAL brace initializers kept
// compiling and kept meaning the same thing. That convention made declaration
// ORDER part of the public API, forced every new knob to live away from the
// field it belongs with, and still failed at the one thing it promised: a
// positional initializer silently rebinds the moment anyone inserts rather than
// appends. The convention is dead — fields live in their natural grouping and
// every construction site names its fields — and each affected struct pins its
// field count with `aggregate_arity_is_v`, so a future append turns red at
// compile time instead of quietly resurrecting the hazard.
//
// Why a template and not a bare `static_assert(requires { T{...}; })`: clang
// reports "excess elements in struct initializer" as a HARD error when the
// over-long initializer appears in a non-template context, so the "N + 1 does
// not compile" half of an arity check has to be evaluated during template
// argument substitution, where it is a recoverable substitution failure.
//
// Internal (`detail/`): no stability promise, not part of the frozen umbrella.

#include <cstddef>
#include <initializer_list>
#include <type_traits>
#include <utility>

namespace atx::vol::detail {

// Stands in for "an initializer of whatever type this member happens to be".
// Declared, never defined: it is only ever named inside an unevaluated
// requires-expression, so no member has to be default-constructible (
// SurfaceParityReport::surface is a whole VolSurface).
//
// Three target types are excluded, each because admitting it would make the
// probe answer a question about overload resolution rather than about the field
// count:
//   * `Aggregate` itself — otherwise `Aggregate{AnyFieldInit{}}` selects the
//     copy/move constructor and every aggregate probes as arity 1;
//   * `std::nullptr_t` — a `shared_ptr` / `std::function` member would find both
//     `X(std::nullptr_t)` and `X(X&&)` viable and fail on the ambiguity;
//   * `std::initializer_list<E>` — the same story for every `std::vector`
//     member, which is constructible from both an initializer_list and an X&&.
template <typename Aggregate>
struct AnyFieldInit {
  template <typename Member>
  static constexpr bool kAdmissible =
      !std::is_same_v<Aggregate, std::remove_cvref_t<Member>> &&
      !std::is_same_v<std::nullptr_t, std::remove_cvref_t<Member>>;

  template <typename Member, typename = std::enable_if_t<kAdmissible<Member>>>
  operator Member() const; // probe only: declared, never defined

  template <typename E>
  operator std::initializer_list<E>() const = delete;
};

// Discards the pack index: the probe needs N fillers, not N distinct types.
template <typename Aggregate, std::size_t>
using AnyFieldInitAt = AnyFieldInit<Aggregate>;

template <typename Aggregate, typename Indices>
struct BraceInitArityHolds : std::false_type {};

template <typename Aggregate, std::size_t... I>
struct BraceInitArityHolds<Aggregate, std::index_sequence<I...>>
    : std::bool_constant<requires { Aggregate{AnyFieldInitAt<Aggregate, I>{}...}; }> {};

// True iff `Aggregate` accepts exactly N brace initializers — N is well-formed
// and N + 1 is not. Fewer than N always compiles (the trailing members fall back
// to their default member initializers), so only the upper edge identifies the
// count.
template <typename Aggregate, std::size_t N>
inline constexpr bool aggregate_arity_is_v =
    BraceInitArityHolds<Aggregate, std::make_index_sequence<N>>::value &&
    !BraceInitArityHolds<Aggregate, std::make_index_sequence<N + 1>>::value;

} // namespace atx::vol::detail
