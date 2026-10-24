// RiskBook — exact-bit dedup of option legs into unique contracts; PreparedRiskBook —
// the grouped (uid, expiry_ns, side) ascending-K, 64-byte-aligned execution view.

#include "atx/vol/api/risk/risk_book.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/aligned.hpp"

namespace atx::vol {

using atx::core::Err;

namespace {

// Process-unique logical identities; 0 is reserved for "no book".
std::atomic<std::uint64_t> g_next_logical_id{1};

[[nodiscard]] std::uint64_t fresh_logical_id() noexcept {
  return g_next_logical_id.fetch_add(1, std::memory_order_relaxed);
}

struct UniqueKey {
  std::uint32_t uid;
  std::uint64_t k_bits;
  std::int64_t expiry_ns;
  Side side;
  [[nodiscard]] bool operator==(const UniqueKey &) const = default;
};

struct UniqueKeyHash {
  [[nodiscard]] std::size_t operator()(const UniqueKey &k) const noexcept {
    // splitmix-style mixing of the four fields; only distribution matters here.
    std::uint64_t h = k.k_bits * 0x9E3779B97F4A7C15ull;
    h ^= (static_cast<std::uint64_t>(k.uid) << 1) + 0x7F4A7C15ull + (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(k.expiry_ns) + 0x9E3779B9ull + (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(k.side) + (h << 6) + (h >> 2);
    return static_cast<std::size_t>(h);
  }
};

[[nodiscard]] double effective_multiplier(double m) noexcept {
  return (std::isfinite(m) && m > 0.0) ? m : 100.0;
}

// Owning, 64-byte-aligned column of a trivially copyable type (the shape
// prepared_portfolio.hpp's detail::AlignedColumn has). Move-only.
template <class T> class AlignedColumn {
  static_assert(std::is_trivially_copyable_v<T>, "AlignedColumn: T must be trivially copyable");

public:
  AlignedColumn() = default;
  explicit AlignedColumn(std::size_t n) : n_{n} {
    if (n_ != 0) {
      data_ = static_cast<T *>(
          atx::core::aligned_alloc_bytes(n_ * sizeof(T), atx::core::kCacheLineSize));
    }
  }
  ~AlignedColumn() { atx::core::aligned_free(data_); }
  AlignedColumn(AlignedColumn &&o) noexcept : data_{o.data_}, n_{o.n_} {
    o.data_ = nullptr;
    o.n_ = 0;
  }
  AlignedColumn &operator=(AlignedColumn &&o) noexcept {
    if (this != &o) {
      atx::core::aligned_free(data_);
      data_ = o.data_;
      n_ = o.n_;
      o.data_ = nullptr;
      o.n_ = 0;
    }
    return *this;
  }
  AlignedColumn(const AlignedColumn &) = delete;
  AlignedColumn &operator=(const AlignedColumn &) = delete;

  [[nodiscard]] bool ok() const noexcept { return n_ == 0 || data_ != nullptr; }
  [[nodiscard]] T *data() noexcept { return data_; }
  [[nodiscard]] std::span<const T> span() const noexcept { return {data_, n_}; }

private:
  T *data_{nullptr};
  std::size_t n_{0};
};

// Strict weak order on strikes that tolerates the NaN / signed-zero strikes an
// invalid leg may carry: finite order first, then NaN by bits, ties by bits so two
// distinct-bit uniques never compare equal.
[[nodiscard]] bool strike_less(double a, double b) noexcept {
  const bool na = std::isnan(a);
  const bool nb = std::isnan(b);
  if (!na && !nb) {
    if (a < b) {
      return true;
    }
    if (b < a) {
      return false;
    }
    return std::bit_cast<std::int64_t>(a) < std::bit_cast<std::int64_t>(b); // -0.0 < +0.0
  }
  if (na != nb) {
    return nb; // non-NaN before NaN
  }
  return std::bit_cast<std::uint64_t>(a) < std::bit_cast<std::uint64_t>(b);
}

} // namespace

// ── RiskBook ──────────────────────────────────────────────────────────────

struct RiskBook::Impl {
  std::vector<OptionLeg> legs;
  std::vector<ShareLeg> shares;
  std::vector<std::uint32_t> unique_slot; // per leg
  std::vector<OptionLeg> uniques;         // first leg that named each unique
  std::vector<std::uint32_t> uids;        // distinct, ascending (options AND shares)
  std::uint64_t logical_id{0};
};

RiskBook::RiskBook() : impl_{std::make_unique<Impl>()} { impl_->logical_id = fresh_logical_id(); }
RiskBook::~RiskBook() = default;
RiskBook::RiskBook(RiskBook &&) noexcept = default;
RiskBook &RiskBook::operator=(RiskBook &&) noexcept = default;

RiskBook::RiskBook(const RiskBook &other)
    : impl_{other.impl_ ? std::make_unique<Impl>(*other.impl_) : std::make_unique<Impl>()} {
  impl_->logical_id = fresh_logical_id(); // a copy is a distinct logical book
}

RiskBook &RiskBook::operator=(const RiskBook &other) {
  if (this != &other) {
    RiskBook copy{other};
    *this = std::move(copy);
  }
  return *this;
}

Result<RiskBook> RiskBook::create(std::span<const OptionLeg> options,
                                  std::span<const ShareLeg> shares) {
  constexpr std::size_t kMaxLegs = std::numeric_limits<std::uint32_t>::max();
  if (options.size() > kMaxLegs || shares.size() > kMaxLegs) {
    return Err(ErrorCode::InvalidArgument, "RiskBook::create: leg count exceeds UINT32_MAX");
  }
  RiskBook book;
  Impl &b = *book.impl_;
  b.legs.assign(options.begin(), options.end());
  b.shares.assign(shares.begin(), shares.end());
  b.unique_slot.resize(options.size());

  // Bounded reserve (Portfolio::create's rule): never a multi-million-bucket table
  // for a book that repeats a small listed universe.
  constexpr std::size_t kAutoReserveCap = 65'536u;
  std::unordered_map<UniqueKey, std::uint32_t, UniqueKeyHash> seen;
  seen.reserve(std::min(options.size(), kAutoReserveCap));
  for (std::size_t i = 0; i < options.size(); ++i) {
    const OptionLeg &l = options[i];
    const UniqueKey key{l.uid, std::bit_cast<std::uint64_t>(l.K), l.expiry_ns, l.side};
    const auto [it, inserted] =
        seen.try_emplace(key, static_cast<std::uint32_t>(b.uniques.size()));
    if (inserted) {
      b.uniques.push_back(l);
    }
    b.unique_slot[i] = it->second;
  }

  b.uids.reserve(b.uniques.size() + b.shares.size());
  for (const OptionLeg &u : b.uniques) {
    b.uids.push_back(u.uid);
  }
  for (const ShareLeg &s : b.shares) {
    b.uids.push_back(s.uid);
  }
  std::sort(b.uids.begin(), b.uids.end());
  b.uids.erase(std::unique(b.uids.begin(), b.uids.end()), b.uids.end());
  return book;
}

std::size_t RiskBook::n_option_legs() const noexcept { return impl_ ? impl_->legs.size() : 0u; }
std::size_t RiskBook::n_share_legs() const noexcept { return impl_ ? impl_->shares.size() : 0u; }
std::size_t RiskBook::n_unique_options() const noexcept {
  return impl_ ? impl_->uniques.size() : 0u;
}
std::size_t RiskBook::n_underliers() const noexcept { return impl_ ? impl_->uids.size() : 0u; }
std::span<const OptionLeg> RiskBook::option_legs() const noexcept {
  return impl_ ? std::span<const OptionLeg>{impl_->legs} : std::span<const OptionLeg>{};
}
std::span<const ShareLeg> RiskBook::share_legs() const noexcept {
  return impl_ ? std::span<const ShareLeg>{impl_->shares} : std::span<const ShareLeg>{};
}
std::span<const std::uint32_t> RiskBook::option_unique_slot() const noexcept {
  return impl_ ? std::span<const std::uint32_t>{impl_->unique_slot}
               : std::span<const std::uint32_t>{};
}
std::span<const OptionLeg> RiskBook::unique_options() const noexcept {
  return impl_ ? std::span<const OptionLeg>{impl_->uniques} : std::span<const OptionLeg>{};
}
std::span<const std::uint32_t> RiskBook::uids() const noexcept {
  return impl_ ? std::span<const std::uint32_t>{impl_->uids} : std::span<const std::uint32_t>{};
}
std::uint64_t RiskBook::logical_id() const noexcept { return impl_ ? impl_->logical_id : 0u; }

// ── PreparedRiskBook ──────────────────────────────────────────────────────

struct PreparedRiskBook::Impl {
  const RiskBook *book{nullptr};
  std::uint64_t book_logical_id{0};
  AlignedColumn<double> strike;
  AlignedColumn<std::int64_t> expiry_ns;
  AlignedColumn<std::uint32_t> uid;
  std::vector<Side> side;
  std::vector<std::uint32_t> prepared_to_unique;
  std::vector<std::uint32_t> unique_to_prepared;
  std::vector<RiskGroup> groups;
  std::vector<std::uint32_t> leg_prepared_row;
  std::vector<double> leg_weight;
  std::vector<std::uint32_t> uids;
  std::vector<std::uint32_t> share_uid_slot;
};

PreparedRiskBook::PreparedRiskBook() = default;
PreparedRiskBook::~PreparedRiskBook() = default;
PreparedRiskBook::PreparedRiskBook(PreparedRiskBook &&) noexcept = default;
PreparedRiskBook &PreparedRiskBook::operator=(PreparedRiskBook &&) noexcept = default;

Result<PreparedRiskBook> PreparedRiskBook::create(const RiskBook &book) {
  if (book.logical_id() == 0u) {
    return Err(ErrorCode::InvalidArgument, "PreparedRiskBook::create: moved-from book");
  }
  const std::span<const OptionLeg> uniques = book.unique_options();
  const std::size_t n = uniques.size();

  auto impl = std::make_unique<Impl>();
  Impl &p = *impl;
  p.book = &book;
  p.book_logical_id = book.logical_id();
  p.uids.assign(book.uids().begin(), book.uids().end());

  p.prepared_to_unique.resize(n);
  std::iota(p.prepared_to_unique.begin(), p.prepared_to_unique.end(), 0u);
  std::sort(p.prepared_to_unique.begin(), p.prepared_to_unique.end(),
            [&uniques](std::uint32_t a, std::uint32_t b) noexcept {
              const OptionLeg &x = uniques[a];
              const OptionLeg &y = uniques[b];
              if (x.uid != y.uid) {
                return x.uid < y.uid;
              }
              if (x.expiry_ns != y.expiry_ns) {
                return x.expiry_ns < y.expiry_ns;
              }
              if (x.side != y.side) {
                return static_cast<std::uint8_t>(x.side) < static_cast<std::uint8_t>(y.side);
              }
              if (strike_less(x.K, y.K) || strike_less(y.K, x.K)) {
                return strike_less(x.K, y.K);
              }
              return a < b; // distinct-bit uniques never tie; keep the order total anyway
            });
  p.unique_to_prepared.resize(n);
  for (std::uint32_t r = 0; r < n; ++r) {
    p.unique_to_prepared[p.prepared_to_unique[r]] = r;
  }

  p.strike = AlignedColumn<double>(n);
  p.expiry_ns = AlignedColumn<std::int64_t>(n);
  p.uid = AlignedColumn<std::uint32_t>(n);
  if (!p.strike.ok() || !p.expiry_ns.ok() || !p.uid.ok()) {
    return Err(ErrorCode::Internal, "PreparedRiskBook::create: aligned allocation failed");
  }
  p.side.resize(n);
  for (std::uint32_t r = 0; r < n; ++r) {
    const OptionLeg &u = uniques[p.prepared_to_unique[r]];
    p.strike.data()[r] = u.K;
    p.expiry_ns.data()[r] = u.expiry_ns;
    p.uid.data()[r] = u.uid;
    p.side[r] = u.side;
  }

  const auto uid_slot = [&p](std::uint32_t uid) noexcept {
    const auto it = std::lower_bound(p.uids.begin(), p.uids.end(), uid);
    return static_cast<std::uint32_t>(it - p.uids.begin());
  };
  for (std::uint32_t r = 0; r < n;) {
    const std::uint32_t uid = p.uid.data()[r];
    const std::int64_t expiry = p.expiry_ns.data()[r];
    const Side side = p.side[r];
    std::uint32_t e = r + 1u;
    while (e < n && p.uid.data()[e] == uid && p.expiry_ns.data()[e] == expiry &&
           p.side[e] == side) {
      ++e;
    }
    p.groups.push_back(RiskGroup{uid, expiry, side, r, e, uid_slot(uid)});
    r = e;
  }

  const std::span<const OptionLeg> legs = book.option_legs();
  const std::span<const std::uint32_t> slots = book.option_unique_slot();
  p.leg_prepared_row.resize(legs.size());
  p.leg_weight.resize(legs.size());
  for (std::size_t i = 0; i < legs.size(); ++i) {
    p.leg_prepared_row[i] = p.unique_to_prepared[slots[i]];
    p.leg_weight[i] = legs[i].qty * effective_multiplier(legs[i].multiplier);
  }
  const std::span<const ShareLeg> shares = book.share_legs();
  p.share_uid_slot.resize(shares.size());
  for (std::size_t j = 0; j < shares.size(); ++j) {
    p.share_uid_slot[j] = uid_slot(shares[j].uid);
  }

  PreparedRiskBook out;
  out.impl_ = std::move(impl);
  return out;
}

const RiskBook &PreparedRiskBook::book() const noexcept { return *impl_->book; }
std::size_t PreparedRiskBook::n_unique() const noexcept { return impl_ ? impl_->side.size() : 0u; }
std::size_t PreparedRiskBook::n_groups() const noexcept {
  return impl_ ? impl_->groups.size() : 0u;
}
std::span<const RiskGroup> PreparedRiskBook::groups() const noexcept {
  return impl_ ? std::span<const RiskGroup>{impl_->groups} : std::span<const RiskGroup>{};
}
std::span<const std::uint32_t> PreparedRiskBook::uid() const noexcept {
  return impl_ ? impl_->uid.span() : std::span<const std::uint32_t>{};
}
std::span<const double> PreparedRiskBook::strike() const noexcept {
  return impl_ ? impl_->strike.span() : std::span<const double>{};
}
std::span<const std::int64_t> PreparedRiskBook::expiry_ns() const noexcept {
  return impl_ ? impl_->expiry_ns.span() : std::span<const std::int64_t>{};
}
std::span<const Side> PreparedRiskBook::side() const noexcept {
  return impl_ ? std::span<const Side>{impl_->side} : std::span<const Side>{};
}
std::span<const std::uint32_t> PreparedRiskBook::prepared_to_unique() const noexcept {
  return impl_ ? std::span<const std::uint32_t>{impl_->prepared_to_unique}
               : std::span<const std::uint32_t>{};
}
std::span<const std::uint32_t> PreparedRiskBook::unique_to_prepared() const noexcept {
  return impl_ ? std::span<const std::uint32_t>{impl_->unique_to_prepared}
               : std::span<const std::uint32_t>{};
}
std::span<const std::uint32_t> PreparedRiskBook::leg_prepared_row() const noexcept {
  return impl_ ? std::span<const std::uint32_t>{impl_->leg_prepared_row}
               : std::span<const std::uint32_t>{};
}
std::span<const double> PreparedRiskBook::leg_weight() const noexcept {
  return impl_ ? std::span<const double>{impl_->leg_weight} : std::span<const double>{};
}
std::span<const std::uint32_t> PreparedRiskBook::uids() const noexcept {
  return impl_ ? std::span<const std::uint32_t>{impl_->uids} : std::span<const std::uint32_t>{};
}
std::span<const std::uint32_t> PreparedRiskBook::share_uid_slot() const noexcept {
  return impl_ ? std::span<const std::uint32_t>{impl_->share_uid_slot}
               : std::span<const std::uint32_t>{};
}
std::uint64_t PreparedRiskBook::book_logical_id() const noexcept {
  return impl_ ? impl_->book_logical_id : 0u;
}

} // namespace atx::vol
