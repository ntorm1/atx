#include <iostream>
#include <string_view>
#include "strategy_nav_replay.hpp"
#include "strategy_target_replay.hpp"

// `nav` verb: self-financing $1bn NAV replay of the pinned saved blend (fixed cost
// scenarios; with --fields/--fields-sha256 the swap-fin-v1 financing matrix).
// Without the verb: the existing planned-target replay, unchanged.
int main(int argc, char** argv) {
  if (argc > 1 && std::string_view{argv[1]} == "nav")
    return atx::impl::strategy::dispatch_nav_replay(argc - 1, argv + 1, std::cout, std::cerr);
  return atx::impl::strategy::dispatch_target_replay(argc, argv, std::cout, std::cerr);
}
