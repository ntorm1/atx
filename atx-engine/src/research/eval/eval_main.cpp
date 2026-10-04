// atx-research-eval: the statistics of record of daily net series (eval_cli.hpp, K-P9-5).
#include <iostream>
#include <string_view>
#include <vector>

#include "atx/engine/research/eval/eval_cli.hpp"

int main(int argc, char **argv) {
  std::vector<std::string_view> args;
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  return atx::engine::research::eval::research_eval_main(args, std::cout, std::cerr);
}
