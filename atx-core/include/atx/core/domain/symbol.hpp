#pragma once

// atx::core::domain Symbol / SymbolTable — string interning for instrument names.
//
// A Symbol is a 32-bit opaque id; comparing or hashing instruments is then a
// single-word integer op instead of a string compare. SymbolTable owns the
// canonical id<->name mapping: intern() is idempotent (equal strings map to the
// same id) and O(1) amortised.
//
// ------------------------------------------------------------------
//  Design contract
// ------------------------------------------------------------------
//   intern(name)  : returns the existing Symbol for an already-seen name, else
//                   assigns the next id and stores the name. Amortised O(1).
//   name(sym)     : the string for a previously-interned Symbol.
//                   PRECONDITION: sym.id < size() (i.e. sym came from THIS
//                   table). Violation ABORTS in debug (ATX_CHECK); it is a
//                   programmer error, not an expected runtime failure.
//   size()        : number of distinct interned symbols.
//
// The heavy storage (hash map + name vector) is hidden behind a pimpl so this
// header stays light — callers that only pass Symbols around do not pull in the
// hash-map / container machinery.
//
// Thread-safety: NONE. Interning mutates shared state; synchronise externally.

#include <memory>      // std::unique_ptr (pimpl)
#include <string_view> // std::string_view (intern input / name output)

#include "atx/core/types.hpp" // u32, usize

namespace atx::core::domain {

// =====================================================================
//  Symbol — opaque interned instrument id.
// =====================================================================
struct Symbol {
  u32 id{};

  [[nodiscard]] friend constexpr bool operator==(Symbol, Symbol) noexcept = default;
  [[nodiscard]] friend constexpr auto operator<=>(Symbol, Symbol) noexcept = default;
};

// =====================================================================
//  SymbolTable — owns the id<->name mapping.
// =====================================================================
class SymbolTable {
public:
  SymbolTable();
  ~SymbolTable();

  // Move-only: the table owns unique storage; copying it would duplicate the
  // id space (Rule of Five, explicit — agent §1).
  SymbolTable(SymbolTable &&) noexcept;
  SymbolTable &operator=(SymbolTable &&) noexcept;
  SymbolTable(const SymbolTable &) = delete;
  SymbolTable &operator=(const SymbolTable &) = delete;

  /// Intern `name`, returning a stable Symbol. Idempotent: equal names yield
  /// the same Symbol across calls. Amortised O(1).
  [[nodiscard]] Symbol intern(std::string_view name);

  /// The name for a Symbol from THIS table. PRECONDITION: sym.id < size()
  /// (ABORTS in debug). The returned view is valid until this table is
  /// destroyed or moved from; interning more symbols never invalidates it
  /// (names are stored as stable, individually-owned strings).
  [[nodiscard]] std::string_view name(Symbol sym) const;

  /// Number of distinct interned symbols.
  [[nodiscard]] usize size() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::core::domain
