#pragma once
// atx::impl::strategy — the pool of a mining campaign (platform v8 H-3, `atx.mine-pool/v1`): what
// the book already has, bound to the campaign's role.
//
//   {"schema": "atx.mine-pool/v1", "status": "complete", "role_manifest_sha256": SHA,
//    "dates": D, "instruments": N,
//    "regressors": [{"name": ID, "file": ID.f64, "sha256": SHA, "bytes": D*N*8}, ...],
//    "members":    [{"name": ID, "file": ID.f64, "sha256": SHA, "bytes": D*N*8}, ...]}
//
// Payloads are date-major little-endian f64 beside the manifest, NaN where undefined (inf is
// refused). Regressors enter the marginal term as given (the book composite, theme composites;
// at most 11); members are the book's signals, ranked over the decision members for the
// mined-v1 rho check (at most 64). An empty path reads as the empty pool (neither); the mining
// verb refuses it, and a manifest without a regressor or a member (Ruling E-32a, review MINE-7).
// Each regressor is loaded once: the fitness and the confirm read borrow it. No member is ever
// held whole: the rho check streams them date by date from their pinned payloads (lane MINE-JOIN).
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "strategy_research_role.hpp"

namespace atx::impl::strategy {

inline constexpr atx::usize kMaxMinePoolMembers = 64;

struct MinePoolFile {
  std::string name;
  std::string file;
  std::string sha256;
};

// The pinned manifest (metadata only, read before any payload for admission).
struct MinePoolManifest {
  std::string path;
  std::string sha256;
  std::string role_manifest_sha256;
  atx::usize dates{};
  atx::usize instruments{};
  std::vector<MinePoolFile> regressors;
  std::vector<MinePoolFile> members;
};

struct MinePoolColumn {
  std::string name;
  std::string sha256;
  std::vector<atx::f64> values; // panel cells, date-major
};

struct MinePool {
  std::string path;
  std::string sha256;
  atx::usize dates{}, instruments{}; // the bound role's axes (every payload is dates x instruments)
  std::vector<MinePoolColumn> regressors;
  std::vector<MinePoolFile> members; // pinned, never loaded whole (stream_mine_pool_members)
};

// Empty path: the empty pool. Otherwise the pin is required.
[[nodiscard]] atx::core::Result<MinePoolManifest> read_mine_pool_manifest(const std::string &path,
                                                                          const std::string &pin);
// Binds the manifest to `role` (its manifest SHA-256, dates and instruments; an empty manifest path
// is the empty pool), loads every regressor (extent and SHA-256 verified while reading, no
// infinity) and checks every member payload as check_mine_pool_members does, so a bad member is
// refused before the search. Lane MINE-MEM: the search holds no member.
[[nodiscard]] atx::core::Result<MinePool> bind_mine_pool(const MinePoolManifest &manifest,
                                                         const ResearchRole &role);
// Streams every member payload once through a fixed buffer and keeps nothing: the extent, the
// read, the extent again and the SHA-256 in the order and words of ic_detail::load_pinned_f64,
// then the infinity check, member by member in manifest order.
[[nodiscard]] atx::core::Status check_mine_pool_members(const MinePool &pool);

// One date of every member: rows[k] is member k's payload row `date` (pool.instruments values, as
// stored), in manifest order. The spans are valid only during the call.
using MinePoolRowsFn = std::function<atx::core::Status(
    atx::usize date, std::span<const std::span<const atx::f64>> rows)>;
// Lane MINE-JOIN: the members read date by date, all payloads in lockstep from date 0 to the last,
// one row of each held at a time (members x instruments values); `on_rows` is called for every date
// in [begin, end), in date order. Every payload is checked as check_mine_pool_members checks it:
// the extent at open, each read, and at the end the extent again, the SHA-256 and the infinity
// check (refusal words as there). A refusal at the end follows calls of `on_rows` on bytes that are
// then refused, so the caller keeps nothing it derived from them unless this returns Ok. Err also
// when [begin, end) is not inside the pool's dates, or when `on_rows` returns Err (returned as is).
[[nodiscard]] atx::core::Status stream_mine_pool_members(const MinePool &pool, atx::usize begin,
                                                         atx::usize end,
                                                         const MinePoolRowsFn &on_rows);

} // namespace atx::impl::strategy
