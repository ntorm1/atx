// ring_buffer_test.cpp — TDD tests for atx::core::container::RingBuffer
//
// Coverage:
//   - FIFO push/pop ordering
//   - push returns false when full
//   - push_overwrite drops oldest when full
//   - Oldest-relative operator[] window access
//   - Wraparound across many push/pop cycles
//   - clear() empties the buffer and allows reuse
//   - pop() on empty returns nullopt
//   - Capacity rounds up to next power of two
//   - Non-trivial element type: ctor/dtor counting proves no leaks or
//     double-destructions (including overwrite, clear, move, copy paths)
//   - Copy constructor and copy assignment
//   - Move constructor and move assignment

#include <atx/core/container/ring_buffer.hpp>

#include <string>
#include <utility>

#include <gtest/gtest.h>

using namespace atx::core::container; // NOLINT(google-build-using-namespace)
using atx::usize;

// ---------------------------------------------------------------------------
// Instrumented type for ctor/dtor counting
// ---------------------------------------------------------------------------
struct Tracked {
    static int ctor_count;
    static int dtor_count;
    static int copy_count;
    static int move_count;

    int value{0};

    explicit Tracked(int v) noexcept : value{v} { ++ctor_count; }

    ~Tracked() noexcept { ++dtor_count; }

    Tracked(const Tracked& o) noexcept : value{o.value} {
        ++ctor_count;
        ++copy_count;
    }
    Tracked& operator=(const Tracked& o) noexcept {
        if (this != &o) {
            value = o.value;
            ++copy_count;
        }
        return *this;
    }

    Tracked(Tracked&& o) noexcept : value{o.value} {
        o.value = -1;
        ++ctor_count;
        ++move_count;
    }
    Tracked& operator=(Tracked&& o) noexcept {
        if (this != &o) {
            value = o.value;
            o.value = -1;
            ++move_count;
        }
        return *this;
    }

    static void reset() noexcept {
        ctor_count = 0;
        dtor_count = 0;
        copy_count = 0;
        move_count = 0;
    }
};

int Tracked::ctor_count = 0;
int Tracked::dtor_count = 0;
int Tracked::copy_count = 0;
int Tracked::move_count = 0;

// ---------------------------------------------------------------------------
// TEST: FIFO push/pop ordering
// ---------------------------------------------------------------------------
TEST(RingBuffer, PushPopFifo) {
    RingBuffer<int, 4> r;
    EXPECT_TRUE(r.push(1));
    EXPECT_TRUE(r.push(2));
    EXPECT_EQ(r.pop().value(), 1);
    EXPECT_EQ(r.pop().value(), 2);
    EXPECT_FALSE(r.pop().has_value());
}

// ---------------------------------------------------------------------------
// TEST: push returns false when buffer is full
// ---------------------------------------------------------------------------
TEST(RingBuffer, PushFailsWhenFull) {
    RingBuffer<int, 2> r;
    EXPECT_TRUE(r.push(1));
    EXPECT_TRUE(r.push(2));
    EXPECT_FALSE(r.push(3));
    EXPECT_EQ(r.size(), 2U);
    // Existing elements must be intact.
    EXPECT_EQ(r.pop().value(), 1);
    EXPECT_EQ(r.pop().value(), 2);
}

// ---------------------------------------------------------------------------
// TEST: push_overwrite drops the oldest element when full
// ---------------------------------------------------------------------------
TEST(RingBuffer, OverwriteDropsOldest) {
    RingBuffer<int, 2> r;
    r.push_overwrite(1);
    r.push_overwrite(2);
    r.push_overwrite(3); // drops 1
    EXPECT_EQ(r.pop().value(), 2);
    EXPECT_EQ(r.pop().value(), 3);
    EXPECT_FALSE(r.pop().has_value());
}

// ---------------------------------------------------------------------------
// TEST: Oldest-relative operator[] window access
// ---------------------------------------------------------------------------
TEST(RingBuffer, IndexedWindowAccess) {
    RingBuffer<int, 4> r;
    EXPECT_TRUE(r.push(10));
    EXPECT_TRUE(r.push(20));
    EXPECT_TRUE(r.push(30));
    EXPECT_EQ(r[0], 10);
    EXPECT_EQ(r[1], 20);
    EXPECT_EQ(r[2], 30);
    EXPECT_EQ(r.size(), 3U);
}

// ---------------------------------------------------------------------------
// TEST: Window index reflects FIFO order after interleaved push/pop
// ---------------------------------------------------------------------------
TEST(RingBuffer, IndexedWindowAfterMixedOps) {
    RingBuffer<int, 4> r;
    EXPECT_TRUE(r.push(1));
    EXPECT_TRUE(r.push(2));
    EXPECT_TRUE(r.push(3));
    (void)r.pop(); // removes 1; oldest is now 2
    EXPECT_TRUE(r.push(4));
    // Buffer: [2, 3, 4]
    EXPECT_EQ(r[0], 2);
    EXPECT_EQ(r[1], 3);
    EXPECT_EQ(r[2], 4);
}

// ---------------------------------------------------------------------------
// TEST: Wraparound across many push/pop cycles
// ---------------------------------------------------------------------------
TEST(RingBuffer, Wraparound) {
    // Use capacity 4 (power of two), drive head/tail around the ring many times.
    RingBuffer<int, 4> r;
    constexpr int kRounds = 100;
    for (int base = 0; base < kRounds; ++base) {
        ASSERT_TRUE(r.push(base * 4 + 0));
        ASSERT_TRUE(r.push(base * 4 + 1));
        ASSERT_TRUE(r.push(base * 4 + 2));
        ASSERT_TRUE(r.push(base * 4 + 3));
        EXPECT_TRUE(r.full());

        for (int k = 0; k < 4; ++k) {
            EXPECT_EQ(r.pop().value(), base * 4 + k);
        }
        EXPECT_TRUE(r.empty());
    }
}

// ---------------------------------------------------------------------------
// TEST: clear() destroys live elements and resets to empty
// ---------------------------------------------------------------------------
TEST(RingBuffer, Clear) {
    RingBuffer<int, 4> r;
    EXPECT_TRUE(r.push(1));
    EXPECT_TRUE(r.push(2));
    EXPECT_TRUE(r.push(3));
    r.clear();

    EXPECT_EQ(r.size(),  0U);
    EXPECT_TRUE(r.empty());
    EXPECT_FALSE(r.full());

    // Must be fully usable after clear.
    EXPECT_TRUE(r.push(42));
    EXPECT_EQ(r.pop().value(), 42);
}

// ---------------------------------------------------------------------------
// TEST: pop() on an empty buffer returns nullopt
// ---------------------------------------------------------------------------
TEST(RingBuffer, PopEmpty) {
    RingBuffer<int, 4> r;
    EXPECT_FALSE(r.pop().has_value());
}

// ---------------------------------------------------------------------------
// TEST: Capacity is rounded up to next power of two
// ---------------------------------------------------------------------------
TEST(RingBuffer, CapacityRoundsToPow2) {
    // Wrap template instantiations in typedefs to avoid confusing the
    // EXPECT_EQ macro with the comma inside the angle brackets.
    using RB3 = RingBuffer<int, 3>;
    using RB4 = RingBuffer<int, 4>;
    using RB5 = RingBuffer<int, 5>;
    using RB8 = RingBuffer<int, 8>;
    using RB1 = RingBuffer<int, 1>;

    EXPECT_EQ(RB3::capacity(), 4U); // Requested 3 → actual 4
    EXPECT_EQ(RB5::capacity(), 8U); // Requested 5 → actual 8
    EXPECT_EQ(RB4::capacity(), 4U); // Exact power-of-two unchanged
    EXPECT_EQ(RB8::capacity(), 8U); // Exact power-of-two unchanged
    EXPECT_EQ(RB1::capacity(), 1U); // Minimum capacity
}

// ---------------------------------------------------------------------------
// TEST: size(), empty(), full() accounting
// ---------------------------------------------------------------------------
TEST(RingBuffer, SizeAccounting) {
    RingBuffer<int, 2> r;
    EXPECT_EQ(r.size(), 0U);
    EXPECT_TRUE(r.empty());
    EXPECT_FALSE(r.full());

    EXPECT_TRUE(r.push(1));
    EXPECT_EQ(r.size(), 1U);
    EXPECT_FALSE(r.empty());
    EXPECT_FALSE(r.full());

    EXPECT_TRUE(r.push(2));
    EXPECT_EQ(r.size(), 2U);
    EXPECT_FALSE(r.empty());
    EXPECT_TRUE(r.full());

    (void)r.pop();
    EXPECT_EQ(r.size(), 1U);
    EXPECT_FALSE(r.full());
}

// ---------------------------------------------------------------------------
// TEST: Non-trivial type — no leaks or double-destructions on normal push/pop
// ---------------------------------------------------------------------------
TEST(RingBuffer, NonTrivialNormalPath) {
    Tracked::reset();
    {
        RingBuffer<Tracked, 4> r;
        EXPECT_TRUE(r.push(Tracked{1}));
        EXPECT_TRUE(r.push(Tracked{2}));
        EXPECT_TRUE(r.push(Tracked{3}));

        // pop() must move-out and then destroy the slot's Tracked.
        auto v = r.pop();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(v->value, 1);
    } // destructor of r destroys remaining 2 live elements; v destroyed here

    // ctor_count covers: 3 temporaries + 3 push move-constructs + 1 pop move-construct
    // dtor_count must equal ctor_count (no leak, no double-free).
    EXPECT_EQ(Tracked::ctor_count, Tracked::dtor_count);
}

// ---------------------------------------------------------------------------
// TEST: Non-trivial type — no leaks when push_overwrite evicts
// ---------------------------------------------------------------------------
TEST(RingBuffer, NonTrivialOverwrite) {
    Tracked::reset();
    {
        RingBuffer<Tracked, 2> r;
        EXPECT_TRUE(r.push(Tracked{1}));
        EXPECT_TRUE(r.push(Tracked{2}));
        // This must destroy Tracked{1} before constructing Tracked{3}.
        r.push_overwrite(Tracked{3});
    }
    EXPECT_EQ(Tracked::ctor_count, Tracked::dtor_count);
}

// ---------------------------------------------------------------------------
// TEST: Non-trivial type — clear() destroys all live elements
// ---------------------------------------------------------------------------
TEST(RingBuffer, NonTrivialClear) {
    Tracked::reset();
    {
        RingBuffer<Tracked, 4> r;
        EXPECT_TRUE(r.push(Tracked{10}));
        EXPECT_TRUE(r.push(Tracked{20}));
        EXPECT_TRUE(r.push(Tracked{30}));
        r.clear(); // must destroy all 3 in-place elements
        EXPECT_EQ(r.size(), 0U);
        // At this point the temporaries have also been destroyed; total must balance.
        // We don't assert here because the optional holding moved-from values also
        // runs dtors; we verify the final count at scope exit.
    }
    EXPECT_EQ(Tracked::ctor_count, Tracked::dtor_count);
}

// ---------------------------------------------------------------------------
// TEST: Copy constructor produces independent copy; original unaffected
// ---------------------------------------------------------------------------
TEST(RingBuffer, CopyConstructor) {
    RingBuffer<int, 4> r;
    EXPECT_TRUE(r.push(10));
    EXPECT_TRUE(r.push(20));
    EXPECT_TRUE(r.push(30));

    RingBuffer<int, 4> copy{r}; // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(copy.size(), 3U);
    EXPECT_EQ(copy[0], 10);
    EXPECT_EQ(copy[1], 20);
    EXPECT_EQ(copy[2], 30);

    // Mutate copy; original must be unaffected.
    (void)copy.pop();
    EXPECT_EQ(r.size(), 3U);
}

// ---------------------------------------------------------------------------
// TEST: Copy assignment replaces contents; old elements destroyed
// ---------------------------------------------------------------------------
TEST(RingBuffer, CopyAssignment) {
    RingBuffer<int, 4> src;
    EXPECT_TRUE(src.push(7));
    EXPECT_TRUE(src.push(8));

    RingBuffer<int, 4> dst;
    EXPECT_TRUE(dst.push(99));
    dst = src;

    EXPECT_EQ(dst.size(), 2U);
    EXPECT_EQ(dst.pop().value(), 7);
    EXPECT_EQ(dst.pop().value(), 8);
    // Source intact.
    EXPECT_EQ(src.size(), 2U);
}

// ---------------------------------------------------------------------------
// TEST: Move constructor transfers elements; source becomes empty
// ---------------------------------------------------------------------------
TEST(RingBuffer, MoveConstructor) {
    RingBuffer<int, 4> r;
    EXPECT_TRUE(r.push(1));
    EXPECT_TRUE(r.push(2));
    EXPECT_TRUE(r.push(3));

    RingBuffer<int, 4> moved{std::move(r)};
    EXPECT_EQ(moved.size(), 3U);
    EXPECT_EQ(moved[0], 1);
    EXPECT_EQ(moved[1], 2);
    EXPECT_EQ(moved[2], 3);
    EXPECT_TRUE(r.empty()); // NOLINT(bugprone-use-after-move) — intentional
}

// ---------------------------------------------------------------------------
// TEST: Move assignment transfers elements; source becomes empty
// ---------------------------------------------------------------------------
TEST(RingBuffer, MoveAssignment) {
    RingBuffer<int, 4> src;
    EXPECT_TRUE(src.push(5));
    EXPECT_TRUE(src.push(6));

    RingBuffer<int, 4> dst;
    EXPECT_TRUE(dst.push(99));
    dst = std::move(src);

    EXPECT_EQ(dst.size(), 2U);
    EXPECT_EQ(dst.pop().value(), 5);
    EXPECT_EQ(dst.pop().value(), 6);
    EXPECT_TRUE(src.empty()); // NOLINT(bugprone-use-after-move) — intentional
}

// ---------------------------------------------------------------------------
// TEST: Non-trivial type — copy ctor/assign produce no extra leaks
// ---------------------------------------------------------------------------
TEST(RingBuffer, NonTrivialCopyNoLeak) {
    Tracked::reset();
    {
        RingBuffer<Tracked, 4> src;
        EXPECT_TRUE(src.push(Tracked{1}));
        EXPECT_TRUE(src.push(Tracked{2}));

        RingBuffer<Tracked, 4> copy{src}; // copy-constructs 2 Tracked
        (void)copy;
    }
    EXPECT_EQ(Tracked::ctor_count, Tracked::dtor_count);
}

// ---------------------------------------------------------------------------
// TEST: Non-trivial type — move ctor produces no extra leaks
// ---------------------------------------------------------------------------
TEST(RingBuffer, NonTrivialMoveNoLeak) {
    Tracked::reset();
    {
        RingBuffer<Tracked, 4> src;
        EXPECT_TRUE(src.push(Tracked{1}));
        EXPECT_TRUE(src.push(Tracked{2}));

        RingBuffer<Tracked, 4> moved{std::move(src)};
        (void)moved;
    }
    EXPECT_EQ(Tracked::ctor_count, Tracked::dtor_count);
}

// ---------------------------------------------------------------------------
// TEST: Wrapping window access — head not at 0
// ---------------------------------------------------------------------------
TEST(RingBuffer, IndexedWindowWrapped) {
    // Advance head into the middle of the ring then check indexing.
    RingBuffer<int, 4> r;
    EXPECT_TRUE(r.push(1));
    EXPECT_TRUE(r.push(2));
    EXPECT_TRUE(r.push(3));
    EXPECT_TRUE(r.push(4));
    (void)r.pop(); // removes 1; head_ is now 1
    (void)r.pop(); // removes 2; head_ is now 2
    EXPECT_TRUE(r.push(5));
    EXPECT_TRUE(r.push(6));
    // Physical: tail wraps. Logical order: [3, 4, 5, 6]
    EXPECT_EQ(r.size(), 4U);
    EXPECT_EQ(r[0], 3);
    EXPECT_EQ(r[1], 4);
    EXPECT_EQ(r[2], 5);
    EXPECT_EQ(r[3], 6);
}

// ---------------------------------------------------------------------------
// TEST: Works with non-trivially-destructible std::string
// ---------------------------------------------------------------------------
TEST(RingBuffer, StringElements) {
    RingBuffer<std::string, 4> r;
    EXPECT_TRUE(r.push(std::string{"alpha"}));
    EXPECT_TRUE(r.push(std::string{"beta"}));
    EXPECT_TRUE(r.push(std::string{"gamma"}));

    EXPECT_EQ(r[0], "alpha");
    auto v = r.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "alpha");
    EXPECT_EQ(r[0], "beta");
}

// ---------------------------------------------------------------------------
// TEST: Single-element capacity (Capacity == 1)
// ---------------------------------------------------------------------------
TEST(RingBuffer, SingleElementCapacity) {
    RingBuffer<int, 1> r;
    EXPECT_EQ(r.capacity(), 1U);
    EXPECT_TRUE(r.push(42));
    EXPECT_TRUE(r.full());
    EXPECT_FALSE(r.push(99));
    EXPECT_EQ(r.pop().value(), 42);
    EXPECT_TRUE(r.empty());
}

// ---------------------------------------------------------------------------
// TEST: push_overwrite with many elements verifies FIFO window
// ---------------------------------------------------------------------------
TEST(RingBuffer, OverwriteManyFifoWindow) {
    RingBuffer<int, 4> r;
    // Push 10 values through a capacity-4 ring.
    for (int i = 0; i < 10; ++i) {
        r.push_overwrite(i);
    }
    // Last 4 values pushed: 6, 7, 8, 9
    EXPECT_EQ(r.size(), 4U);
    EXPECT_EQ(r[0], 6);
    EXPECT_EQ(r[1], 7);
    EXPECT_EQ(r[2], 8);
    EXPECT_EQ(r[3], 9);
}
