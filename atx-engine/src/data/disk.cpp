#include "atx/engine/data/disk.hpp"

#include <algorithm>
#include <system_error>

namespace atx::engine::data {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

std::string_view store_name(Store s) noexcept {
  switch (s) {
    case Store::Ohlc1D: return "OHLC1D";
    case Store::Ohlc1M: return "OHLC1M";
    case Store::OpraBbo: return "OPRA_BBO";
    case Store::OChain: return "OCHAIN";
  }
  return "UNKNOWN";
}

Result<DiskStore> DiskStore::open(std::filesystem::path root) {
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return Err(ErrorCode::IoError, "create root: " + ec.message());
  }
  DiskStore store{std::move(root)};
  for (const Store s : {Store::Ohlc1D, Store::Ohlc1M, Store::OpraBbo, Store::OChain}) {
    std::filesystem::create_directories(store.store_dir(s), ec);
    if (ec) {
      return Err(ErrorCode::IoError, "create store dir: " + ec.message());
    }
  }
  return Ok(std::move(store));
}

std::filesystem::path DiskStore::store_dir(Store s) const {
  return root_ / std::string{store_name(s)};
}

std::filesystem::path DiskStore::partition_path(Store s, std::string_view date) const {
  return store_dir(s) / ("date=" + std::string{date}) / "data.parquet";
}

Result<void> DiskStore::write_partition(Store s, std::string_view date,
                                        std::span<const core::io::WriteColumn> cols) const {
  const std::filesystem::path out = partition_path(s, date);
  std::error_code ec;
  std::filesystem::create_directories(out.parent_path(), ec);
  if (ec) {
    return Err(ErrorCode::IoError, "create partition dir: " + ec.message());
  }
  auto status = core::io::write_parquet(cols, out.string());
  if (!status.has_value()) {
    return Err(status.error());
  }
  return Ok();
}

Result<core::io::LazyParquet> DiskStore::scan_partition(Store s, std::string_view date) const {
  const std::filesystem::path p = partition_path(s, date);
  if (!std::filesystem::exists(p)) {
    return Err(ErrorCode::IoError, "partition not found: " + p.string());
  }
  return core::io::LazyParquet::scan(p.string());
}

Result<std::vector<std::string>> DiskStore::list_dates(Store s) const {
  std::vector<std::string> dates;
  const std::filesystem::path dir = store_dir(s);
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    return Ok(std::move(dates));
  }
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      return Err(ErrorCode::IoError, "iterate store: " + ec.message());
    }
    if (!e.is_directory()) {
      continue;
    }
    const std::string name = e.path().filename().string();
    constexpr std::string_view kPrefix = "date=";
    if (name.rfind(kPrefix, 0) == 0) {
      dates.emplace_back(name.substr(kPrefix.size()));
    }
  }
  std::sort(dates.begin(), dates.end());
  return Ok(std::move(dates));
}

} // namespace atx::engine::data
