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
// Each payload is stored once: the fitness and the confirm read borrow the regressors, the rho
// check reads the members in place.
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
  std::vector<MinePoolColumn> regressors;
  std::vector<MinePoolColumn> members;
};

// Empty path: the empty pool. Otherwise the pin is required.
[[nodiscard]] atx::core::Result<MinePoolManifest> read_mine_pool_manifest(const std::string &path,
                                                                          const std::string &pin);
// The pool in two steps (lane MINE-MEM): only the promotion's rho check reads the members, so the
// search does not hold them. Both bind the manifest to `role` first (its manifest SHA-256, dates
// and instruments); an empty manifest path is the empty pool.
// bind_mine_pool loads every regressor (extent and SHA-256 verified while reading, no infinity)
// and streams every member payload once through a fixed buffer -- the same checks, in the same
// order and words -- without keeping it, so a bad member is still refused before the search.
// load_mine_pool_members then loads the members into `pool`, verifying each again as it is read.
[[nodiscard]] atx::core::Result<MinePool> bind_mine_pool(const MinePoolManifest &manifest,
                                                         const ResearchRole &role);
[[nodiscard]] atx::core::Status load_mine_pool_members(const MinePoolManifest &manifest,
                                                       const ResearchRole &role, MinePool &pool);

} // namespace atx::impl::strategy
