#pragma once

// atx::core util — small zero-cost RAII and type-safety utilities.
//
// Provides:
//   ScopeGuard<F>    — RAII callable: runs F on destruction unless dismissed.
//   make_scope_guard — factory deducing F.
//   ATX_DEFER(stmt)  — macro: run stmt at scope exit (built on ATX_UNIQUE_NAME).
//   NonNull<T*>      — non-owning pointer wrapper that asserts non-null.
//   EnumFlags<E>     — type-safe bitfield over an enum class E.
//   to_underlying(e) — free function mirroring std::to_underlying (C++23).
//
// Design notes:
//   - ScopeGuard is move-only (Rule of Five); the moved-from guard is disarmed.
//   - NonNull does not own the pointed-to object; it is a borrow/observer.
//   - EnumFlags stores the underlying integer; all operators return new values.
//   - to_underlying is provided even when <utility> has std::to_underlying so
//     callers don't need a C++23 feature-test macro.

#include <type_traits>
#include <utility>

#include "atx/core/macro.hpp" // ATX_ASSERT, ATX_UNIQUE_NAME, ATX_CONCAT

namespace atx::core {

// =============================================================================
// ScopeGuard<F>
// =============================================================================

/// RAII scope-exit guard: invokes F when it goes out of scope unless dismissed.
///
/// @tparam F  Callable type; must be nothrow-invocable (destructor is noexcept).
///
/// Preconditions: none (F is always called unless dismiss() has been called).
/// Postcondition: F is invoked at most once (move disarms the source).
/// Thread-safety: not thread-safe (single-owner, stack-local idiom).
template <typename F>
class ScopeGuard {
public:
    // Construction: takes the callable and arms the guard.
    explicit ScopeGuard(F f) noexcept(std::is_nothrow_move_constructible_v<F>)
        : fn_{std::move(f)}, armed_{true} {}

    // Rule of Five — copy disabled; move transfers ownership.
    ATX_DISABLE_COPY(ScopeGuard);

    ScopeGuard(ScopeGuard &&other) noexcept(
        std::is_nothrow_move_constructible_v<F>)
        : fn_{std::move(other.fn_)}, armed_{other.armed_} {
        other.armed_ = false;
    }

    /// Move-assign: run the current guard's callback (if armed), then take over
    /// the source guard. The source is left disarmed.
    ///
    /// noexcept reflects the body precisely: it invokes fn_() and then
    /// move-assigns fn_, so it is nothrow only when both operations are.
    /// (Overstating noexcept would force std::terminate on a real throw.)
    ScopeGuard &operator=(ScopeGuard &&other) noexcept(
        std::is_nothrow_invocable_v<F> &&std::is_nothrow_move_assignable_v<F>) {
        if (this != &other) {
            if (armed_) {
                fn_(); // run our own cleanup before adopting the new one
            }
            fn_    = std::move(other.fn_);
            armed_ = other.armed_;
            other.armed_ = false;
        }
        return *this;
    }

    /// Destructor: fires the callback iff still armed.
    ///
    /// noexcept: ScopeGuard is designed for stack-local RAII; a throwing
    /// destructor during stack unwinding would call std::terminate, which is
    /// worse than the original exception. Callers must supply a non-throwing F.
    ~ScopeGuard() noexcept {
        if (armed_) {
            fn_();
        }
    }

    /// Cancel the guard: the callable will NOT be invoked on destruction.
    /// Idempotent — safe to call multiple times.
    void dismiss() noexcept { armed_ = false; }

private:
    F    fn_;
    bool armed_;
};

// =============================================================================
// make_scope_guard
// =============================================================================

/// Factory: deduces F so callers don't need to spell the type.
///
/// @param f  Callable (lambda, function pointer, functor) executed at scope exit.
/// @return   An armed ScopeGuard<F>.
///
/// Example:
///   auto g = make_scope_guard([&] { cleanup(); });
template <typename F>
[[nodiscard]] ScopeGuard<F> make_scope_guard(F f) noexcept(
    std::is_nothrow_move_constructible_v<F>) {
    return ScopeGuard<F>{std::move(f)};
}

// =============================================================================
// ATX_DEFER
// =============================================================================

/// ATX_DEFER(stmt...) — execute stmt at the end of the enclosing scope.
///
/// Built on ATX_UNIQUE_NAME so multiple ATX_DEFER on the same or adjacent lines
/// each bind to a uniquely-named ScopeGuard variable.
///
/// Example:
///   FILE* f = fopen("x", "r");
///   ATX_DEFER(fclose(f));
///
// SAFETY: The lambda captures by reference; the captured variables must outlive
//         the scope. This is the same contract as any RAII guard.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) — language can't express this
#define ATX_DEFER(...)                                                         \
    auto ATX_UNIQUE_NAME(_atx_defer_) =                                        \
        ::atx::core::make_scope_guard([&]() noexcept { __VA_ARGS__; })

// =============================================================================
// NonNull<T*>
// =============================================================================

/// Non-owning pointer wrapper that asserts non-null at construction time.
///
/// @tparam T  A raw pointer type, e.g. NonNull<int*>.
///
/// Invariant: get() != nullptr — enforced by ATX_ASSERT at construction.
/// Ownership: does NOT own the pointed-to object (borrow/observer pattern).
/// Thread-safety: same rules as the underlying raw pointer.
template <typename T>
    requires std::is_pointer_v<T>
class NonNull {
public:
    using element_type = std::remove_pointer_t<T>;

    /// Construct from a raw pointer. ATX_ASSERT fires in debug if ptr == nullptr.
    ///
    /// @param ptr  A non-null pointer. Passing nullptr is a precondition
    ///             violation; behaviour in release builds is undefined (the
    ///             invariant is considered always upheld at that level).
    explicit NonNull(T ptr) noexcept : ptr_{ptr} { ATX_ASSERT(ptr_ != nullptr); }

    // Deleted: constructing from nullptr is a compile-time error, not a runtime
    // one. Makes illegal states unrepresentable.
    NonNull(std::nullptr_t) = delete;

    // No default constructor — a NonNull with an unset pointer is incoherent.
    NonNull() = delete;

    // Value semantics: copyable (it's just a non-owning observer).
    NonNull(const NonNull &)            = default;
    NonNull &operator=(const NonNull &) = default;
    NonNull(NonNull &&)                 = default;
    NonNull &operator=(NonNull &&)      = default;
    ~NonNull()                          = default;

    /// Dereference. Pre: invariant holds (always true by construction).
    [[nodiscard]] element_type &operator*() const noexcept { return *ptr_; }

    /// Member access.
    [[nodiscard]] T operator->() const noexcept { return ptr_; }

    /// Raw pointer accessor.
    [[nodiscard]] T get() const noexcept { return ptr_; }

private:
    T ptr_;
};

// =============================================================================
// to_underlying (free function)
// =============================================================================

/// Convert an enum to its underlying integral type.
///
/// Mirrors std::to_underlying (C++23) so callers can use the atx:: spelling
/// uniformly regardless of which C++ standard revision is active.
///
/// @param e  An enumerator value.
/// @return   The value cast to std::underlying_type_t<E>.
template <typename E>
    requires std::is_enum_v<E>
[[nodiscard]] constexpr std::underlying_type_t<E> to_underlying(E e) noexcept {
    return static_cast<std::underlying_type_t<E>>(e);
}

// =============================================================================
// EnumFlags<E>
// =============================================================================

/// Type-safe bitfield over an enum class E.
///
/// @tparam E  An enum class whose underlying type is an unsigned integer.
///            Enumerators should be powers of two.
///
/// All bitwise operations return new EnumFlags values; the original is not
/// mutated (value semantics). Mutating helpers (set/clear) operate in-place.
///
/// Example:
///   enum class Perm : unsigned { Read = 1U, Write = 2U, Exec = 4U };
///   auto f = EnumFlags<Perm>{Perm::Read} | Perm::Write;
///   f.test(Perm::Read); // true
template <typename E>
    requires std::is_enum_v<E> &&
             std::is_unsigned_v<std::underlying_type_t<E>>
class EnumFlags {
public:
    using underlying_type = std::underlying_type_t<E>;

    /// Default: all bits clear.
    constexpr EnumFlags() noexcept : bits_{0U} {}

    /// Construct from a single enumerator.
    constexpr explicit EnumFlags(E e) noexcept
        : bits_{::atx::core::to_underlying(e)} {}

    /// Construct directly from the raw underlying value (internal/advanced use).
    constexpr explicit EnumFlags(underlying_type raw) noexcept : bits_{raw} {}

    // Value semantics.
    constexpr EnumFlags(const EnumFlags &)            = default;
    constexpr EnumFlags &operator=(const EnumFlags &) = default;
    constexpr EnumFlags(EnumFlags &&)                 = default;
    constexpr EnumFlags &operator=(EnumFlags &&)      = default;
    ~EnumFlags()                                       = default;

    // ---- Bitwise operators --------------------------------------------------

    [[nodiscard]] constexpr EnumFlags operator|(EnumFlags rhs) const noexcept {
        return EnumFlags{static_cast<underlying_type>(bits_ | rhs.bits_)};
    }
    [[nodiscard]] constexpr EnumFlags operator|(E rhs) const noexcept {
        return *this | EnumFlags{rhs};
    }
    [[nodiscard]] constexpr EnumFlags operator&(EnumFlags rhs) const noexcept {
        return EnumFlags{static_cast<underlying_type>(bits_ & rhs.bits_)};
    }
    [[nodiscard]] constexpr EnumFlags operator&(E rhs) const noexcept {
        return *this & EnumFlags{rhs};
    }
    [[nodiscard]] constexpr EnumFlags operator~() const noexcept {
        // Apply NOT on the unsigned underlying type to avoid integer promotion
        // to a signed int (which would make the operation/cast value-dependent).
        return EnumFlags{
            static_cast<underlying_type>(~static_cast<underlying_type>(bits_))};
    }

    // ---- Mutating helpers ---------------------------------------------------

    /// Set the bit(s) corresponding to e.
    constexpr void set(E e) noexcept {
        bits_ = static_cast<underlying_type>(
            bits_ | ::atx::core::to_underlying(e));
    }

    /// Clear the bit(s) corresponding to e.
    constexpr void clear(E e) noexcept {
        bits_ = static_cast<underlying_type>(
            bits_ & static_cast<underlying_type>(
                ~::atx::core::to_underlying(e)));
    }

    // ---- Query --------------------------------------------------------------

    /// Returns true iff all bits in e are set.
    [[nodiscard]] constexpr bool test(E e) const noexcept {
        const auto mask =
            static_cast<underlying_type>(::atx::core::to_underlying(e));
        return (bits_ & mask) == mask;
    }

    /// Returns the raw underlying integer value.
    [[nodiscard]] constexpr underlying_type to_underlying() const noexcept {
        return bits_;
    }

private:
    underlying_type bits_;
};

// Free-function operator| so you can write: Perm::Read | Perm::Write
// (both operands are E, not yet EnumFlags).
template <typename E>
    requires std::is_enum_v<E> &&
             std::is_unsigned_v<std::underlying_type_t<E>>
[[nodiscard]] constexpr EnumFlags<E> operator|(E lhs, E rhs) noexcept {
    return EnumFlags<E>{lhs} | rhs;
}

} // namespace atx::core
