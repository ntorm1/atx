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
// mined-v1 rho check (at most 64). An empty pool (no --pool) has neither.
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
// Loads every payload (extent and SHA-256 verified while reading) after binding the manifest to
// `role`: its manifest SHA-256, dates and instruments.
[[nodiscard]] atx::core::Result<MinePool> load_mine_pool(const MinePoolManifest &manifest,
                                                         const ResearchRole &role);

} // namespace atx::impl::strategy
