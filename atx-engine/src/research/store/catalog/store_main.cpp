// atx-research-store: the research store's command line (store_cli.hpp). stdout / stderr are
// binary on Windows so every printed byte is the byte the CLI wrote (`schema --json` prints the
// exact text a store keeps in store_info('schema_json'): LF only).

#include <iostream>
#include <string>
#include <vector>

#include "atx/engine/research/store/catalog/store_cli.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
  (void)_setmode(_fileno(stdout), _O_BINARY);
  (void)_setmode(_fileno(stderr), _O_BINARY);
#endif
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  const int code = atx::engine::research::store::catalog::run_store_cli(args, std::cout, std::cerr);
  std::cout.flush();
  return code;
}
