// cluster_scaffold_test.cpp — S11-0 compile/link gate.
//
// Proves the four declaration-only S11 scaffold headers parse, their config /
// result types are constructible, and their enum defaults are what S11-1..S11-4
// will build on. No algorithm is exercised: the free functions
// (rmt_clean / cluster / build_cluster_panel / compute_params_hash) are only
// DECLARED in S11-0, so this unit constructs the value types and asserts the
// frozen defaults rather than calling into unimplemented logic.

#include <vector>

#include <gtest/gtest.h>

#include "atx/core/cluster/cluster.hpp"
#include "atx/core/linalg/rmt_clean.hpp"
#include "atx/engine/alpha/cluster_panel.hpp"
#include "atx/engine/store/cluster_panel.hpp"

namespace atxtest_store_cluster_scaffold_test {

// Constructing every scaffold config / result type and asserting the frozen
// enum + numeric defaults documents the seam S11-1..S11-4 inherit. If any default
// shifts, this test changes alongside it (a deliberate signal, not a silent drift).
TEST(ClusterScaffold, DefaultsAndConstructibility) {
  // atx-core RMT cleaner: default mode is the hard MP clip.
  atx::core::linalg::RmtConfig rmt{};
  EXPECT_EQ(rmt.mode, atx::core::linalg::RmtConfig::Mode::Clip);
  EXPECT_EQ(rmt.ridge, 0.0);
  atx::core::linalg::CleanedCorr cleaned{};
  EXPECT_EQ(cleaned.clipped, 0);

  // atx-core clusterer: default algorithm Hierarchical, default linkage Ward.
  atx::core::cluster::ClusterConfig cc{};
  EXPECT_EQ(cc.algo, atx::core::cluster::Algo::Hierarchical);
  EXPECT_EQ(cc.linkage, atx::core::cluster::Linkage::Ward);
  atx::core::cluster::Clustering clustering{};
  EXPECT_EQ(clustering.n_labels, 0);
  EXPECT_TRUE(clustering.cluster_id.empty());

  // engine cluster panel: default residualization is raw returns.
  atx::engine::alpha::ClusterPanelConfig pc{};
  EXPECT_EQ(pc.residualize, atx::engine::alpha::ClusterPanelConfig::Residualize::None);
  atx::engine::alpha::ClusterPanel panel{};
  EXPECT_TRUE(panel.snapshots.empty());
  EXPECT_EQ(atx::engine::alpha::ClusterPanel::kUnclustered, -1);

  // store record: integer columns default to zero, ready for register_panel.
  atx::engine::store::ClusterPanelRecord rec{};
  EXPECT_EQ(rec.params_hash, 0u);
  EXPECT_EQ(rec.k, 0);

  SUCCEED();
}

}  // namespace atxtest_store_cluster_scaffold_test
