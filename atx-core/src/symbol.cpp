// Out-of-line body for atx/core/domain/symbol.hpp.
//
// The id<->name storage lives here (behind the header's pimpl) so the heavy
// hash-map / container includes stay out of the public header, per the
// atx-core layout convention.
//
// Storage model:
//   names_  : vector<unique_ptr<string>> — id (index) -> owned name. Each name
//             is heap-allocated individually so a string_view returned by
//             name() stays valid even as the vector grows and reallocates its
//             pointer array (only the pointers move, never the strings).
//   index_  : HashMap<string_view, u32> — name -> id, for idempotent intern().
//             The view keys borrow the strings owned by names_, whose lifetime
//             (heap-stable per above) encloses the map's.

#include "atx/core/domain/symbol.hpp"

#include <cstdint>     // UINT32_MAX
#include <memory>      // std::make_unique, std::unique_ptr
#include <string>      // std::string
#include <string_view> // std::string_view
#include <vector>      // std::vector

#include "atx/core/container/hash_map.hpp" // HashMap
#include "atx/core/macro.hpp"              // ATX_CHECK
#include "atx/core/types.hpp"              // u32, usize

namespace atx::core::domain {

struct SymbolTable::Impl {
  // id -> owned name (heap-stable backing for the views handed out by name()).
  std::vector<std::unique_ptr<std::string>> names;
  // name -> id. Keys are views into the strings owned by `names` above; that
  // owner outlives this map, so the views never dangle.
  container::HashMap<std::string_view, u32> index;
};

SymbolTable::SymbolTable() : impl_{std::make_unique<Impl>()} {}
SymbolTable::~SymbolTable() = default;
SymbolTable::SymbolTable(SymbolTable &&) noexcept = default;
SymbolTable &SymbolTable::operator=(SymbolTable &&) noexcept = default;

Symbol SymbolTable::intern(std::string_view name) {
  if (const auto it = impl_->index.find(name); it != impl_->index.end()) {
    return Symbol{it->second};
  }
  // New name: assign the next id. size() bounds the id space to u32; a table
  // with >4e9 distinct symbols is a programmer error, not an expected case.
  const usize next = impl_->names.size();
  ATX_CHECK(next <= static_cast<usize>(UINT32_MAX));
  const auto id = static_cast<u32>(next);

  // Own the name first, then key the index by a view into that stable storage.
  impl_->names.push_back(std::make_unique<std::string>(name));
  const std::string_view stable_key{*impl_->names.back()};
  impl_->index.emplace(stable_key, id);
  return Symbol{id};
}

std::string_view SymbolTable::name(Symbol sym) const {
  // PRECONDITION: the Symbol was interned by this table.
  ATX_CHECK(sym.id < impl_->names.size());
  return std::string_view{*impl_->names[sym.id]};
}

usize SymbolTable::size() const noexcept { return impl_->names.size(); }

} // namespace atx::core::domain
