#include "atx/engine/alpha/panel.hpp"

namespace atx::engine::alpha {

atx::core::Result<Panel>
Panel::create(atx::usize dates, atx::usize instruments, std::vector<std::string> field_names,
              std::vector<std::vector<atx::f64>> field_data, std::vector<std::uint8_t> universe) {
  if (field_names.size() != field_data.size()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "Panel::create: field_names and field_data size mismatch");
  }
  const atx::usize cells = dates * instruments;
  for (const std::vector<atx::f64> &col : field_data) {
    if (col.size() != cells) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Panel::create: a field column is not dates*instruments cells");
    }
  }
  if (!universe.empty() && universe.size() != cells) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "Panel::create: universe is neither empty nor dates*instruments cells");
  }

  Panel p;
  p.dates_ = dates;
  p.instruments_ = instruments;
  p.field_names_ = std::move(field_names);
  p.backing_ = std::move(field_data);
  p.columns_.reserve(p.backing_.size());
  for (const std::vector<atx::f64> &col : p.backing_) {
    p.columns_.emplace_back(col.data(), col.size());
  }
  // Empty universe == all-valid: materialize an all-ones mask so in_universe()
  // is a single O(1) read with no special-casing on the hot path.
  if (universe.empty()) {
    p.universe_.assign(cells, std::uint8_t{1});
  } else {
    p.universe_ = std::move(universe);
  }
  return atx::core::Ok(std::move(p));
}

atx::core::Result<Panel>
Panel::create_borrowed(atx::usize dates, atx::usize instruments, std::vector<std::string> field_names,
                       std::vector<std::span<const atx::f64>> columns,
                       std::vector<std::uint8_t> universe) {
  if (field_names.size() != columns.size()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "Panel::create_borrowed: field_names and columns size mismatch");
  }
  const atx::usize cells = dates * instruments;
  for (const std::span<const atx::f64> &col : columns) {
    if (col.size() != cells) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "Panel::create_borrowed: a column is not dates*instruments cells");
    }
  }
  if (!universe.empty() && universe.size() != cells) {
    return atx::core::Err(
        atx::core::ErrorCode::InvalidArgument,
        "Panel::create_borrowed: universe is neither empty nor dates*instruments cells");
  }
  Panel p;
  p.dates_ = dates;
  p.instruments_ = instruments;
  p.field_names_ = std::move(field_names);
  p.columns_ = std::move(columns); // borrowed; backing_ stays empty
  if (universe.empty()) {
    p.universe_.assign(cells, std::uint8_t{1});
  } else {
    p.universe_ = std::move(universe);
  }
  return atx::core::Ok(std::move(p));
}

void Panel::copy_from(const Panel &o) {
  dates_ = o.dates_;
  instruments_ = o.instruments_;
  field_names_ = o.field_names_;
  universe_ = o.universe_;
  backing_ = o.backing_;
  if (backing_.empty()) {
    columns_ = o.columns_; // borrowed (or zero-field): external spans copy fine
  } else {
    columns_.clear();
    columns_.reserve(backing_.size());
    for (const std::vector<atx::f64> &col : backing_) {
      columns_.emplace_back(col.data(), col.size());
    }
  }
}

} // namespace atx::engine::alpha
