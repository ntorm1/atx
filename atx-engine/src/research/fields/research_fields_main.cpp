// atx-research-fields: the engine's research field builder (research_fields_cli.hpp).
#include <iostream>
#include <string_view>
#include <vector>

#include "atx/engine/research/fields/research_fields_cli.hpp"

int main(int argc, char **argv) {
  std::vector<std::string_view> args;
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  return atx::engine::research::fields::research_fields_main(args, std::cout, std::cerr);
}
