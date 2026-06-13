#include "atx/engine/data/data_handler.hpp"

namespace atx::engine::data {

// NOLINTNEXTLINE(bugprone-exception-escape)
InMemoryBarFeed::InMemoryBarFeed(std::span<const std::span<const BarRow>> sources, SimClock &clock,
                                 EventBus<> &bus) noexcept
    : sources_{sources}, clock_{&clock}, bus_{&bus} {
  // One allocation each, sized to N: the cursors and the heap's backing store.
  cursors_.assign(sources_.size(), 0);
  for (atx::usize s = 0; s < sources_.size(); ++s) {
    if (!sources_[s].empty()) {
      heap_.push(
          HeapEntry{sources_[s].front().knowledge_ts.unix_nanos(), static_cast<atx::u32>(s)});
    }
  }
}

} // namespace atx::engine::data
