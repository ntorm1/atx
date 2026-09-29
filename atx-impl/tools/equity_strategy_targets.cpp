#include <iostream>
#include <string_view>
#include "strategy_live.hpp"
#include "strategy_nav_replay.hpp"
#include "strategy_reconcile.hpp"
#include "strategy_target_replay.hpp"

// `nav` verb: self-financing $1bn NAV replay of the pinned saved blend (fixed cost
// scenarios; with --fields/--fields-sha256 the swap-fin-v1 financing matrix).
// `decide` verb: the deployed book's targets and orders at one as-of session from the
// actual positions (atx.book-deploy/v1 manifest; TRAIN-only in this build).
// `reconcile` verb: the book a decide expected vs the broker positions, after corporate
// actions (breaks exit 5).
// Without a verb: the existing planned-target replay, unchanged.
int main(int argc, char** argv) {
  if (argc > 1 && std::string_view{argv[1]} == "nav")
    return atx::impl::strategy::dispatch_nav_replay(argc - 1, argv + 1, std::cout, std::cerr);
  if (argc > 1 && std::string_view{argv[1]} == "decide")
    return atx::impl::strategy::dispatch_decide(argc - 1, argv + 1, std::cout, std::cerr);
  if (argc > 1 && std::string_view{argv[1]} == "reconcile")
    return atx::impl::strategy::dispatch_reconcile(argc - 1, argv + 1, std::cout, std::cerr);
  return atx::impl::strategy::dispatch_target_replay(argc, argv, std::cout, std::cerr);
}
