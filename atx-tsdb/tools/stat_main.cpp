// atx-tsdb-stat — print header + geometry of a sealed segment (and verify crc).
//
// Usage: atx-tsdb-stat SEGMENT.atxseg

#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "atx/tsdb/segment_reader.hpp"

// The body is fully wrapped in try/catch(...); the only residual throw site is
// the diagnostic stream I/O inside the handlers, which at worst std::terminates
// a CLI on its way out — acceptable for an entry point.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char **argv) {
  try {
    // SAFETY: argv holds argc valid pointers (plus a trailing nullptr). Copy them
    // into owned strings so the path is read from a bounds-checked vector.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const std::vector<std::string> args(argv, argv + argc);
    if (args.size() != 2) {
      std::cerr << "usage: atx-tsdb-stat SEGMENT.atxseg\n";
      return 2;
    }
    auto reader = atx::tsdb::SegmentReader::attach(args[1]);
    if (!reader) {
      std::cerr << "attach failed: " << reader.error().to_string() << "\n";
      return 1;
    }
    std::cout << "segment: " << args[1] << "\n"
              << "  fields:      " << reader->field_count() << "\n"
              << "  instruments: " << reader->instrument_count() << "\n"
              << "  time rows:   " << reader->time_count() << "\n"
              << "  content_hash:" << reader->content_hash() << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "fatal: unknown exception\n";
    return 1;
  }
}
