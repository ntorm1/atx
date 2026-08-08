// track_compact -- operator CLI over `atx::vol::compact()` (Task D2,
// backtest-production-lakehouse sprint). Folds every staged track under
// <lake_root>/staging/ into hive-partitioned, zstd-compressed Parquet batch
// files under <lake_root>/tracks/underlier=<U>/family=<F>/batch-NNNNNN.parquet.
// See atx/vol/research/track_store.hpp for the schema and the atomic-publish
// discipline this CLI relies on end to end.
//
// Only built when ATX_VOL_LAKEHOUSE is ON (atx-vol/CMakeLists.txt) -- the
// library entry point it wraps does not exist in the OFF build.

#include "atx/vol/research/track_store.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

void print_usage(const char *argv0) {
  std::fprintf(stderr,
               "usage: %s <lake_root>\n"
               "  Compacts every staged track under <lake_root>/staging/ into\n"
               "  hive-partitioned, zstd-compressed Parquet batch files under\n"
               "  <lake_root>/tracks/underlier=<U>/family=<F>/batch-NNNNNN.parquet.\n",
               argv0);
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    print_usage(argv[0]);
    return 2;
  }
  const std::string_view arg1{argv[1]};
  if (arg1 == "--help" || arg1 == "-h") {
    print_usage(argv[0]);
    return 0;
  }

  const std::string lake_root{arg1};
  const atx::vol::Result<atx::vol::CompactStats> result = atx::vol::compact(lake_root);
  if (!result.has_value()) {
    std::fprintf(stderr, "track_compact: %s\n", result.error().to_string().c_str());
    return 1;
  }

  const atx::vol::CompactStats &stats = *result;
  std::printf("track_compact: %llu track(s) compacted into %llu batch file(s); "
              "%llu staged input(s) deleted\n",
              static_cast<unsigned long long>(stats.tracks_compacted),
              static_cast<unsigned long long>(stats.batch_files_written),
              static_cast<unsigned long long>(stats.staged_files_deleted));
  return 0;
}
