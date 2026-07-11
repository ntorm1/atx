#include "cached_artifacts.hpp"

#include "atx/vol/session.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/vol_curve.hpp"
#include "opra_fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <random>
#include <string>
#include <system_error>

namespace atx::vol::test {
namespace fs = std::filesystem;

fs::path cached_spy_convex_dense() {
  const fs::path dir{"artifact-cache"};  // under the ctest CWD (build tree)
  std::error_code ec;
  fs::create_directories(dir, ec);

  // Suffixed with kArchiveMajor (the ATXVSA on-wire format version, from
  // surface_archive.hpp) so a format bump invalidates any stale cache built by
  // an older binary.
  const fs::path file =
      dir / ("spy_convexdense_nc40_v" + std::to_string(kArchiveMajor) + ".atxvsa");
  if (fs::exists(file)) {
    return file;
  }

  auto board = atx::vol::testkit::load_opra_board("spy", "SPY");
  if (!board.has_value()) {
    return {};  // caller GTEST_SKIPs, same as today
  }

  // The 99.5% recipe — verbatim from spy_archive_roundtrip_test.cpp /
  // spy_bidask_regression_test.cpp: Fast preset, ConvexDense, node_cap 40.
  SessionInputs in = make_session_inputs(FitPreset::Fast, board->spot(), board->r,
                                         board->now_ns());
  in.cash_divs = board->panel.frame.divs;
  in.curve.kind = VolCurveKind::ConvexDense;
  in.curve.convex.node_cap = 40;

  auto sess = VolaSession::build(board->underlying(), in);
  if (!sess.has_value()) {
    ADD_FAILURE() << sess.error().to_string();
    return {};
  }
  auto priced = sess->to_priced_surface();
  if (!priced.has_value()) {
    ADD_FAILURE() << priced.error().to_string();
    return {};
  }

  // Atomic publish so concurrent ctest -j misses never observe a torn file:
  // write to a per-process-random tmp name, then rename into place. A losing
  // rename means another process already published a byte-identical archive
  // (same fit recipe on the same fixture) — the loser just discards its tmp.
  std::mt19937_64 rng{std::random_device{}()};
  const fs::path tmp =
      dir / (file.filename().string() + "." + std::to_string(rng()) + ".tmp");
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &*priced}};
  auto wrote = write_surface_archive_file(tmp.string(), items);
  if (!wrote.has_value()) {
    ADD_FAILURE() << wrote.error().to_string();
    fs::remove(tmp, ec);
    return {};
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    fs::remove(tmp, ec);  // lost the race: someone else published — fine
  }
  return file;
}

}  // namespace atx::vol::test
