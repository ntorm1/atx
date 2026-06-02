#pragma once

// atx::core::container IntrusiveList — non-owning intrusive doubly-linked list.
//
// Design:
//   An intrusive list threads a chain of nodes through a ListHook member
//   embedded directly in each user-defined type T.  The list does NOT own the
//   nodes; the caller is responsible for node lifetime.  This means:
//     - Zero heap allocation for list management.
//     - O(1) push, pop, and unlink without searching.
//     - A node can be in multiple independent lists via distinct hook members.
//
// Hook↔node conversion approach:
//   The iterator and accessor functions need to convert a ListHook* back to a
//   T*.  We store a T* directly in each hook at link time (hook.owner_).  This
//   avoids any reinterpret_cast or pointer-arithmetic UB while keeping the
//   hook self-contained.
//
//   Alternatively the "container_of" pattern (reinterpret_cast<char*> + offset)
//   is well-defined by the C++ object model when offsetof is applicable, but
//   it requires std::is_standard_layout<T> and carries a larger SAFETY burden.
//   Storing owner_ is simpler, proven safe, and equally zero-cost in a release
//   build.
//
// Sentinel node:
//   A single sentinel_ hook acts as both the head-prev and tail-next, forming
//   a circular doubly-linked ring.  This eliminates all nullptr branches in the
//   hot push/pop/unlink paths — every node always has valid prev/next pointers.
//
// Copy / Move:
//   Copying an IntrusiveList would silently alias the raw node pointers, giving
//   two lists that share the same nodes with undefined removal order.  Moving is
//   similarly hazardous because it would leave dangling back-pointers in nodes
//   that still reference the moved-from sentinel address.  Both are explicitly
//   deleted via ATX_DISABLE_COPY_MOVE and documented here so callers get a clear
//   compiler error rather than silent aliasing.
//
// Thread-safety: NONE.  Synchronise externally when sharing between threads.

#include <cstddef>  // offsetof (informational; not used for conversion)
#include <iterator>

#include "atx/core/macro.hpp"  // ATX_ASSERT, ATX_DISABLE_COPY_MOVE
#include "atx/core/types.hpp"  // usize

namespace atx::core::container {

// =============================================================================
// ListHook — embed one of these as a member in every node type.
// =============================================================================

/// Intrusive doubly-linked list hook.
///
/// Embed as a named member in your node type.  A node may contain multiple
/// hooks to participate in multiple independent lists simultaneously.
///
/// @invariant  When the hook is unlinked, prev == nullptr && next == nullptr
///             && owner_ == nullptr.
/// @invariant  When linked, prev != nullptr, next != nullptr, owner_ != nullptr.
struct ListHook {
    ListHook* prev{nullptr};
    ListHook* next{nullptr};

    /// Non-owning back-pointer to the T that contains this hook.
    /// Set on link, cleared on unlink.  Stored as void* to keep the hook
    /// type-independent (the templated IntrusiveList supplies the T*).
    ///
    // SAFETY: owner_ is set to &node (a T&) by IntrusiveList before any access,
    //   and cleared (nullptr) on every unlink path.  It is read back as T* only
    //   inside IntrusiveList<T, Hook> which knows the correct type.
    void* owner_{nullptr};

    /// True when this hook is currently threaded into a list.
    [[nodiscard]] bool is_linked() const noexcept {
        return owner_ != nullptr;
    }
};

// =============================================================================
// IntrusiveList<T, Hook> — non-owning circular doubly-linked list.
// =============================================================================

/// Non-owning intrusive doubly-linked list.
///
/// @tparam T     The node type.  Must contain a ListHook member.
/// @tparam Hook  Pointer-to-member selecting which ListHook field to use.
///
/// The list is non-owning: it only threads existing T objects together.
/// Nodes must outlive the list (or be unlinked before destruction).
template <class T, ListHook T::* Hook>
class IntrusiveList {
public:
    // -------------------------------------------------------------------------
    // Construction / destruction
    // -------------------------------------------------------------------------

    /// Construct an empty list (sentinel wired to itself).
    IntrusiveList() noexcept {
        sentinel_.prev = &sentinel_;
        sentinel_.next = &sentinel_;
    }

    /// Destroy the list.  Nodes are not destroyed (non-owning), but every
    /// linked hook is reset so the nodes can be safely re-linked later.
    ~IntrusiveList() noexcept { clear(); }

    // Copy and move are unsafe for intrusive lists (see file-level comment).
    ATX_DISABLE_COPY_MOVE(IntrusiveList);

    // -------------------------------------------------------------------------
    // Modifiers — push
    // -------------------------------------------------------------------------

    /// Append node at the back of the list.
    ///
    /// @pre node is not currently linked (asserted in debug).
    void push_back(T& node) noexcept {
        ListHook& h = node.*Hook;
        ATX_ASSERT(!h.is_linked());
        insert_before(sentinel_, h, node);
        ++size_;
    }

    /// Prepend node at the front of the list.
    ///
    /// @pre node is not currently linked (asserted in debug).
    void push_front(T& node) noexcept {
        ListHook& h = node.*Hook;
        ATX_ASSERT(!h.is_linked());
        insert_before(*sentinel_.next, h, node);
        ++size_;
    }

    // -------------------------------------------------------------------------
    // Modifiers — pop
    // -------------------------------------------------------------------------

    /// Remove and return the front (oldest) node, or nullptr if empty.
    [[nodiscard]] T* pop_front() noexcept {
        if (empty()) {
            return nullptr;
        }
        ListHook& h = *sentinel_.next;
        // SAFETY: owner_ was set to a T* by push_front/push_back; the node's
        //   T object is still alive (non-owning contract: caller manages lifetime).
        T* node = static_cast<T*>(h.owner_);
        detach(h);
        --size_;
        return node;
    }

    /// Remove and return the back (newest) node, or nullptr if empty.
    [[nodiscard]] T* pop_back() noexcept {
        if (empty()) {
            return nullptr;
        }
        ListHook& h = *sentinel_.prev;
        // SAFETY: same as pop_front.
        T* node = static_cast<T*>(h.owner_);
        detach(h);
        --size_;
        return node;
    }

    // -------------------------------------------------------------------------
    // Modifiers — unlink specific node
    // -------------------------------------------------------------------------

    /// Remove node from the list in O(1).
    ///
    /// @pre node is currently linked in this list (asserted in debug).
    void unlink(T& node) noexcept {
        ListHook& h = node.*Hook;
        ATX_ASSERT(h.is_linked());
        detach(h);
        --size_;
    }

    // -------------------------------------------------------------------------
    // Element access
    // -------------------------------------------------------------------------

    /// Reference to the front element.
    ///
    /// @pre !empty() (asserted in debug).
    [[nodiscard]] T& front() noexcept {
        ATX_ASSERT(!empty());
        // SAFETY: same as pop_front.
        return *static_cast<T*>(sentinel_.next->owner_);
    }

    [[nodiscard]] const T& front() const noexcept {
        ATX_ASSERT(!empty());
        // SAFETY: same as pop_front.
        return *static_cast<const T*>(sentinel_.next->owner_);
    }

    /// Reference to the back element.
    ///
    /// @pre !empty() (asserted in debug).
    [[nodiscard]] T& back() noexcept {
        ATX_ASSERT(!empty());
        // SAFETY: same as pop_front.
        return *static_cast<T*>(sentinel_.prev->owner_);
    }

    [[nodiscard]] const T& back() const noexcept {
        ATX_ASSERT(!empty());
        // SAFETY: same as pop_front.
        return *static_cast<const T*>(sentinel_.prev->owner_);
    }

    // -------------------------------------------------------------------------
    // Observers
    // -------------------------------------------------------------------------

    /// Number of nodes currently in the list.
    [[nodiscard]] usize size() const noexcept { return size_; }

    /// True when the list contains no nodes.
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }

    // -------------------------------------------------------------------------
    // clear — unlink all nodes without destroying them
    // -------------------------------------------------------------------------

    /// Unlink and reset every hook in the list.  Nodes are NOT destroyed.
    /// After this call the list is empty and all nodes may be safely re-linked.
    void clear() noexcept {
        ListHook* cur = sentinel_.next;
        while (cur != &sentinel_) {
            ListHook* next = cur->next;
            cur->prev   = nullptr;
            cur->next   = nullptr;
            cur->owner_ = nullptr;
            cur = next;
        }
        sentinel_.prev = &sentinel_;
        sentinel_.next = &sentinel_;
        size_ = 0U;
    }

    // -------------------------------------------------------------------------
    // Iterator
    // -------------------------------------------------------------------------

    /// Forward iterator that walks the hook chain and yields T&.
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = T*;
        using reference         = T&;

        explicit Iterator(ListHook* hook) noexcept : hook_{hook} {}

        [[nodiscard]] T& operator*() const noexcept {
            // SAFETY: hook_ is a node hook (not the sentinel) while the iterator
            //   is in the [begin, end) range; owner_ was set at link time.
            return *static_cast<T*>(hook_->owner_);
        }

        [[nodiscard]] T* operator->() const noexcept {
            return static_cast<T*>(hook_->owner_);
        }

        Iterator& operator++() noexcept {
            hook_ = hook_->next;
            return *this;
        }

        Iterator operator++(int) noexcept {
            Iterator tmp{hook_};
            hook_ = hook_->next;
            return tmp;
        }

        [[nodiscard]] bool operator==(const Iterator& other) const noexcept {
            return hook_ == other.hook_;
        }

        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept {
            return hook_ != other.hook_;
        }

    private:
        ListHook* hook_;
    };

    /// Const forward iterator.
    class ConstIterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = const T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const T*;
        using reference         = const T&;

        explicit ConstIterator(const ListHook* hook) noexcept : hook_{hook} {}

        [[nodiscard]] const T& operator*() const noexcept {
            // SAFETY: same provenance guarantee as Iterator::operator*.
            return *static_cast<const T*>(hook_->owner_);
        }

        [[nodiscard]] const T* operator->() const noexcept {
            return static_cast<const T*>(hook_->owner_);
        }

        ConstIterator& operator++() noexcept {
            hook_ = hook_->next;
            return *this;
        }

        ConstIterator operator++(int) noexcept {
            ConstIterator tmp{hook_};
            hook_ = hook_->next;
            return tmp;
        }

        [[nodiscard]] bool operator==(const ConstIterator& other) const noexcept {
            return hook_ == other.hook_;
        }

        [[nodiscard]] bool operator!=(const ConstIterator& other) const noexcept {
            return hook_ != other.hook_;
        }

    private:
        const ListHook* hook_;
    };

    [[nodiscard]] Iterator begin() noexcept {
        return Iterator{sentinel_.next};
    }

    [[nodiscard]] Iterator end() noexcept {
        return Iterator{&sentinel_};
    }

    [[nodiscard]] ConstIterator begin() const noexcept {
        return ConstIterator{sentinel_.next};
    }

    [[nodiscard]] ConstIterator end() const noexcept {
        return ConstIterator{&sentinel_};
    }

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Wire hook into the chain immediately before the given anchor hook.
    /// Sets hook.owner_ = &node so the iterator can convert back to T*.
    ///
    // SAFETY: anchor is always a valid hook in this list (either the sentinel or
    //   a user node); hook is freshly unlinked (asserted by callers).
    void insert_before(ListHook& anchor, ListHook& hook, T& node) noexcept {
        hook.prev   = anchor.prev;
        hook.next   = &anchor;
        hook.owner_ = &node;

        // SAFETY: hook.prev was a valid linked hook before this insertion; we
        //   update its next pointer to rethread the chain through hook.
        anchor.prev->next = &hook;
        anchor.prev       = &hook;
    }

    /// Remove hook from the chain and reset its fields to the unlinked state.
    ///
    // SAFETY: hook.prev and hook.next are both valid linked hooks (invariant of
    //   the circular list); bypassing hook is well-defined pointer surgery.
    void detach(ListHook& hook) noexcept {
        hook.prev->next = hook.next;
        hook.next->prev = hook.prev;
        hook.prev       = nullptr;
        hook.next       = nullptr;
        hook.owner_     = nullptr;
    }

    // -------------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------------

    /// Sentinel node — its next/prev form the circular ring boundary.
    /// sentinel_.owner_ is never set (sentinel is not a T node).
    ListHook sentinel_{};

    /// Number of nodes currently threaded through the list.
    usize size_{0U};
};

} // namespace atx::core::container
