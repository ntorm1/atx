// P9 lane COVR (ruling COV-12 (b)): the risk estimator's Debug-vs-Release attribution on the COV
// lane fixture, CovPanel(300, 200, 23) (the verb suites' verb_panel(), root's COV-7 fixture), under
// both recipes. Diagnostic only: DISABLED_, so it counts in no suite total, and it changes no
// output of record (no production source is touched; it calls the public estimator API). Run it in
// each build with
//   --gtest_also_run_disabled_tests --gtest_filter=CovStageDigest.*
// and compare the dumps with covr_cmp.py (sprint dir, cov-evidence/covr).
//
// It writes one FNV-1a 64 digest of the bit patterns per estimator stage and session, in pipeline
// order: the inputs; the CRT constants the estimator derives from its half-lives (exp2(-1/H), the
// strategy_risk_model.cpp LaggedEwma constructor and update_vra, and the Newey-West lag weights
// pow(d, l), LaggedEwma::push); then per session the exposures, the WLS factor returns, the
// recursive EWMA / Newey-West moments, the Kish count, the VRA, the fully observed block before the
// eigenvalue floor, after it, after the atx-cov-v1 eigen adjustment, the final F (structural border
// and VRA), the specific risk and the encoded container block. The factor side is a replica built
// from the production LaggedEwma / serial_adjusted_variance / eigen_adjust_block, checked equal bit
// for bit to the production F in every session (a replica that drifted would fail this test). Two
// isolated probes run the eigen kernel and the EWMA class on inputs identical in every build.
//
// Environment:
//   ATX_COVR_DUMP  file the digest lines go to (required; without it the test skips).
//   ATX_COVR_OUT   optional directory: the fixture's role + fields (in/) and one in-process `risk`
//                  verb run per recipe (v11/, covv1/) with root's COV-7 argv (--random-portfolios 4
//                  --emit-exposures all --emit-container --bias-families extended).
//   ATX_COVR_SWAP  optional, Windows: comma-separated CRT function names (e.g. "exp2") whose
//                  imports in every loaded module are rebound for this run to the OTHER C runtime's
//                  export
//                  (a /MDd Debug build: ucrtbase.dll; a /MD Release build: ucrtbased.dll), then
//                  restored. The counterfactual: if Debug with ucrtbase's exp2 writes Release's
//                  digests (and Release with ucrtbased's exp2 writes Debug's), that one function is
//                  the whole build-to-build difference on this fixture.
// Synthetic inputs only.

#include "strategy_cov_fixture.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// NOLINTBEGIN(misc-include-cleaner) -- windows.h is the Win32 umbrella; tlhelp32 the module walk.
#include <windows.h>
// tlhelp32.h needs windows.h first (a separate include block keeps clang-format from sorting it).
#include <tlhelp32.h>
// NOLINTEND(misc-include-cleaner)
#endif

namespace {
using namespace atx_cov_test;
constexpr usize kf = rk::factor_count;

#if defined(_DEBUG)
constexpr const char *build_type = "debug";
#else
constexpr const char *build_type = "release";
#endif

std::string env(const char *name) {
#if defined(_WIN32)
  char *value = nullptr;
  usize length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
    return {};
  std::string out(value);
  std::free(value);
  return out;
#else
  const char *value = std::getenv(name);
  return value ? std::string(value) : std::string();
#endif
}

// FNV-1a 64 over object representations.
struct Fnv {
  u64 h{0xcbf29ce484222325ULL};
  void bytes(const void *data, usize n) {
    const auto *p = static_cast<const unsigned char *>(data);
    for (usize i = 0; i < n; ++i) {
      h ^= p[i];
      h *= 0x100000001b3ULL;
    }
  }
  template <class T> Fnv &add(std::span<const T> s) {
    bytes(s.data(), s.size_bytes());
    return *this;
  }
  Fnv &value(f64 v) {
    bytes(&v, sizeof v);
    return *this;
  }
  Fnv &count(usize v) {
    const u64 x = v;
    bytes(&x, sizeof x);
    return *this;
  }
};
template <class T> u64 digest(std::span<const T> s) { return Fnv{}.add(s).h; }
std::string hex(u64 v) {
  std::ostringstream o;
  o << std::hex;
  o.width(16);
  o.fill('0');
  o << v;
  return o.str();
}
std::string bits(f64 v) { return hex(std::bit_cast<u64>(v)); }

struct Dump {
  std::ofstream out;
  explicit Dump(const std::string &path) : out(path, std::ios::binary | std::ios::trunc) {}
  void line(const std::string &key, const std::string &value) {
    out << key << ' ' << value << '\n';
  }
};

// The configuration's half-life as a run-time value (no constant fold of the CRT call in this TU).
f64 decay_of(usize half_life) {
  volatile usize h = half_life;
  const usize read = h;
  return read == 0 ? 1.0 : std::exp2(-1.0 / static_cast<f64>(read));
}
f64 lag_weight(f64 d, usize lag) {
  volatile f64 dv = d;
  volatile f64 lv = static_cast<f64>(lag);
  return std::pow(dv, lv);
}
Eigen::Index ix(usize i) { return static_cast<Eigen::Index>(i); }

#if defined(_WIN32)
std::string narrow(const wchar_t *w, usize n) {
  std::string s;
  for (usize i = 0; i < n; ++i)
    s.push_back(static_cast<char>(w[i] < 128 ? w[i] : L'?'));
  return s;
}
std::string module_of(const void *address) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(address), &m))
    return "?";
  std::array<wchar_t, MAX_PATH> path{};
  const DWORD n = GetModuleFileNameW(m, path.data(), static_cast<DWORD>(path.size()));
  return narrow(path.data(), n);
}
bool is_crt(const char *dll) {
  std::string s(dll);
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s == "ucrtbase.dll" || s == "ucrtbased.dll" || s.rfind("api-ms-win-crt-", 0) == 0;
}
// Every IAT slot, in every loaded module, that imports `name` by name from a C-runtime DLL.
struct Slot {
  std::string module;
  IMAGE_THUNK_DATA *slot{};
};
std::vector<Slot> crt_imports(const char *name) {
  std::vector<Slot> out;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE)
    return out;
  MODULEENTRY32W me{};
  me.dwSize = static_cast<DWORD>(sizeof(me));
  for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
    BYTE *base = me.modBaseAddr;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
      continue;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0)
      continue;
    for (auto *imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);
         imp->Name != 0; ++imp) {
      if (!is_crt(reinterpret_cast<const char *>(base + imp->Name)))
        continue;
      const DWORD names = imp->OriginalFirstThunk != 0 ? imp->OriginalFirstThunk : imp->FirstThunk;
      const auto *lookup = reinterpret_cast<const IMAGE_THUNK_DATA *>(base + names);
      auto *slot = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
      for (; lookup->u1.AddressOfData != 0; ++lookup, ++slot) {
        if (IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal))
          continue;
        const auto *by_name =
            reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + lookup->u1.AddressOfData);
        if (std::strcmp(reinterpret_cast<const char *>(by_name->Name), name) == 0)
          out.push_back({narrow(me.szModule, std::wcslen(me.szModule)), slot});
      }
    }
  }
  CloseHandle(snap);
  return out;
}
struct Rebound {
  IMAGE_THUNK_DATA *slot{};
  ULONG_PTR old{};
};
bool write_slot(IMAGE_THUNK_DATA *slot, ULONG_PTR value) {
  DWORD protect = 0;
  if (!VirtualProtect(&slot->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &protect))
    return false;
  slot->u1.Function = value;
  DWORD unused = 0;
  return VirtualProtect(&slot->u1.Function, sizeof(ULONG_PTR), protect, &unused) != 0;
}
#endif

// ---- the factor-side replica (strategy_risk_model.cpp fully_observed_forecast, adjust_block) ---
struct StageSink final : rk::RiskSink {
  Dump &dump;
  std::string recipe;
  const rk::RiskModelConfig &cfg;
  rk::LaggedEwma fast, nw, corr;
  usize replica_mismatches{}, sessions{};
  StageSink(Dump &d, std::string r, const rk::RiskModelConfig &c)
      : dump(d), recipe(std::move(r)), cfg(c), fast(kf, c.vol_halflife, 0, false),
        nw(kf, c.nw_halflife, c.variance_nw_lags, false),
        corr(kf, c.correlation_halflife, c.correlation_nw_lags, true) {}
  void stage(usize t, const char *name, u64 h) {
    dump.line("stage/" + recipe + "/" + std::to_string(t) + "/" + name, hex(h));
  }
  co::Status on_day(const rk::RiskDay &d) override {
    const usize t = d.date;
    ++sessions;
    // Exposures X_t and the WLS factor returns of t (on X_{t-1}).
    stage(t, "x.slot", digest(d.industry_slot));
    stage(t, "x.eligible", digest(d.eligible));
    stage(t, "x.styles", digest(d.styles));
    stage(t, "f.return", digest(d.factor_return));
    // The estimators take t's factor returns (update_vra, t > 0), then F_t reads them.
    if (t > 0) {
      fast.push(d.factor_return);
      nw.push(d.factor_return);
      corr.push(d.factor_return);
    }
    Fnv m_fast, m_base, m_hac, m_corr;
    for (usize k = 0; k < kf; ++k) {
      m_fast.value(fast.zero_lag(k, k));
      m_base.value(nw.zero_lag(k, k));
      m_hac.value(nw.covariance(k, k));
      for (usize j = 0; j < kf; ++j)
        m_corr.value(corr.covariance(k, j));
    }
    stage(t, "ewma.fast_var", m_fast.h);
    stage(t, "ewma.nw_zero_lag", m_base.h);
    stage(t, "ewma.nw_hac", m_hac.h);
    stage(t, "ewma.corr", m_corr.h);
    stage(t, "vra.kish_n_eff", Fnv{}.value(d.eigen_effective).h);
    stage(t, "vra.bias_factor", Fnv{}.value(d.bias_factor).h);
    stage(t, "vra.lambda2_factor", Fnv{}.value(d.lambda2_factor).h);
    // The fully observed block (fully_observed_forecast): variances, rho, floor, S2b.
    std::vector<usize> keep;
    std::vector<f64> var;
    for (usize k = 0; k < kf; ++k) {
      if (fast.observations(k) < cfg.min_factor_history)
        continue;
      const f64 v = rk::serial_adjusted_variance(fast, nw, k);
      if (!(std::isfinite(v) && v > 0))
        continue;
      keep.push_back(k);
      var.push_back(v);
    }
    Fnv h_var;
    for (usize a = 0; a < keep.size(); ++a)
      h_var.count(keep[a]).value(var[a]);
    stage(t, "F.serial_var", h_var.h);
    const usize m = keep.size();
    Eigen::MatrixXd f(ix(m), ix(m));
    for (usize a = 0; a < m; ++a) {
      f(ix(a), ix(a)) = var[a];
      const f64 ca = corr.covariance(keep[a], keep[a]);
      for (usize b = a + 1; b < m; ++b) {
        const f64 cb = corr.covariance(keep[b], keep[b]);
        const f64 cab = corr.covariance(keep[a], keep[b]);
        const f64 denom = std::sqrt(std::max(0.0, ca)) * std::sqrt(std::max(0.0, cb));
        const f64 rho = denom > 0 && std::isfinite(cab) ? std::clamp(cab / denom, -1.0, 1.0) : 0.0;
        const f64 value = rho * std::sqrt(var[a]) * std::sqrt(var[b]);
        f(ix(a), ix(b)) = value;
        f(ix(b), ix(a)) = value;
      }
    }
    const auto block_digest = [&](const Eigen::MatrixXd &g) {
      Fnv h;
      for (usize a = 0; a < m; ++a)
        for (usize b = 0; b < m; ++b)
          h.value(g(ix(a), ix(b)));
      return h.h;
    };
    stage(t, "F.raw", block_digest(f));
    bool floored = false;
    if (m > 0) {
      const Eigen::LLT<Eigen::MatrixXd> llt(f);
      if (llt.info() != Eigen::Success) {
        const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(f);
        if (eig.info() == Eigen::Success) {
          Eigen::VectorXd values = eig.eigenvalues();
          const f64 floor = std::max(1e-12, 1e-10 * f.trace() / static_cast<f64>(m));
          for (Eigen::Index e = 0; e < values.size(); ++e)
            values[e] = std::max(values[e], floor);
          Eigen::MatrixXd repaired =
              eig.eigenvectors() * values.asDiagonal() * eig.eigenvectors().transpose();
          f = 0.5 * (repaired + repaired.transpose());
          floored = true;
        }
      }
    }
    stage(t, "F.floor", Fnv{}.count(floored ? 1U : 0U).h ^ block_digest(f));
    rk::EigenOutcome outcome = rk::EigenOutcome::NotRun;
    std::vector<f64> gamma(kf, missing);
    if (m > 0 && cfg.recipe == rk::RiskRecipe::CovV1) {
      std::vector<f64> block(m * m);
      for (usize a = 0; a < m; ++a)
        for (usize b = 0; b < m; ++b)
          block[a * m + b] = f(ix(a), ix(b));
      const rk::EigenBlock adjusted =
          rk::eigen_adjust_block(block, m, d.eigen_effective, d.session, cfg);
      outcome = adjusted.outcome;
      if (adjusted.outcome == rk::EigenOutcome::Adjusted) {
        for (usize a = 0; a < m; ++a)
          for (usize b = 0; b < m; ++b)
            f(ix(a), ix(b)) = adjusted.covariance[a * m + b];
        for (usize k = 0; k < m && k < kf; ++k)
          gamma[k] = adjusted.gamma[k];
      }
    }
    stage(t, "F.eigen", Fnv{}.count(static_cast<usize>(outcome)).h ^ block_digest(f));
    stage(t, "F.eigen_gamma", digest(d.eigen_gamma));
    // Replica check: lambda2 f over the kept factors is the production F bit for bit.
    for (usize a = 0; a < m; ++a)
      for (usize b = 0; b < m; ++b) {
        const f64 want = d.lambda2_factor * f(ix(a), ix(b));
        if (std::bit_cast<u64>(want) != std::bit_cast<u64>(d.covariance[keep[a] * kf + keep[b]]))
          ++replica_mismatches;
      }
    if (outcome != d.eigen)
      ++replica_mismatches;
    for (usize k = 0; k < kf; ++k)
      if (std::bit_cast<u64>(gamma[k]) != std::bit_cast<u64>(d.eigen_gamma[k]))
        ++replica_mismatches;
    // F_t as published (structural border and VRA), its structural rows alone, the specific side.
    stage(t, "F.final", digest(d.covariance));
    Fnv border;
    for (usize k = 0; k < kf; ++k)
      if (d.factor_structural[k])
        border.count(k).add(d.covariance.subspan(k * kf, kf));
    stage(t, "F.structural_border", border.h);
    stage(t, "D.bias_specific", Fnv{}.value(d.bias_specific).h);
    stage(t, "D.lambda2_specific", Fnv{}.value(d.lambda2_specific).h);
    stage(t, "D.structural_bounds",
          Fnv{}.value(d.structural_sigma_lower).value(d.structural_sigma_upper).h);
    stage(t, "D.specific_variance", digest(d.specific_variance));
    stage(t, "D.flags", digest(d.specific_flags));
    return co::Ok();
  }
};

const CovPanel &lane_panel() {
  static const CovPanel p(300, 200, 23);
  return p;
}

// The eigen kernel on a planted block built from uniforms (+ - * / and Householder QR only), the
// same input bytes in every build.
std::vector<f64> iso_block(usize m) {
  u64 state = 0x9E3779B97F4A7C15ULL;
  const auto uniform = [&state] {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11U) * 0x1.0p-53 - 0.5;
  };
  Eigen::MatrixXd g(ix(m), ix(m));
  for (Eigen::Index a = 0; a < g.rows(); ++a)
    for (Eigen::Index b = 0; b < g.cols(); ++b)
      g(a, b) = uniform();
  const Eigen::HouseholderQR<Eigen::MatrixXd> qr(g);
  const Eigen::MatrixXd q = qr.householderQ();
  Eigen::VectorXd lambda(ix(m));
  for (usize k = 0; k < m; ++k)
    lambda[ix(k)] = 1e-6 * static_cast<f64>(1 + 3 * k);
  const Eigen::MatrixXd f = q * lambda.asDiagonal() * q.transpose();
  std::vector<f64> out(m * m);
  for (usize a = 0; a < m; ++a)
    for (usize b = 0; b < m; ++b)
      out[a * m + b] = 0.5 * (f(ix(a), ix(b)) + f(ix(b), ix(a)));
  return out;
}

TEST(CovStageDigest, DISABLED_DumpPerStageOnTheLaneFixture) {
  const std::string dump_path = env("ATX_COVR_DUMP");
  if (dump_path.empty())
    GTEST_SKIP() << "diagnostic: set ATX_COVR_DUMP (and optionally ATX_COVR_OUT, "
                    "ATX_COVR_SWAP)";
  Dump dump(dump_path);
  ASSERT_TRUE(dump.out.good()) << dump_path;
  dump.line("meta/build", build_type);
  const std::string swap = env("ATX_COVR_SWAP");
#if defined(_WIN32)
  for (const char *fn : {"exp2", "pow", "exp", "log", "sin", "cos"}) {
    const auto slots = crt_imports(fn);
    dump.line(std::string("meta/bound/") + fn,
              slots.empty()
                  ? std::string("not-imported")
                  : slots.front().module + " -> " +
                        module_of(reinterpret_cast<const void *>(slots.front().slot->u1.Function)));
  }
  std::vector<Rebound> rebound;
  if (!swap.empty()) {
#if defined(_DEBUG)
    const wchar_t *other = L"ucrtbase.dll";
#else
    const wchar_t *other = L"ucrtbased.dll";
#endif
    HMODULE crt = LoadLibraryExW(other, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_TRUE(crt != nullptr) << "other CRT not loadable";
    std::stringstream names(swap);
    std::string fn;
    while (std::getline(names, fn, ',')) {
      const FARPROC target = GetProcAddress(crt, fn.c_str());
      ASSERT_TRUE(target != nullptr) << fn;
      const auto address = reinterpret_cast<ULONG_PTR>(target);
      usize count = 0;
      for (const auto &s : crt_imports(fn.c_str())) {
        const ULONG_PTR old = s.slot->u1.Function;
        ASSERT_TRUE(write_slot(s.slot, address)) << fn;
        rebound.push_back({s.slot, old});
        ++count;
      }
      dump.line("meta/swap/" + fn, std::to_string(count) + " slot(s) -> " +
                                       module_of(reinterpret_cast<const void *>(address)));
    }
  }
#else
  if (!swap.empty())
    GTEST_SKIP() << "ATX_COVR_SWAP needs Windows";
#endif
  const CovPanel &p = lane_panel();
  // Inputs.
  dump.line("input/sessions", hex(digest(std::span<const i64>(p.sessions))));
  dump.line("input/ids", hex(digest(std::span<const u64>(p.ids))));
  dump.line("input/close", hex(digest(std::span<const f64>(p.close))));
  dump.line("input/raw_close", hex(digest(std::span<const f64>(p.raw))));
  dump.line("input/volume", hex(digest(std::span<const f64>(p.volume))));
  dump.line("input/cap", hex(digest(std::span<const f64>(p.cap))));
  dump.line("input/present", hex(digest(std::span<const u8>(p.present))));
  dump.line("input/member", hex(digest(std::span<const u8>(p.member))));
  dump.line("input/industry", hex(digest(std::span<const u8>(p.industry))));
  dump.line("input/value", hex(digest(std::span<const f32>(p.value))));
  dump.line("input/earnings", hex(digest(std::span<const f32>(p.earnings))));
  // The CRT constants of the default (both recipes') half-lives, as the estimator computes them:
  // LaggedEwma's decay 2^(-1/H) (strategy_risk_model.cpp, LaggedEwma constructor; Model builds
  // factor_fast HL vol, factor_nw HL nw, factor_corr HL corr, spec_fast HL specific, spec_nw HL
  // nw), update_vra's VRA and Kish decays (HL vra, HL corr), and push's lag weights pow(d, l).
  const rk::RiskModelConfig defaults{};
  const std::array<std::pair<const char *, usize>, 5> half_lives{
      {{"vol", defaults.vol_halflife},
       {"nw", defaults.nw_halflife},
       {"corr", defaults.correlation_halflife},
       {"specific", defaults.specific_halflife},
       {"vra", defaults.vra_halflife}}};
  for (const auto &[role, h] : half_lives) {
    const f64 arg = -1.0 / static_cast<f64>(h);
    const f64 d = decay_of(h);
    dump.line(std::string("const/exp2_arg/") + role + "_hl" + std::to_string(h), bits(arg));
    dump.line(std::string("const/exp2/") + role + "_hl" + std::to_string(h), bits(d));
    for (usize l = 1; l <= 5; ++l)
      dump.line(std::string("const/pow_lag/") + role + "_hl" + std::to_string(h) + "/" +
                    std::to_string(l),
                bits(lag_weight(d, l)));
  }
  // Isolated probes on inputs identical in every build.
  for (const usize m : {usize{10}, usize{30}}) {
    const auto block = iso_block(m);
    dump.line("iso/eigen_input_m" + std::to_string(m), hex(digest(std::span<const f64>(block))));
    rk::RiskModelConfig cfg;
    cfg.recipe = rk::RiskRecipe::CovV1;
    const auto e = rk::eigen_adjust_block(block, m, 250.0, p.sessions[150], cfg);
    dump.line("iso/eigen_output_m" + std::to_string(m),
              hex(Fnv{}
                      .count(static_cast<usize>(e.outcome))
                      .add(std::span<const f64>(e.covariance))
                      .add(std::span<const f64>(e.gamma))
                      .h));
  }
  // Recipes: the stage replica with the in-memory container sink (the verb's ContainerSink
  // encoding without the file), then the replica check.
  const auto panel = p.panel();
  std::vector<std::vector<f64>> factor_returns;
  for (const auto &[key, recipe] :
       {std::pair{"v11", rk::RiskRecipe::RiskV11}, std::pair{"covv1", rk::RiskRecipe::CovV1}}) {
    rk::RiskModelConfig cfg;
    cfg.recipe = recipe;
    StageSink stages(dump, key, cfg);
    BlockLog blocks;
    blocks.ids = p.ids;
    struct Returns final : rk::RiskSink {
      std::vector<std::vector<f64>> *out{};
      co::Status on_day(const rk::RiskDay &d) override {
        out->emplace_back(d.factor_return.begin(), d.factor_return.end());
        return co::Ok();
      }
    } returns;
    returns.out = &factor_returns;
    std::vector<rk::RiskSink *> sinks{&stages, &blocks};
    if (recipe == rk::RiskRecipe::RiskV11)
      sinks.push_back(&returns);
    const auto status = rk::run_risk_model(panel, cfg, sinks);
    ASSERT_TRUE(status) << status.error().to_string();
    for (usize t = 0; t < blocks.blocks.size(); ++t)
      dump.line("stage/" + std::string(key) + "/" + std::to_string(t) + "/block",
                hex(digest(std::span<const u8>(blocks.blocks[t]))));
    EXPECT_EQ(stages.sessions, p.d) << key;
    EXPECT_EQ(stages.replica_mismatches, 0U) << key << ": the replica drifted from production";
    dump.line("check/" + std::string(key) + "/replica_mismatches",
              std::to_string(stages.replica_mismatches));
  }
  // The EWMA class itself on the (build-identical) factor-return series at several half-lives:
  // equal across builds exactly where 2^(-1/H) is.
  for (const usize h : {usize{21}, usize{42}, usize{84}, usize{252}, usize{504}}) {
    rk::LaggedEwma e(kf, h, 5, true);
    for (usize t = 1; t < factor_returns.size(); ++t)
      e.push(factor_returns[t]);
    Fnv out;
    for (usize a = 0; a < kf; ++a)
      for (usize b = 0; b < kf; ++b)
        out.value(e.covariance(a, b));
    dump.line("iso/ewma_hl" + std::to_string(h) + "_lags5", hex(out.h));
  }
  // The verb on the written fixture (root's COV-7 argv), when asked.
  const std::string out_dir = env("ATX_COVR_OUT");
  if (!out_dir.empty()) {
    const fs::path root(out_dir);
    fs::create_directories(root);
    const Pinned pinned = write_role_and_fields(root / "in", p);
    dump.line("verb/role_sha256", pinned.role_sha);
    dump.line("verb/fields_sha256", pinned.fields_sha);
    for (const auto &[key, recipe] :
         {std::pair{"v11", "atx-risk-v1.1"}, std::pair{"covv1", "atx-cov-v1"}}) {
      std::string error;
      const int code = run_risk_verb(pinned, root / key, error,
                                     {"--emit-exposures", "all", "--emit-container",
                                      "--bias-families", "extended", "--recipe", recipe});
      EXPECT_EQ(code, 0) << key << ": " << error;
      if (code == 0)
        dump.line(std::string("verb/") + key + "/model.atxcov",
                  co::sha256_file((root / key / "model.atxcov").string()).value());
    }
  }
#if defined(_WIN32)
  for (auto it = rebound.rbegin(); it != rebound.rend(); ++it)
    EXPECT_TRUE(write_slot(it->slot, it->old));
#endif
  dump.out.flush();
  EXPECT_TRUE(dump.out.good());
}
} // namespace
