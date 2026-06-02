// intrusive_list_test.cpp — TDD tests for atx::core::container::IntrusiveList
//
// Coverage:
//   - push_front / push_back ordering
//   - pop_front / pop_back return correct node, nullptr on empty
//   - unlink middle, head, and tail nodes
//   - front() / back() references
//   - size(), empty(), clear()
//   - Re-linking a node after it has been unlinked
//   - Forward iteration order (begin/end)
//   - Const iteration
//   - size accounting after mixed operations
//   - is_linked() hook helper
//
// Naming: all local helper types are suffixed "IL" to avoid ODR collisions in
// the single test binary that includes every test translation unit.

#include <atx/core/container/intrusive_list.hpp>

#include <vector>

#include <gtest/gtest.h>

using namespace atx::core::container; // NOLINT(google-build-using-namespace)
using atx::usize;

// ---------------------------------------------------------------------------
// Helper node type
// ---------------------------------------------------------------------------
struct NodeIL {
    int      v;
    ListHook hook;
};

// ---------------------------------------------------------------------------
// TEST: Seed tests from spec
// ---------------------------------------------------------------------------
TEST(IntrusiveList, PushPopOrder) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{1, {}}, b{2, {}};
    l.push_back(a);
    l.push_back(b);
    EXPECT_EQ(&l.front(), &a);
    EXPECT_EQ(&l.back(), &b);
    (void)l.pop_front();
    EXPECT_EQ(&l.front(), &b);
}

TEST(IntrusiveList, UnlinkNode) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{1, {}}, b{2, {}}, c{3, {}};
    l.push_back(a);
    l.push_back(b);
    l.push_back(c);
    l.unlink(b);
    EXPECT_EQ(l.size(), 2U);
}

// ---------------------------------------------------------------------------
// TEST: push_front inserts at head; order is LIFO from front perspective
// ---------------------------------------------------------------------------
TEST(IntrusiveList, PushFrontOrder) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{1, {}}, b{2, {}}, c{3, {}};
    l.push_front(a);
    l.push_front(b);
    l.push_front(c);
    // Expected order front→back: c, b, a
    EXPECT_EQ(&l.front(), &c);
    EXPECT_EQ(&l.back(), &a);
    EXPECT_EQ(l.size(), 3U);
}

// ---------------------------------------------------------------------------
// TEST: pop_back returns tail node, nullptr on empty
// ---------------------------------------------------------------------------
TEST(IntrusiveList, PopBack) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{1, {}}, b{2, {}};
    l.push_back(a);
    l.push_back(b);

    NodeIL* got = l.pop_back();
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got, &b);
    EXPECT_EQ(l.size(), 1U);

    got = l.pop_back();
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got, &a);
    EXPECT_TRUE(l.empty());

    // Empty list returns nullptr.
    EXPECT_EQ(l.pop_back(), nullptr);
}

// ---------------------------------------------------------------------------
// TEST: pop_front returns nullptr on empty
// ---------------------------------------------------------------------------
TEST(IntrusiveList, PopFrontEmpty) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    EXPECT_EQ(l.pop_front(), nullptr);
}

// ---------------------------------------------------------------------------
// TEST: unlink head — list remains consistent
// ---------------------------------------------------------------------------
TEST(IntrusiveList, UnlinkHead) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{1, {}}, b{2, {}}, c{3, {}};
    l.push_back(a);
    l.push_back(b);
    l.push_back(c);

    l.unlink(a);
    EXPECT_EQ(l.size(), 2U);
    EXPECT_EQ(&l.front(), &b);
}

// ---------------------------------------------------------------------------
// TEST: unlink tail — list remains consistent
// ---------------------------------------------------------------------------
TEST(IntrusiveList, UnlinkTail) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{1, {}}, b{2, {}}, c{3, {}};
    l.push_back(a);
    l.push_back(b);
    l.push_back(c);

    l.unlink(c);
    EXPECT_EQ(l.size(), 2U);
    EXPECT_EQ(&l.back(), &b);
}

// ---------------------------------------------------------------------------
// TEST: unlink middle — neighbors are wired together
// ---------------------------------------------------------------------------
TEST(IntrusiveList, UnlinkMiddle) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{1, {}}, b{2, {}}, c{3, {}};
    l.push_back(a);
    l.push_back(b);
    l.push_back(c);

    l.unlink(b);
    EXPECT_EQ(l.size(), 2U);
    EXPECT_EQ(&l.front(), &a);
    EXPECT_EQ(&l.back(), &c);
}

// ---------------------------------------------------------------------------
// TEST: forward iteration produces elements in push_back order
// ---------------------------------------------------------------------------
TEST(IntrusiveList, IterationOrder) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{10, {}}, b{20, {}}, c{30, {}};
    l.push_back(a);
    l.push_back(b);
    l.push_back(c);

    std::vector<int> values;
    for (NodeIL& n : l) {
        values.push_back(n.v);
    }
    ASSERT_EQ(values.size(), 3U);
    EXPECT_EQ(values[0], 10);
    EXPECT_EQ(values[1], 20);
    EXPECT_EQ(values[2], 30);
}

// ---------------------------------------------------------------------------
// TEST: const iteration
// ---------------------------------------------------------------------------
TEST(IntrusiveList, ConstIteration) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{1, {}}, b{2, {}};
    l.push_back(a);
    l.push_back(b);

    const auto& cl = l;
    std::vector<int> values;
    for (const NodeIL& n : cl) {
        values.push_back(n.v);
    }
    ASSERT_EQ(values.size(), 2U);
    EXPECT_EQ(values[0], 1);
    EXPECT_EQ(values[1], 2);
}

// ---------------------------------------------------------------------------
// TEST: size accounting — push, pop, unlink
// ---------------------------------------------------------------------------
TEST(IntrusiveList, SizeAccounting) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    EXPECT_EQ(l.size(), 0U);
    EXPECT_TRUE(l.empty());

    NodeIL a{1, {}}, b{2, {}}, c{3, {}};
    l.push_back(a);
    EXPECT_EQ(l.size(), 1U);
    EXPECT_FALSE(l.empty());

    l.push_back(b);
    l.push_back(c);
    EXPECT_EQ(l.size(), 3U);

    (void)l.pop_front();
    EXPECT_EQ(l.size(), 2U);

    l.unlink(c);
    EXPECT_EQ(l.size(), 1U);
}

// ---------------------------------------------------------------------------
// TEST: clear() unlinks all nodes without destroying them; nodes reusable
// ---------------------------------------------------------------------------
TEST(IntrusiveList, Clear) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{1, {}}, b{2, {}}, c{3, {}};
    l.push_back(a);
    l.push_back(b);
    l.push_back(c);

    l.clear();
    EXPECT_TRUE(l.empty());
    EXPECT_EQ(l.size(), 0U);

    // Nodes are not destroyed — they live on the stack.
    EXPECT_EQ(a.v, 1);
    EXPECT_EQ(b.v, 2);
    EXPECT_EQ(c.v, 3);
}

// ---------------------------------------------------------------------------
// TEST: re-link a node after it has been unlinked
// ---------------------------------------------------------------------------
TEST(IntrusiveList, RelinkAfterUnlink) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{1, {}}, b{2, {}};
    l.push_back(a);
    l.push_back(b);

    l.unlink(a);
    EXPECT_EQ(l.size(), 1U);

    // a is no longer linked; push it again at the front.
    l.push_front(a);
    EXPECT_EQ(l.size(), 2U);
    EXPECT_EQ(&l.front(), &a);
}

// ---------------------------------------------------------------------------
// TEST: is_linked() hook helper
// ---------------------------------------------------------------------------
TEST(IntrusiveList, IsLinked) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{1, {}};

    EXPECT_FALSE(a.hook.is_linked());
    l.push_back(a);
    EXPECT_TRUE(a.hook.is_linked());

    l.unlink(a);
    EXPECT_FALSE(a.hook.is_linked());
}

// ---------------------------------------------------------------------------
// TEST: Single-element list — front() and back() refer to the same node
// ---------------------------------------------------------------------------
TEST(IntrusiveList, SingleElement) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{42, {}};
    l.push_back(a);

    EXPECT_EQ(&l.front(), &a);
    EXPECT_EQ(&l.back(), &a);
    EXPECT_EQ(l.size(), 1U);

    NodeIL* got = l.pop_front();
    EXPECT_EQ(got, &a);
    EXPECT_TRUE(l.empty());
}

// ---------------------------------------------------------------------------
// TEST: clear() followed by new insertions — list is fully reusable
// ---------------------------------------------------------------------------
TEST(IntrusiveList, ClearAndReuse) {
    IntrusiveList<NodeIL, &NodeIL::hook> l;
    NodeIL a{1, {}}, b{2, {}};
    l.push_back(a);
    l.push_back(b);
    l.clear();

    // Re-use the same nodes in a different order.
    l.push_front(b);
    l.push_front(a);
    EXPECT_EQ(l.size(), 2U);
    EXPECT_EQ(&l.front(), &a);
    EXPECT_EQ(&l.back(), &b);
}

// ---------------------------------------------------------------------------
// TEST: Different hook member pointer — list works with any hook field
// ---------------------------------------------------------------------------
struct DualHookIL {
    int      v{0};
    ListHook hookA;
    ListHook hookB;
};

TEST(IntrusiveList, DualHookMemberPointer) {
    IntrusiveList<DualHookIL, &DualHookIL::hookA> la;
    IntrusiveList<DualHookIL, &DualHookIL::hookB> lb;

    DualHookIL x{1, {}, {}}, y{2, {}, {}};

    la.push_back(x);
    la.push_back(y);
    lb.push_back(y); // y is in both lists via different hooks
    lb.push_back(x);

    EXPECT_EQ(la.size(), 2U);
    EXPECT_EQ(lb.size(), 2U);
    EXPECT_EQ(&la.front(), &x);
    EXPECT_EQ(&lb.front(), &y);
}
