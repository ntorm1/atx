#pragma once
#include <atx/server/core.hpp>
#include <string>
#include <string_view>

namespace atx::server::library_detail {
struct Input {
    std::string name, description, tags, hypothesis, request, hash, source_job_id, project_id, actor_id;
};
[[nodiscard]] Input validate(const AlphaInput& input);
[[nodiscard]] std::string canonical_recipe(std::string_view request);
void validate_project(std::string_view project);
void validate_page(std::uint32_t limit, std::uint32_t offset);
} // namespace atx::server::library_detail
