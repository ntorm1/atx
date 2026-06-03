#pragma once

// atx::core::series Frame — multi-column heterogeneous time-series store.
//
// Design:
//   A Frame is a named collection of typed columns plus a shared Timestamp
//   index, modelling a tabular time-series ("DataFrame"). Columns may hold
//   different element types (e.g. a `double` price column beside an `int`
//   count column), so the store is HETEROGENEOUS and uses TYPE ERASURE:
//
//     IColumn         — abstract base: virtual dtor + size() + RTTI tag.
//     ColumnHolder<T> — concrete holder owning one Column<T>.
//
//   Lookups are name-keyed via a HashMap<string, slot>; the slot indexes a
//   parallel vector<unique_ptr<IColumn>>. Type-correct retrieval is enforced
//   with dynamic_cast on the holder (RTTI), so column<WrongType>() returns an
//   error rather than reinterpreting the buffer (no UB).
//
//   rows() convention:
//     rows() == the maximum length over the index column and every data
//     column. Columns are appended to independently, so they may transiently
//     differ in length; rows() reports the longest. An empty frame has 0 rows.
//
//   Shared index:
//     A Column<Timestamp> index() is always present (initially empty). It is
//     OPTIONAL to populate — append_index() grows it — and its length counts
//     toward rows().
//
//   Ownership (Rule of Zero, move-only):
//     Members are a HashMap, a vector of unique_ptr, a vector of names, and a
//     Column<Timestamp>. All are movable; none is copyable through the
//     unique_ptr<IColumn>. Frame is therefore MOVE-ONLY: copy is deleted, move
//     defaulted. References returned by add_column()/column() are stable for
//     the lifetime of the owning holder (columns live behind unique_ptr, so a
//     vector reallocation does not move the Column itself).
//
//   Thread-safety: NONE. Synchronise externally when shared between threads.

#include <functional> // std::reference_wrapper
#include <memory>     // std::unique_ptr, std::make_unique
#include <string>
#include <string_view>
#include <typeinfo>   // std::type_info, typeid
#include <utility>
#include <vector>

#include "atx/core/container/hash_map.hpp" // HashMap
#include "atx/core/datetime.hpp"           // atx::core::time::Timestamp
#include "atx/core/error.hpp"              // Result, Ok, Err, ErrorCode
#include "atx/core/macro.hpp"              // ATX_ASSERT
#include "atx/core/series/column.hpp"      // Column<T>
#include "atx/core/types.hpp"              // usize

namespace atx::core::series {

// ---------------------------------------------------------------------------
// Type-erased column interface
// ---------------------------------------------------------------------------

/// Abstract base for a type-erased column. Exposes only the type-agnostic
/// queries the Frame needs (row count, element type tag) plus a virtual
/// destructor so holders are destroyed through this base.
class IColumn {
public:
    IColumn() noexcept = default;
    IColumn(const IColumn&) = delete;
    IColumn& operator=(const IColumn&) = delete;
    IColumn(IColumn&&) = delete;
    IColumn& operator=(IColumn&&) = delete;
    virtual ~IColumn() = default;

    /// Number of rows currently stored in this column.
    [[nodiscard]] virtual usize size() const noexcept = 0;

    /// RTTI tag for the stored element type (used for type-checked retrieval).
    [[nodiscard]] virtual const std::type_info& type() const noexcept = 0;
};

/// Concrete holder owning a Column<T>. The Frame stores these behind
/// unique_ptr<IColumn>; dynamic_cast<ColumnHolder<T>*> recovers the typed
/// column iff T matches the stored element type.
template <class T>
class ColumnHolder final : public IColumn {
public:
    ColumnHolder() = default;

    [[nodiscard]] usize size() const noexcept override { return column_.size(); }
    [[nodiscard]] const std::type_info& type() const noexcept override { return typeid(T); }

    [[nodiscard]] Column<T>& column() noexcept { return column_; }
    [[nodiscard]] const Column<T>& column() const noexcept { return column_; }

private:
    Column<T> column_{};
};

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

/// Named heterogeneous columns plus a shared Timestamp index. Move-only.
class Frame {
public:
    /// Lightweight, no-copy descriptor of a half-open row range [begin, end).
    /// Returned by slice(); does not own or reference frame storage.
    struct Slice {
        usize begin{};
        usize end{};
    };

    // Rule of Zero (move-only): unique_ptr<IColumn> is non-copyable, so the
    // Frame copy operations are implicitly deleted; we delete them explicitly
    // for clarity and default the moves.
    Frame() = default;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&&) noexcept = default;
    Frame& operator=(Frame&&) noexcept = default;
    ~Frame() = default;

    // ------------------------------------------------------------------
    // Column management
    // ------------------------------------------------------------------

    /// Creates a new, empty Column<T> named `name` and returns a stable
    /// reference to it. Err(AlreadyExists) if a column of that name already
    /// exists (regardless of its element type); Err(InvalidArgument) if `name`
    /// is empty.
    template <class T>
    [[nodiscard]] Result<std::reference_wrapper<Column<T>>> add_column(std::string_view name) {
        if (name.empty()) {
            return Err(ErrorCode::InvalidArgument, "Frame::add_column empty name");
        }
        std::string key{name};
        if (index_.contains(key)) {
            return Err(ErrorCode::AlreadyExists, "Frame::add_column duplicate name");
        }
        auto holder = std::make_unique<ColumnHolder<T>>();
        Column<T>& ref = holder->column();
        const usize slot = columns_.size();
        columns_.push_back(std::move(holder));
        names_.push_back(key);
        index_.insert_or_assign(std::move(key), slot);
        return Ok(std::reference_wrapper<Column<T>>{ref});
    }

    /// Looks up the column named `name` as a Column<T>. Err(NotFound) if no
    /// such name; Err(InvalidArgument) if the stored element type != T.
    template <class T>
    [[nodiscard]] Result<std::reference_wrapper<Column<T>>> column(std::string_view name) {
        ColumnHolder<T>* const holder = find_holder<T>(name);
        if (holder == nullptr) {
            return lookup_error<T>(name);
        }
        return Ok(std::reference_wrapper<Column<T>>{holder->column()});
    }

    /// @overload Const lookup returning a reference to a const Column<T>.
    template <class T>
    [[nodiscard]] Result<std::reference_wrapper<const Column<T>>>
    column(std::string_view name) const {
        const ColumnHolder<T>* const holder = find_holder<T>(name);
        if (holder == nullptr) {
            return lookup_error<const T>(name);
        }
        return Ok(std::reference_wrapper<const Column<T>>{holder->column()});
    }

    // ------------------------------------------------------------------
    // Shared Timestamp index
    // ------------------------------------------------------------------

    /// Appends one timestamp to the shared index column.
    void append_index(time::Timestamp ts) { ts_index_.append(ts); }

    /// The shared Timestamp index column (initially empty).
    [[nodiscard]] Column<time::Timestamp>& index() noexcept { return ts_index_; }
    [[nodiscard]] const Column<time::Timestamp>& index() const noexcept { return ts_index_; }

    // ------------------------------------------------------------------
    // Observers
    // ------------------------------------------------------------------

    /// Number of rows = the maximum length over the index and all data
    /// columns. 0 for an empty frame. See header note on the convention.
    [[nodiscard]] usize rows() const noexcept {
        usize n = ts_index_.size();
        for (const auto& holder : columns_) {
            ATX_ASSERT(holder != nullptr);
            const usize s = holder->size();
            if (s > n) {
                n = s;
            }
        }
        return n;
    }

    /// Number of named data columns (excludes the Timestamp index).
    [[nodiscard]] usize num_columns() const noexcept { return columns_.size(); }

    /// True iff a column named `name` exists.
    [[nodiscard]] bool has_column(std::string_view name) const {
        return index_.contains(std::string{name});
    }

    /// Half-open row-range descriptor [begin, end). No bounds are enforced;
    /// the caller intersects it with rows() as needed.
    [[nodiscard]] Slice slice(usize begin, usize end) const noexcept {
        return Slice{begin, end};
    }

private:
    // Resolves `name` to a slot, or std::nullopt-equivalent (returns false).
    // Returns nullptr when the name is missing OR the stored type != T.
    template <class T>
    [[nodiscard]] ColumnHolder<T>* find_holder(std::string_view name) {
        const usize* const slot = find_slot(name);
        if (slot == nullptr) {
            return nullptr;
        }
        ATX_ASSERT(*slot < columns_.size());
        IColumn* const base = columns_[*slot].get();
        // SAFETY: dynamic_cast is the type-checked recovery of the concrete
        // holder; it yields nullptr (not UB) when the stored element type != T.
        return dynamic_cast<ColumnHolder<T>*>(base);
    }

    template <class T>
    [[nodiscard]] const ColumnHolder<T>* find_holder(std::string_view name) const {
        const usize* const slot = find_slot(name);
        if (slot == nullptr) {
            return nullptr;
        }
        ATX_ASSERT(*slot < columns_.size());
        const IColumn* const base = columns_[*slot].get();
        // SAFETY: as above — dynamic_cast is null on type mismatch, never UB.
        return dynamic_cast<const ColumnHolder<T>*>(base);
    }

    // Looks up the slot for `name`; nullptr when absent.
    [[nodiscard]] const usize* find_slot(std::string_view name) const {
        const auto it = index_.find(std::string{name});
        if (it == index_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    // Distinguishes "missing name" (NotFound) from "wrong type"
    // (InvalidArgument) for a failed lookup. T is only used to keep the
    // template-dependent return type; the value is unused.
    template <class T>
    [[nodiscard]] tl::unexpected<Error> lookup_error(std::string_view name) const {
        if (find_slot(name) == nullptr) {
            return Err(ErrorCode::NotFound, "Frame::column no such column");
        }
        return Err(ErrorCode::InvalidArgument, "Frame::column element-type mismatch");
    }

    container::HashMap<std::string, usize> index_{};       // name -> slot
    std::vector<std::unique_ptr<IColumn>>  columns_{};     // parallel: holders
    std::vector<std::string>               names_{};       // parallel: names
    Column<time::Timestamp>                ts_index_{};     // shared index
};

} // namespace atx::core::series
