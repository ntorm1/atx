#pragma once

// atx::core::container HashMap / HashSet — thin wrappers over
// ankerl::unordered_dense::map and ::set.
//
// Value-adds over the raw library:
//   • HashMap::at(key) — [[nodiscard]] Result<V*> / Result<const V*>.
//     Returns Err(ErrorCode::NotFound) when the key is absent instead of
//     throwing std::out_of_range.  noexcept: all error paths go through
//     Result; no exception escapes.
//   • Otherwise the public API exactly mirrors std::unordered_map /
//     std::unordered_set plus ankerl extras (reserve is O(n), not amortised).
//
// Design:
//   • Composition (one private member) — Rule of Zero; the underlying
//     map/set handles all special-member semantics.
//   • The underlying member is not exposed in the public API: callers
//     work entirely through the wrapper's forwarding methods and iterators,
//     which are the underlying container's iterators re-exported via
//     using-declarations.
//   • operator[] documents that it default-inserts on a missing key
//     (identical contract to std::map::operator[]).
//
// Thread-safety: NONE.  Synchronise externally if shared between threads.

#include <ankerl/unordered_dense.h>

#include <cstddef>
#include <functional>
#include <utility>

#include "atx/core/error.hpp" // Result, Err, ErrorCode

namespace atx::core::container {

// ---------------------------------------------------------------------------
// HashMap<K, V, Hash, Eq>
// ---------------------------------------------------------------------------

/// A hash map that wraps ankerl::unordered_dense::map.
///
/// @tparam K     Key type.
/// @tparam V     Value type.
/// @tparam Hash  Hash callable; defaults to ankerl::unordered_dense::hash<K>.
/// @tparam Eq    Equality predicate; defaults to std::equal_to<K>.
template <class K,
          class V,
          class Hash = ankerl::unordered_dense::hash<K>,
          class Eq   = std::equal_to<K>>
class HashMap {
    using Impl = ankerl::unordered_dense::map<K, V, Hash, Eq>;
    Impl map_;

public:
    // ---- Re-exported types ------------------------------------------------
    using key_type        = typename Impl::key_type;
    using mapped_type     = typename Impl::mapped_type;
    using value_type      = typename Impl::value_type;
    using size_type       = typename Impl::size_type;
    using iterator        = typename Impl::iterator;
    using const_iterator  = typename Impl::const_iterator;

    // Rule of Zero — default all special members; Impl handles them.
    HashMap()                            = default;
    HashMap(const HashMap&)              = default;
    HashMap(HashMap&&) noexcept          = default;
    HashMap& operator=(const HashMap&)   = default;
    HashMap& operator=(HashMap&&) noexcept = default;
    ~HashMap()                           = default;

    // ---- Capacity ---------------------------------------------------------

    /// Returns the number of key-value pairs.
    [[nodiscard]] size_type size() const noexcept { return map_.size(); }

    /// Returns true iff the map contains no elements.
    [[nodiscard]] bool empty() const noexcept { return map_.empty(); }

    /// Pre-allocates capacity for at least n elements without rehashing.
    void reserve(size_type n) { map_.reserve(n); }

    // ---- Modifiers --------------------------------------------------------

    /// Inserts or overwrites the mapping for key k with value v.
    template <class KK, class VV>
    void insert_or_assign(KK&& k, VV&& v) {
        map_.insert_or_assign(std::forward<KK>(k), std::forward<VV>(v));
    }

    /// Constructs a key-value pair in-place from args.
    template <class... Args>
    void emplace(Args&&... args) {
        map_.emplace(std::forward<Args>(args)...);
    }

    /// Removes the element with the given key (no-op if absent).
    void erase(const K& k) { map_.erase(k); }

    /// Removes all elements.
    void clear() noexcept { map_.clear(); }

    // ---- Lookup -----------------------------------------------------------

    /// Returns true iff the map contains a mapping for k.
    [[nodiscard]] bool contains(const K& k) const { return map_.contains(k); }

    /// Returns an iterator to the element with key k, or end() if absent.
    [[nodiscard]] iterator find(const K& k) { return map_.find(k); }

    /// Returns a const_iterator to the element with key k, or end() if absent.
    [[nodiscard]] const_iterator find(const K& k) const { return map_.find(k); }

    /// Default-inserts a value for k if absent; returns a reference to the
    /// mapped value.  Documented default-insert behaviour mirrors std::map.
    [[nodiscard]] V& operator[](const K& k) { return map_[k]; }

    /// @overload (rvalue key)
    [[nodiscard]] V& operator[](K&& k) { return map_[std::move(k)]; }

    /// Returns a pointer to the mapped value for k, or Err(NotFound).
    ///
    /// noexcept: no exception leaves this function; all error information is
    /// carried in the Result return value.
    ///
    /// The returned pointer is valid until the next mutating operation on
    /// this map.
    [[nodiscard]] atx::core::Result<V*> at(const K& k) noexcept {
        auto it = map_.find(k);
        if (it == map_.end()) {
            return atx::core::Err(atx::core::ErrorCode::NotFound);
        }
        return atx::core::Ok(&it->second);
    }

    /// Const overload — returns a pointer-to-const.
    [[nodiscard]] atx::core::Result<const V*> at(const K& k) const noexcept {
        auto it = map_.find(k);
        if (it == map_.end()) {
            return atx::core::Err(atx::core::ErrorCode::NotFound);
        }
        return atx::core::Ok(&it->second);
    }

    // ---- Iterators --------------------------------------------------------

    [[nodiscard]] iterator       begin()        noexcept { return map_.begin(); }
    [[nodiscard]] const_iterator begin()  const noexcept { return map_.begin(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return map_.cbegin(); }
    [[nodiscard]] iterator       end()          noexcept { return map_.end(); }
    [[nodiscard]] const_iterator end()    const noexcept { return map_.end(); }
    [[nodiscard]] const_iterator cend()   const noexcept { return map_.cend(); }
};

// ---------------------------------------------------------------------------
// HashSet<K, Hash, Eq>
// ---------------------------------------------------------------------------

/// A hash set that wraps ankerl::unordered_dense::set.
///
/// @tparam K     Key type.
/// @tparam Hash  Hash callable; defaults to ankerl::unordered_dense::hash<K>.
/// @tparam Eq    Equality predicate; defaults to std::equal_to<K>.
template <class K,
          class Hash = ankerl::unordered_dense::hash<K>,
          class Eq   = std::equal_to<K>>
class HashSet {
    using Impl = ankerl::unordered_dense::set<K, Hash, Eq>;
    Impl set_;

public:
    // ---- Re-exported types ------------------------------------------------
    using key_type       = typename Impl::key_type;
    using value_type     = typename Impl::value_type;
    using size_type      = typename Impl::size_type;
    using iterator       = typename Impl::iterator;
    using const_iterator = typename Impl::const_iterator;

    // Rule of Zero
    HashSet()                            = default;
    HashSet(const HashSet&)              = default;
    HashSet(HashSet&&) noexcept          = default;
    HashSet& operator=(const HashSet&)   = default;
    HashSet& operator=(HashSet&&) noexcept = default;
    ~HashSet()                           = default;

    // ---- Capacity ---------------------------------------------------------

    /// Returns the number of elements.
    [[nodiscard]] size_type size() const noexcept { return set_.size(); }

    /// Returns true iff the set contains no elements.
    [[nodiscard]] bool empty() const noexcept { return set_.empty(); }

    /// Pre-allocates capacity for at least n elements.
    void reserve(size_type n) { set_.reserve(n); }

    // ---- Modifiers --------------------------------------------------------

    /// Inserts k into the set (no-op if already present).
    template <class KK>
    void insert(KK&& k) { set_.insert(std::forward<KK>(k)); }

    /// Removes k from the set (no-op if absent).
    void erase(const K& k) { set_.erase(k); }

    /// Removes all elements.
    void clear() noexcept { set_.clear(); }

    // ---- Lookup -----------------------------------------------------------

    /// Returns true iff k is in the set.
    [[nodiscard]] bool contains(const K& k) const { return set_.contains(k); }

    // ---- Iterators --------------------------------------------------------

    [[nodiscard]] iterator       begin()        noexcept { return set_.begin(); }
    [[nodiscard]] const_iterator begin()  const noexcept { return set_.begin(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return set_.cbegin(); }
    [[nodiscard]] iterator       end()          noexcept { return set_.end(); }
    [[nodiscard]] const_iterator end()    const noexcept { return set_.end(); }
    [[nodiscard]] const_iterator cend()   const noexcept { return set_.cend(); }
};

} // namespace atx::core::container
