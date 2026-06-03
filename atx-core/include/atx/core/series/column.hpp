#pragma once

// atx::core::series Column<T> — cache-aligned growable columnar buffer.
//
// Design:
//   Column<T> is a vector-like, dynamically-growing buffer whose backing
//   storage is always allocated with kCacheLineSize (64-byte) alignment via
//   aligned_alloc_bytes / aligned_free.  This makes data() suitable for
//   aligned SIMD loads and prevents false sharing between adjacent columns.
//   It models a single typed column of POD numeric data in a columnar store.
//
//   Element type:
//     T is constrained to trivially-copyable types (static_assert).  Columns
//     hold POD numeric data; restricting to trivially-copyable lets growth and
//     copy use std::memcpy and keeps the layout SIMD-friendly.  Non-trivial T
//     (with placement-new lifecycle) is deliberately out of scope.
//
//   Growth:
//     append() is amortized O(1): when size_ == capacity_ the buffer doubles
//     (from a base capacity of kInitialCapacity).  Growth allocates a fresh
//     aligned buffer, memcpy's the live elements, and frees the old buffer.
//     reserve()/append_bulk() grow at most once to the exact needed capacity.
//
//   Ownership (Rule of Five):
//     Column owns an aligned heap buffer, so it defines all five special
//     members.  It is a value type: copy is a deep copy (independent buffer),
//     move steals the buffer (source left empty and valid), destructor frees.
//
//   Validity bitmap:
//     A parallel std::vector<bool> tracks per-row validity.  append() pushes a
//     valid (true) slot; append_null() pushes an invalid (false) slot whose
//     value is default-constructed.  is_valid(i) queries it.  This mirrors the
//     null-handling of columnar formats (e.g. Arrow) while staying simple.
//
//   Thread-safety: NONE.  Synchronise externally when shared between threads.

#include <cstring> // std::memcpy
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/core/aligned.hpp"   // aligned_alloc_bytes, aligned_free
#include "atx/core/error.hpp"     // Result, Ok, Err, ErrorCode
#include "atx/core/macro.hpp"     // ATX_ASSERT
#include "atx/core/platform.hpp"  // kCacheLineSize
#include "atx/core/types.hpp"     // usize

namespace atx::core::series {

/// Cache-aligned, growable columnar buffer of trivially-copyable T.
///
/// @tparam T  Element type. Must be trivially copyable (POD numeric data).
template <class T>
class Column {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Column<T>: T must be trivially copyable (columns hold POD data)");

public:
    using value_type      = T;
    using size_type       = usize;
    using reference       = T&;
    using const_reference = const T&;
    using pointer         = T*;
    using const_pointer   = const T*;

    // ------------------------------------------------------------------
    // Construction / destruction (Rule of Five)
    // ------------------------------------------------------------------

    /// Constructs an empty column. No allocation; data() is nullptr.
    Column() noexcept = default;

    /// Deep-copies another column into a fresh, independent aligned buffer.
    Column(const Column& other) : valid_{other.valid_} {
        if (other.size_ != 0U) {
            data_     = allocate(other.size_);
            capacity_ = other.size_;
            // SAFETY: T is trivially copyable; memcpy of size_ elements from a
            // valid source buffer into a freshly-allocated buffer of equal
            // capacity is well-defined and equivalent to element-wise copy.
            std::memcpy(data_, other.data_, other.size_ * sizeof(T));
            size_ = other.size_;
        }
    }

    /// Steals other's buffer; leaves other empty and valid.
    Column(Column&& other) noexcept
        : data_{other.data_},
          size_{other.size_},
          capacity_{other.capacity_},
          valid_{std::move(other.valid_)} {
        other.data_     = nullptr;
        other.size_     = 0U;
        other.capacity_ = 0U;
        other.valid_.clear();
    }

    /// Copy-assigns via copy-and-swap (strong guarantee, self-assign safe).
    Column& operator=(const Column& other) {
        if (this != &other) {
            Column tmp{other};
            swap(tmp);
        }
        return *this;
    }

    /// Move-assigns: frees own buffer, steals other's.
    Column& operator=(Column&& other) noexcept {
        if (this != &other) {
            aligned_free(data_);
            data_           = other.data_;
            size_           = other.size_;
            capacity_       = other.capacity_;
            valid_          = std::move(other.valid_);
            other.data_     = nullptr;
            other.size_     = 0U;
            other.capacity_ = 0U;
            other.valid_.clear();
        }
        return *this;
    }

    /// Frees the owned aligned buffer.
    ~Column() { aligned_free(data_); }

    // ------------------------------------------------------------------
    // Modifiers
    // ------------------------------------------------------------------

    /// Appends a single value. Amortized O(1); grows (doubling) when full.
    void append(const T& value) {
        if (size_ == capacity_) {
            grow(capacity_ == 0U ? kInitialCapacity : capacity_ * 2U);
        }
        data_[size_] = value;
        valid_.push_back(true);
        ++size_;
    }

    /// Appends an invalid (null) slot whose value is default-constructed.
    void append_null() {
        if (size_ == capacity_) {
            grow(capacity_ == 0U ? kInitialCapacity : capacity_ * 2U);
        }
        data_[size_] = T{};
        valid_.push_back(false);
        ++size_;
    }

    /// Appends every element of values, growing at most once if needed.
    void append_bulk(std::span<const T> values) {
        if (values.empty()) {
            return;
        }
        reserve(size_ + values.size());
        // SAFETY: T is trivially copyable; the destination window
        // [size_, size_ + values.size()) is in-bounds after reserve, and the
        // source span is contiguous, so the bulk memcpy equals element copy.
        std::memcpy(data_ + size_, values.data(), values.size() * sizeof(T));
        valid_.insert(valid_.end(), values.size(), true);
        size_ += values.size();
    }

    /// Ensures capacity() >= n. No-op when n <= capacity(). Never shrinks.
    void reserve(usize n) {
        if (n > capacity_) {
            grow(n);
        }
    }

    /// Resets size() to 0; retains capacity() and the buffer.
    void clear() noexcept {
        size_ = 0U;
        valid_.clear();
    }

    // ------------------------------------------------------------------
    // Observers
    // ------------------------------------------------------------------

    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] usize capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }

    /// Pointer to the contiguous buffer. nullptr only while capacity() == 0.
    /// SAFETY: when non-null the pointer is kCacheLineSize-aligned (guaranteed
    /// by aligned_alloc_bytes) and valid for [0, capacity()).
    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }

    /// Read-only span over [0, size()).
    [[nodiscard]] std::span<const T> view() const noexcept {
        return std::span<const T>{data_, size_};
    }

    /// Mutable span over [0, size()).
    [[nodiscard]] std::span<T> mutable_view() noexcept {
        return std::span<T>{data_, size_};
    }

    /// Unchecked element access. Precondition: i < size().
    [[nodiscard]] T& operator[](usize i) noexcept {
        ATX_ASSERT(i < size_);
        return data_[i];
    }

    [[nodiscard]] const T& operator[](usize i) const noexcept {
        ATX_ASSERT(i < size_);
        return data_[i];
    }

    /// Bounds-checked access. Returns the element by value, or OutOfRange.
    [[nodiscard]] Result<T> at(usize i) const {
        if (i >= size_) {
            return Err(ErrorCode::OutOfRange, "Column::at index out of range");
        }
        return Ok(data_[i]);
    }

    /// Per-row validity. Precondition: i < size(). Default-valid rows from
    /// append() are true; rows from append_null() are false.
    [[nodiscard]] bool is_valid(usize i) const noexcept {
        ATX_ASSERT(i < size_);
        return valid_[i];
    }

private:
    static constexpr usize kInitialCapacity = 8U;

    /// Allocates an aligned buffer for `n` elements. Aborts on OOM (the
    /// allocation budget for a growable column is unbounded by contract).
    [[nodiscard]] static T* allocate(usize n) {
        void* const raw = aligned_alloc_bytes(n * sizeof(T), kCacheLineSize);
        ATX_CHECK(raw != nullptr);
        return static_cast<T*>(raw);
    }

    /// Reallocates to exactly `new_cap` elements (must be > current size_),
    /// copying live elements into the new aligned buffer and freeing the old.
    void grow(usize new_cap) {
        ATX_ASSERT(new_cap >= size_);
        T* const new_data = allocate(new_cap);
        if (size_ != 0U) {
            // SAFETY: T is trivially copyable; copying size_ <= new_cap live
            // elements from the old buffer into the new one is well-defined.
            std::memcpy(new_data, data_, size_ * sizeof(T));
        }
        aligned_free(data_);
        data_     = new_data;
        capacity_ = new_cap;
    }

    /// Swaps two columns (used by copy-and-swap assignment). noexcept.
    void swap(Column& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
        valid_.swap(other.valid_);
    }

    T*    data_     = nullptr;
    usize size_     = 0U;
    usize capacity_ = 0U;
    std::vector<bool> valid_{}; // parallel per-row validity bitmap
};

} // namespace atx::core::series
