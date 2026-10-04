#pragma once

// The store_info descriptor, shared by every group whose store needs a self description
// (catalog_core and cache). PRIVATE header: included by tables_core.hpp / tables_cache.hpp.
// Its operations are instantiated once, in core_ops.cpp.

#include "atx/engine/research/store/rows_core.hpp"
#include "research/store/detail/table.hpp"

namespace atx::engine::research::store {

// Volatile: a store's self description (schema text, creating executable) is not catalog
// content, so it stays out of the catalog digest.
inline constexpr auto kStoreInfoTable = table<StoreInfoRow>(
    "store_info",
    TableOpts{.version = 1, .since = 1, .append_only = false, .volatile_table = true, .check = {}},
    col<Sql::Text>("key", &StoreInfoRow::key, kKey),
    col<Sql::Text>("value", &StoreInfoRow::value));

} // namespace atx::engine::research::store
