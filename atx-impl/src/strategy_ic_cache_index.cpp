#include "strategy_ic_cache_index.hpp"

#include <mutex>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include "atx/core/db/connection.hpp"
#include "atx/core/db/sqlite.hpp"
#include "atx/engine/research/store/ops_cache.hpp"
#include "atx/engine/research/store/store.hpp"

namespace atx::impl::strategy {
namespace {
namespace co = atx::core;
namespace db = atx::core::db;
namespace st = atx::engine::research::store;
namespace fs = std::filesystem;

constexpr std::string_view miss_note = "unreadable: treated as a miss";
constexpr std::string_view skip_note = "store skipped";

// The failure policy (header): a busy or I/O failure is a miss (read) or a skipped store (write).
[[nodiscard]] bool transient(const co::Error& e) noexcept {
  return e.code() == co::ErrorCode::Unavailable || e.code() == co::ErrorCode::IoError;
}
} // namespace

std::optional<CacheIndexMode> parse_cache_index_mode(std::string_view text) noexcept {
  if (text == "files") return CacheIndexMode::Files;
  if (text == kCacheIndexSqlite) return CacheIndexMode::Sqlite;
  return std::nullopt;
}

bool cache_index_present(const fs::path& dir) {
  std::error_code ec;
  const bool present = fs::is_regular_file(dir / std::string(st::kCacheIndexName), ec);
  return present && !ec;
}

// The connection and the one mutex that guards it (the header's lock order: a leaf lock, held
// only across statements on `connection`). `dir`, `progress` and `access` never change after open.
struct CacheIndex::Impl {
  Impl(fs::path where, std::ostream* sink, IndexAccess mode, db::Database opened)
      : dir{std::move(where)}, progress{sink}, access{mode}, connection{std::move(opened)} {}
  const fs::path dir;
  std::ostream* const progress;
  const IndexAccess access;
  std::mutex mutex;
  db::Database connection; // guarded by `mutex`

  // `fn(connection)` under the lock; the lock is released before the result is used.
  template <class Fn> auto with_connection(Fn&& fn) {
    const std::scoped_lock lock(mutex);
    return fn(connection);
  }
  // One progress line; called with the lock released (the stream is the caller's).
  void line(std::string_view table, std::string_view key, std::string_view what) const {
    if (progress == nullptr) return;
    *progress << "cache-index " << (dir / std::string(st::kCacheIndexName)).string() << ": "
              << table << ' ' << key << ": " << what << '\n'
              << std::flush;
  }
  void note(std::string_view table, std::string_view key, const co::Error& e,
            std::string_view outcome) const {
    line(table, key, e.to_string() + "; " + std::string(outcome));
  }
  // A keyed read under the policy: a transient failure is "no row".
  template <class Row>
  co::Result<std::optional<Row>> read(co::Result<std::optional<Row>> found, std::string_view table,
                                      std::string_view key) const {
    if (found || !transient(found.error())) return found;
    note(table, key, found.error(), miss_note);
    return co::Ok(std::optional<Row>{});
  }
  // `fn(connection)` in one BEGIN IMMEDIATE (with_immediate: bounded retry on SQLITE_BUSY) under
  // the lock. true: committed; false: skipped by the policy (a progress line written).
  template <class Fn>
  co::Result<bool> write(std::string_view table, std::string_view key, Fn&& fn) {
    co::Status status = [&] {
      const std::scoped_lock lock(mutex);
      return db::with_immediate(connection, fn);
    }();
    if (status) return co::Ok(true);
    if (!transient(status.error())) return co::Err(std::move(status).error());
    note(table, key, status.error(), skip_note);
    return co::Ok(false);
  }
};

CacheIndex::CacheIndex(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
CacheIndex::CacheIndex(CacheIndex&& other) noexcept = default;
CacheIndex& CacheIndex::operator=(CacheIndex&& other) noexcept = default;
CacheIndex::~CacheIndex() {
  if (!impl_ || impl_->access != IndexAccess::ReadWrite) return;
  const std::scoped_lock lock(impl_->mutex);
  // Best effort (sql-design 3.3): another process's reader can keep the WAL; never an error.
  (void)db::checkpoint_truncate(impl_->connection);
}

co::Result<CacheIndex> CacheIndex::open(const fs::path& dir, std::ostream* progress,
                                        IndexAccess access) {
  ATX_TRY(auto connection, st::open_cache(dir));
  ATX_TRY_VOID(
      connection.pragma("cache_size", std::to_string(-(kCacheIndexPageCacheBytes >> 10))));
  return co::Ok(CacheIndex(std::make_unique<Impl>(dir, progress, access, std::move(connection))));
}

const fs::path& CacheIndex::directory() const noexcept { return impl_->dir; }
bool CacheIndex::writable() const noexcept { return impl_->access == IndexAccess::ReadWrite; }

co::Result<std::string> CacheIndex::relative(const fs::path& path) const {
  const auto rel = path.lexically_normal().lexically_relative(impl_->dir.lexically_normal());
  bool inside = !rel.empty() && !rel.is_absolute() && !rel.has_root_name();
  for (const auto& part : rel) inside = inside && part != ".." && part != ".";
  if (!inside)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "cache index: " + path.string() + " is outside " + impl_->dir.string());
  return co::Ok(rel.generic_string());
}

void CacheIndex::note(std::string_view table, std::string_view key, std::string_view what) const {
  impl_->line(table, key, what);
}

co::Result<std::optional<st::IcSignalRow>> CacheIndex::find_signal(std::string_view key_sha256) {
  auto found = impl_->with_connection(
      [&](db::Database& c) { return st::select_ic_signal(c, key_sha256); });
  return impl_->read(std::move(found), "ic_signal", key_sha256);
}

co::Result<IndexPutResult<st::IcSignalRow>> CacheIndex::put_signal(const st::IcSignalRow& row,
                                                                   bool replace) {
  IndexPutResult<st::IcSignalRow> out;
  if (!writable()) return co::Ok(std::move(out));
  const auto txn = [&](db::Database& c) -> co::Status {
    out = {};
    if (replace) {
      ATX_TRY_VOID(st::upsert(c, row));
      out.outcome = IndexPut::Inserted;
      return co::Ok();
    }
    ATX_TRY(const bool inserted, st::insert_or_ignore(c, row));
    if (!inserted) {
      ATX_TRY(out.existing, st::select_ic_signal(c, row.key_sha256));
    }
    out.outcome = inserted ? IndexPut::Inserted : IndexPut::Present;
    return co::Ok();
  };
  ATX_TRY(const bool done, impl_->write("ic_signal", row.key_sha256, txn));
  if (!done) out = {};
  return co::Ok(std::move(out));
}

co::Result<std::optional<st::IcResultRow>> CacheIndex::find_ic_result(std::string_view entry_dir,
                                                                      std::string_view id) {
  auto found = impl_->with_connection(
      [&](db::Database& c) { return st::select_ic_result(c, entry_dir, id); });
  return impl_->read(std::move(found), "ic_result", id);
}

co::Result<IndexPutResult<st::IcResultRow>> CacheIndex::put_ic_result(const st::IcResultRow& row,
                                                                      bool replace) {
  IndexPutResult<st::IcResultRow> out;
  if (!writable()) return co::Ok(std::move(out));
  const auto txn = [&](db::Database& c) -> co::Status {
    out = {};
    if (replace) {
      ATX_TRY_VOID(st::upsert(c, row));
      out.outcome = IndexPut::Inserted;
      return co::Ok();
    }
    ATX_TRY(const bool inserted, st::insert_or_ignore(c, row));
    if (!inserted) {
      ATX_TRY(out.existing, st::select_ic_result(c, row.entry_dir, row.id));
    }
    out.outcome = inserted ? IndexPut::Inserted : IndexPut::Present;
    return co::Ok();
  };
  ATX_TRY(const bool done, impl_->write("ic_result", row.id, txn));
  if (!done) out = {};
  return co::Ok(std::move(out));
}

co::Result<std::optional<st::PairKeyRow>> CacheIndex::find_pair_key(std::string_view key_sha256) {
  auto found = impl_->with_connection(
      [&](db::Database& c) { return st::select_pair_key(c, key_sha256); });
  return impl_->read(std::move(found), "pair_key", key_sha256);
}

co::Result<std::optional<std::vector<st::PairStatRow>>>
CacheIndex::pair_stats_page(std::string_view key_sha256, std::string_view after_lo,
                            std::string_view after_hi) {
  using Rows = std::vector<st::PairStatRow>;
  auto page = impl_->with_connection([&](db::Database& c) {
    return st::select_pair_stats_page(c, key_sha256, after_lo, after_hi, kPairPageRows);
  });
  if (page) return co::Ok(std::optional<Rows>{std::move(*page)});
  if (!transient(page.error())) return co::Err(std::move(page).error());
  impl_->note("pair_stat", key_sha256, page.error(), miss_note);
  return co::Ok(std::optional<Rows>{});
}

co::Result<PairsPut> CacheIndex::put_pairs(std::string_view key_sha256,
                                           std::span<const st::PairStatRow> rows,
                                           const st::PairKeyRow* key) {
  PairsPut out;
  if (!writable()) return co::Ok(out);
  const auto refuse = [&](const std::string& what) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "cache index " + impl_->dir.string() + ": " + what);
  };
  const auto txn = [&](db::Database& c) -> co::Status {
    out = {};
    // Bounded by rows.size().
    for (const auto& row : rows) {
      if (row.key_sha256 != key_sha256 || row.payload_hi < row.payload_lo)
        return refuse("pair_stat row outside its key or not ordered: " + row.payload_lo + " " +
                      row.payload_hi);
      ATX_TRY(const bool inserted, st::insert_or_ignore(c, row));
      if (inserted) {
        ++out.inserted;
        continue;
      }
      ATX_TRY(const auto held,
              st::select_pair_stat(c, row.key_sha256, row.payload_lo, row.payload_hi));
      if (!held || held->sum_bits != row.sum_bits || held->dates != row.dates)
        return refuse("one pair, two statistics: " + row.payload_lo + " " + row.payload_hi);
    }
    if (key != nullptr) {
      if (key->key_sha256 != key_sha256) return refuse("pair_key of another key");
      ATX_TRY(const bool fresh_key, st::insert_or_ignore(c, *key));
      if (!fresh_key) {
        ATX_TRY(const auto held, st::select_pair_key(c, key->key_sha256));
        if (!held || held->version != key->version || held->key_text != key->key_text)
          return refuse("pair_key " + key->key_sha256 + " holds another key text");
      }
    }
    out.outcome = IndexPut::Inserted;
    return co::Ok();
  };
  ATX_TRY(const bool done, impl_->write("pair_stat", key_sha256, txn));
  if (!done) out = {};
  return co::Ok(out);
}

} // namespace atx::impl::strategy
