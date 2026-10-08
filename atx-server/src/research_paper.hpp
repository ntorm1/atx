#pragma once

#include "research_portfolio.hpp"

namespace atx::server::research_detail {

[[nodiscard]] PaperReplayResult collect_paper_ledger(const DatasetPanel& data,
    const PortfolioRecipe& recipe, const PortfolioReplayOutput& output, std::stop_token stop);

} // namespace atx::server::research_detail
