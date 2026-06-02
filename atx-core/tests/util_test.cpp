// util_test.cpp — TDD tests for atx::core util module.
//
// Covers: ScopeGuard (run, dismiss, move, double-dismiss, ATX_DEFER),
//         NonNull (deref, arrow, get, nullptr rejected at compile time),
//         EnumFlags (or, and, not, set, clear, test, to_underlying).

#include <atx/core/util.hpp>

#include <functional>

#include <gtest/gtest.h>

using namespace atx::core;

// =============================================================================
// ScopeGuard
// =============================================================================

TEST(ScopeGuard, RunsOnScopeExit) {
    int n = 0;
    {
        auto g = make_scope_guard([&] { ++n; });
        EXPECT_EQ(n, 0);
    }
    EXPECT_EQ(n, 1);
}

TEST(ScopeGuard, DismissPrevents) {
    int n = 0;
    {
        auto g = make_scope_guard([&] { ++n; });
        g.dismiss();
    }
    EXPECT_EQ(n, 0);
}

TEST(ScopeGuard, DoubleDismissIsSafe) {
    int n = 0;
    {
        auto g = make_scope_guard([&] { ++n; });
        g.dismiss();
        g.dismiss(); // second dismiss must be a no-op
    }
    EXPECT_EQ(n, 0);
}

TEST(ScopeGuard, MoveTransfersGuard) {
    int n = 0;
    {
        auto g1 = make_scope_guard([&] { ++n; });
        {
            auto g2 = std::move(g1); // ownership transfers to g2
            EXPECT_EQ(n, 0);         // g1 is now disarmed; g2 is armed
        }
        // g2 destroyed here — callback fires once
        EXPECT_EQ(n, 1);
    }
    // g1 destroyed here — must NOT fire again
    EXPECT_EQ(n, 1);
}

TEST(ScopeGuard, MoveAssignTransfersGuard) {
    // Move-assign requires the same F type. Use std::function<void()> to give
    // both guards an identical (erased) callable type so the assignment is valid.
    int n = 0;
    {
        // g1 increments n by 1; g2 increments n by 10.
        ScopeGuard<std::function<void()>> g1{std::function<void()>{[&] { ++n; }}};
        ScopeGuard<std::function<void()>> g2{
            std::function<void()>{[&] { n += 10; }}};
        // g2's old callback fires during move-assign (it was armed), then g2
        // adopts g1's callback and g1 is disarmed.
        g2 = std::move(g1);
        EXPECT_EQ(n, 10); // g2's old callback ran
    }
    // g2 (now holding g1's callback) fires; g1 is disarmed
    EXPECT_EQ(n, 11);
}

TEST(ScopeGuard, DeferMacroRunsAtScopeExit) {
    int n = 0;
    {
        ATX_DEFER(++n);
        EXPECT_EQ(n, 0);
    }
    EXPECT_EQ(n, 1);
}

TEST(ScopeGuard, DeferMacroMultipleOnSameLine) {
    // Two ATX_DEFER expansions on adjacent lines must each get unique names.
    int a = 0;
    int b = 0;
    {
        ATX_DEFER(++a);
        ATX_DEFER(++b);
    }
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 1);
}

// =============================================================================
// NonNull
// =============================================================================

TEST(NonNull, DerefsUnderlying) {
    int x = 7;
    NonNull<int *> p{&x};
    EXPECT_EQ(*p, 7);
}

TEST(NonNull, ArrowOperator) {
    struct Point {
        int x{3};
        int y{4};
    };
    Point pt{};
    NonNull<Point *> p{&pt};
    EXPECT_EQ(p->x, 3);
    EXPECT_EQ(p->y, 4);
}

TEST(NonNull, GetReturnsPointer) {
    int x = 42;
    NonNull<int *> p{&x};
    EXPECT_EQ(p.get(), &x);
}

TEST(NonNull, ConstDeref) {
    int x = 99;
    const NonNull<int *> p{&x};
    EXPECT_EQ(*p, 99);
}

// Compile-time check: NonNull<int*>(nullptr) must be deleted.
// (This is a static_assert / deleted-function check; verified by the header.)

// =============================================================================
// EnumFlags
// =============================================================================

enum class Perm : unsigned { Read = 1U, Write = 2U, Exec = 4U };

TEST(EnumFlags, OrAndTest) {
    auto f = EnumFlags<Perm>{Perm::Read} | Perm::Write;
    EXPECT_TRUE(f.test(Perm::Read));
    EXPECT_TRUE(f.test(Perm::Write));
    EXPECT_FALSE(f.test(Perm::Exec));
}

TEST(EnumFlags, SetAndClear) {
    EnumFlags<Perm> f{Perm::Read};
    f.set(Perm::Exec);
    EXPECT_TRUE(f.test(Perm::Exec));
    f.clear(Perm::Exec);
    EXPECT_FALSE(f.test(Perm::Exec));
}

TEST(EnumFlags, BitwiseAnd) {
    auto a = EnumFlags<Perm>{Perm::Read} | Perm::Write;
    auto b = EnumFlags<Perm>{Perm::Write} | Perm::Exec;
    auto c = a & b;
    EXPECT_FALSE(c.test(Perm::Read));
    EXPECT_TRUE(c.test(Perm::Write));
    EXPECT_FALSE(c.test(Perm::Exec));
}

TEST(EnumFlags, BitwiseNot) {
    // ~Read should have Write and Exec bits set (within the three-bit universe).
    auto f = ~EnumFlags<Perm>{Perm::Read};
    EXPECT_FALSE(f.test(Perm::Read));
    EXPECT_TRUE(f.test(Perm::Write));
    EXPECT_TRUE(f.test(Perm::Exec));
    // Full-width assertion: every bit except Read (bit 0) is set in a 32-bit
    // unsigned universe. Confirms the NOT runs on the full underlying width.
    EXPECT_EQ(f.to_underlying(), 0xFFFFFFFEU);
}

TEST(EnumFlags, ToUnderlying) {
    auto f = EnumFlags<Perm>{Perm::Read} | Perm::Exec;
    EXPECT_EQ(f.to_underlying(), 5U);
}

TEST(EnumFlags, DefaultConstructEmpty) {
    EnumFlags<Perm> f{};
    EXPECT_FALSE(f.test(Perm::Read));
    EXPECT_FALSE(f.test(Perm::Write));
    EXPECT_FALSE(f.test(Perm::Exec));
}

TEST(EnumFlags, FreeToUnderlying) {
    EXPECT_EQ(to_underlying(Perm::Write), 2U);
}
