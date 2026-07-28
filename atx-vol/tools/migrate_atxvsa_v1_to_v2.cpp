// migrate_atxvsa_v1_to_v2 — THROWAWAY one-shot migrator (WS-S / S3).
//
// Reads a v1 `.atxvsa` (ATXVSA v3) archive and re-packs it into a v2 `.atxvsa2`
// (zero-copy columnar) archive with IDENTICAL numeric content. This is the ONLY
// place v1 byte-layout knowledge is meant to survive after the clean-break cut
// (sprint §0): wave 2 (S4) runs it once over the committed fixtures, then v1 read
// is deleted. It reconstructs each surface via the v1 reader and re-serializes
// through the v2 writer, so the bytes change but every fitted double does not.
//
// Usage:
//   migrate_atxvsa_v1_to_v2 <in.atxvsa> <out.atxvsa2>
//
// It preserves per-surface provenance (map_all_with_provenance -> v2 items) and
// therefore the schema_hash/validation semantics the readers depend on. A
// created_ts of 0 lets the v2 writer stamp the wall clock; pass nothing else —
// this tool is deliberately option-free and disposable.
//
// One kind does NOT migrate: a v1 SplineVol record has no `mult_cap`/`w_offset`
// on the wire, and both are live terms of `SplineVolCurve::w()`, so the v1 reader
// refuses it (plan item 2.15) rather than let this tool forward invented zeros
// into a v2 record that can carry them. Such an archive aborts here with that
// error; the surface has to be re-fitted and written as v2 directly.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_archive.hpp"

namespace {

int fail(std::string_view what, std::string_view detail) {
  std::fprintf(stderr, "migrate_atxvsa_v1_to_v2: %.*s: %.*s\n", static_cast<int>(what.size()),
               what.data(), static_cast<int>(detail.size()), detail.data());
  return 1;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <in.atxvsa> <out.atxvsa2>\n", argv[0]);
    return 2;
  }
  const std::string in_path = argv[1];
  const std::string out_path = argv[2];

  // 1. Open the v1 archive and reconstruct every surface + its provenance.
  auto v1 = atx::vol::SurfaceArchive::open_file(in_path);
  if (!v1) {
    return fail("open v1", v1.error().to_string());
  }
  auto archived = v1->map_all_with_provenance();
  if (!archived) {
    return fail("reconstruct v1", archived.error().to_string());
  }

  // 2. Recover each surface's canonical symbol from the v1 directory (directory
  //    order matches map_all_with_provenance order).
  const std::span<const atx::vol::ArchiveDirEntry> dir = v1->directory();
  if (dir.size() != archived->size()) {
    return fail("directory", "surface count / directory size mismatch");
  }

  // 3. Build the v2 item list (non-owning; surfaces/symbols outlive the write).
  std::vector<std::string> symbols;
  symbols.reserve(archived->size());
  for (const atx::vol::ArchiveDirEntry &de : dir) {
    symbols.emplace_back(de.symbol, de.symbol + de.symbol_len);
  }
  std::vector<atx::vol::SurfaceArchiveItem> items;
  items.reserve(archived->size());
  for (std::size_t i = 0; i < archived->size(); ++i) {
    const atx::vol::ArchivedSurface &a = (*archived)[i];
    atx::vol::SurfaceArchiveItem item;
    item.symbol = symbols[i];
    item.surface = &a.surface;
    // A v1 legacy (zero-provenance) surface must not be written as explicit
    // provenance (the v2 writer rejects legacy_format); leave it nullopt so the
    // v2 record stores the same legacy zero-fill and reads back identically.
    if (!a.provenance.legacy_format) {
      item.provenance = a.provenance;
    }
    items.push_back(item);
  }

  // 4. Serialize to v2 and persist atomically.
  const atx::vol::Status st = atx::vol::write_surface_archive_v2_file(out_path, items);
  if (!st) {
    return fail("write v2", st.error().to_string());
  }
  std::fprintf(stdout, "migrated %zu surface(s): %s -> %s\n", archived->size(), in_path.c_str(),
               out_path.c_str());
  return 0;
}
