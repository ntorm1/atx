#include "cached_artifacts.hpp"

#include "atx/vol/chain.hpp"
#include "atx/vol/corpus.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/vol_curve.hpp"
#include "opra_fixture.hpp"
#include "spy_fit_fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <random>
#include <string>
#include <system_error>

namespace atx::vol::test {
namespace fs = std::filesystem;

fs::path cached_spy_convex_dense() {
  const fs::path dir{"artifact-cache"}; // under the ctest CWD (build tree)
  std::error_code ec;
  fs::create_directories(dir, ec);

  // `shapev3` is the fitted-surface behavior revision, independent of the
  // ATXVSA wire-format version. Bump it whenever an intentional fit-policy or
  // curve-shape change makes an archive from an older binary semantically stale;
  // kArchiveMajor alone cannot detect that case.
  const fs::path file =
      dir / ("spy_convexdense_nc40_shapev3_v" + std::to_string(kArchiveV2Major) + ".atxvsa");
  if (fs::exists(file)) {
    return file;
  }

  auto board = atx::vol::testkit::load_opra_board("spy", "SPY");
  if (!board.has_value()) {
    return {}; // caller GTEST_SKIPs, same as today
  }

  // The 99.5% recipe — verbatim from spy_archive_roundtrip_test.cpp /
  // spy_bidask_regression_test.cpp: Fast preset, ConvexDense, node_cap 40.
  SessionInputs in = make_session_inputs(FitPreset::Fast, board->spot(), board->r, board->now_ns());
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
  const fs::path tmp = dir / (file.filename().string() + "." + std::to_string(rng()) + ".tmp");
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &*priced}};
  auto wrote = write_surface_archive_v2_file(tmp.string(), items);
  if (!wrote.has_value()) {
    ADD_FAILURE() << wrote.error().to_string();
    fs::remove(tmp, ec);
    return {};
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    fs::remove(tmp, ec); // lost the race: someone else published — fine
  }
  return file;
}

fs::path cached_corpus(const char *key, const std::function<std::vector<CorpusBoard>()> &boards) {
  const fs::path root{"artifact-cache"}; // under the ctest CWD (build tree)
  const fs::path dir = root / key;
  std::error_code ec;
  if (fs::exists(dir / "manifest.tsv")) {
    return dir;
  }
  fs::create_directories(root, ec);

  // Atomic publish, same pattern as cached_spy_convex_dense: build into a
  // per-process-random tmp dir, then rename into place. On Windows,
  // fs::rename to an already-existing destination directory errors (it does
  // not merge/replace) — that failure IS the expected "lost the publish
  // race" case, not a real error, so we just discard our tmp copy.
  std::mt19937_64 rng{std::random_device{}()};
  const fs::path tmp = root / (std::string{key} + "." + std::to_string(rng()) + ".tmp");
  auto man = build_corpus(boards(), tmp.string());
  if (!man.has_value()) {
    ADD_FAILURE() << "cached_corpus(" << key << "): " << man.error().to_string();
    return tmp; // let the caller fail on load with context (tmp dir left as-is)
  }

  // build_corpus bakes its OUT-DIR ARGUMENT (here, the tmp dir) into every
  // entry's archive_path, and already wrote that (now-stale) manifest.tsv to
  // disk inside tmp. Rewrite every entry to the archive's FINAL post-rename
  // location (dir/<date>.atxvsa -- one archive per date, shared by every
  // entry of that date) and re-persist the manifest before publishing, so a
  // caller that reads dir/manifest.tsv after the rename finds archive_path
  // values that actually resolve.
  for (CorpusEntry &e : man->entries) {
    if (!e.archive_path.empty()) {
      e.archive_path = (dir / (e.date + ".atxvsa")).generic_string();
    }
  }
  auto rewrote = write_manifest_file((tmp / "manifest.tsv").string(), *man);
  if (!rewrote.has_value()) {
    ADD_FAILURE() << "cached_corpus(" << key << "): " << rewrote.error().to_string();
    return tmp;
  }

  fs::rename(tmp, dir, ec);
  if (ec) {
    fs::remove_all(tmp, ec); // lost the publish race — the winner's dir is equivalent
  }
  return dir;
}

fs::path cached_hft_fit(const atx::vol::testkit::SpyFitFixture &fixture) {
  const fs::path dir{"artifact-cache"}; // under the ctest CWD (build tree)
  std::error_code ec;
  fs::create_directories(dir, ec);

  // Keyed on fixture.id (stable, unique per fixture) + kArchiveMajor, same
  // invalidate-on-format-bump rationale as cached_spy_convex_dense.
  const fs::path file = dir / ("hftfit_" + std::string(fixture.id) + "_v" +
                               std::to_string(kArchiveV2Major) + ".atxvsa");
  if (fs::exists(file)) {
    return file;
  }

  auto board = atx::vol::testkit::load_spy_fit_fixture(fixture);
  if (!board.has_value()) {
    return {}; // caller GTEST_SKIPs, same as today
  }

  // Verbatim fit recipe from SpyFitCorpus.HftColdStartPreserves98PctOnEveryAvailableSlice
  // / SigmaInterpCorpus.RealBoard_WithinGates: Hft preset PricerFitter over the
  // fixture's chain.
  auto chain = OptionChain::from_frame(board->panel.frame, board->env());
  if (!chain.has_value()) {
    ADD_FAILURE() << chain.error().to_string();
    return {};
  }

  PricerFitter fitter{PricerConfig{.preset = FitPreset::Hft}};
  const Status fit = fitter.fit(*chain);
  if (!fit.has_value()) {
    ADD_FAILURE() << fit.error().to_string();
    return {};
  }

  auto priced = fitter.surface()->session().to_priced_surface();
  if (!priced.has_value()) {
    ADD_FAILURE() << priced.error().to_string();
    return {};
  }

  // Atomic publish, identical pattern to cached_spy_convex_dense.
  std::mt19937_64 rng{std::random_device{}()};
  const fs::path tmp = dir / (file.filename().string() + "." + std::to_string(rng()) + ".tmp");
  const std::array<SurfaceArchiveItem, 1> items{SurfaceArchiveItem{"SPY", &*priced}};
  auto wrote = write_surface_archive_v2_file(tmp.string(), items);
  if (!wrote.has_value()) {
    ADD_FAILURE() << wrote.error().to_string();
    fs::remove(tmp, ec);
    return {};
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    fs::remove(tmp, ec); // lost the race: someone else published — fine
  }
  return file;
}

} // namespace atx::vol::test
