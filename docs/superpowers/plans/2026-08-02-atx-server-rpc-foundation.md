# atx-server RPC Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `atx-server`, a loopback-only daemon that serves `atx-vol` surface-database metadata and serialized binary surfaces to many concurrent clients over length-prefixed protobuf on raw TCP.

**Architecture:** Four units. `atx-proto` holds the protobuf message contract and nothing else. `atx-rpc` holds the transport — socket shim, framing, dispatcher, server runtime, client — and is shared by the server and every future client so framing is never written twice. `atx-server` holds the domain: realm (the only path holder), a SQLite coverage catalog, a lazily-opened `SurfaceDb` registry, pure domain→proto encoders, auth, and two services. `atx-ui` is a separate spec and is not touched here.

**Tech Stack:** C++20, CMake 3.20+, vcpkg (protobuf, GTest), Winsock2, vendored SQLite via `atx::core::db`, `atx::vol` for surface storage and archive encode/decode.

**Spec:** `docs/superpowers/specs/2026-08-02-atx-server-rpc-foundation-design.md`

---

## Global Constraints

Every task's requirements implicitly include this section.

- **C++20.** `set(CMAKE_CXX_STANDARD 20)` is set repo-wide. `target_compile_features(<t> PUBLIC cxx_std_20)` on every new target, matching `atx-db/CMakeLists.txt`.
- **Error vocabulary.** All fallible functions return `atx::core::Result<T>` or `atx::core::Status` (`= Result<void>`) from `atx/core/error.hpp`. Construct with `Ok(v)`, `Ok()`, `Err(ErrorCode::X, "msg")`. Propagate with `ATX_TRY(auto x, expr)` / `ATX_TRY_VOID(expr)`. **Never** use exceptions for control flow. `Result` is `[[nodiscard]]`.
- **`ErrorCode` enumerators available** (`atx/core/error.hpp`): `Unknown, InvalidArgument, OutOfRange, NotFound, AlreadyExists, PermissionDenied, Unavailable, Internal, NotImplemented, IoError, ParseError`. There is no `Busy` or `DeadlineExceeded`; those exist only on the wire as `RpcCode` values.
- **Warnings.** Every hand-written target links `atx_warnings` (`/W4 /permissive- /WX`). Generated protobuf sources go in their own target **without** `atx_warnings`, following the `atx_sqlite3` precedent.
- **Naming refinement vs the spec.** The spec calls the wire status message `Status` and its enum `Code`. This plan names them **`RpcStatus`** and **`RpcCode`** because `atx::core::Status` already exists and means `Result<void>`; two things named `Status` in the same translation unit is a defect waiting to happen. Everything else matches the spec verbatim.
- **Namespaces.** Transport is `atx::rpc`. Server domain is `atx::server`. Generated protobuf is `atx::rpc::v1` (from `package atx.rpc.v1`).
- **Bind is loopback-only.** Non-loopback `--listen` must fail startup with a non-zero exit code. This is a hard gate, not a warning. There is no TLS in this build.
- **No filesystem path crosses the wire**, in either direction, in any message. Clients address data as `(db_id, partition_key, symbol)`.
- **Limits are configured, never unbounded.** Defaults: `max_frame_bytes = 64 MiB`, `max_blob_bytes = 16 MiB`, `max_connections = 256`, `ready_queue_depth = 1024`, `recv_timeout = 30s`, `send_timeout = 30s`.
- **Protocol version.** `kProtocolVersion = 1`. Validated once at handshake, not per call.
- **protoc** comes from the vcpkg tree, resolved by `find_package(Protobuf CONFIG REQUIRED)`. The chocolatey `protoc` on `PATH` must not be used.
- **Test label** is `atx_server` for every test in this plan, on all three subprojects. Selected with `ctest -L atx_server`.
- **Tests are built only under `ATX_BUILD_SERVER=ON` AND `ATX_BUILD_TESTS=ON`.**
- **Every commit** ends with the trailer `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.

---

## File Structure

### `atx-proto/` — message contract only, no transport, no logic

| File | Responsibility |
|---|---|
| `CMakeLists.txt` | `protobuf_generate` over the `.proto` set; produces target `atx-proto` |
| `atx/rpc/v1/envelope.proto` | `Envelope`, `RpcStatus`, `RpcCode`, `Hello`, `HelloAck` |
| `atx/rpc/v1/keys.proto` | `SymbolKey`, `ExpiryKey`, `OptionKey`, `PartitionKey`, `SurfaceKey` |
| `atx/rpc/v1/common.proto` | `ResponseMeta`, `Page`, `IsoDate` |
| `atx/rpc/v1/surface.proto` | `SurfaceMeta`, `ExpirySummary`, `VolCurveSlice`, `VolCurvePoint`, `VolQuotePoint`, `SurfaceDiagnostics`, `SymbolFitConfig`, `SurfaceBlob`, `CoverageCell` |
| `atx/rpc/v1/surface_service.proto` | request/response messages for the nine `SurfaceService` methods |
| `atx/rpc/v1/admin.proto` | `AdminService` messages, `RealmConfig`, `DatabaseInfo`, `ServerInfo`, `ServerStats` |

No `service` blocks: with no gRPC plugin they generate nothing. Method names are string constants in `atx-server`, checked against a declared list by the Task 17 agreement test.

### `atx-rpc/` — transport, shared by server and every client

| File | Responsibility |
|---|---|
| `include/atx/rpc/limits.hpp` | `FrameLimits`, `kFrameMagic`, `kProtocolVersion` |
| `include/atx/rpc/byte_stream.hpp` | `ByteStream` interface — the seam that makes framing testable with no socket |
| `include/atx/rpc/frame.hpp` + `src/frame.cpp` | `read_frame` / `write_frame`. Pure, fuzzable. Highest-risk file in the project. |
| `include/atx/rpc/socket.hpp` + `src/socket.cpp` | `WsaScope`, `Socket`, `Listener`, `SocketStream`. The only file containing `#ifdef _WIN32`. |
| `include/atx/rpc/call_context.hpp` | `CallContext`, `Entitlements` |
| `include/atx/rpc/method_table.hpp` + `src/method_table.cpp` | typed `add<Req,Resp>`; owns decode/encode so handlers never see bytes |
| `include/atx/rpc/service.hpp` | `Service` interface |
| `include/atx/rpc/dispatcher.hpp` + `src/dispatcher.cpp` | `Envelope` → `MethodTable` → `Envelope`. No socket. |
| `include/atx/rpc/server.hpp` + `src/server.cpp` | `RpcServer`: acceptor + poller + bounded worker pool + limits |
| `include/atx/rpc/client.hpp` + `src/client.cpp` | `RpcClient`: connect, handshake, `call<Req,Resp>` |
| `tests/*_test.cpp` | one per header |

### `atx-server/` — domain

| File | Responsibility |
|---|---|
| `include/atx/server/config.hpp` + `src/config.cpp` | `ServerConfig`, argv parsing, the loopback gate |
| `include/atx/server/realm.hpp` + `src/realm.cpp` | `db_id → {kind, root}`. The only path holder in the process. |
| `include/atx/server/catalog.hpp` + `src/catalog.cpp` | SQLite `:memory:` main + ATTACHed `state.db`; schema, migration, tokens, entitlements |
| `include/atx/server/catalog_scan.hpp` + `src/catalog_scan.cpp` | index a `SurfaceDb` into `main`; warm-start snapshot restore |
| `include/atx/server/catalog_refresh.hpp` + `src/catalog_refresh.cpp` | `CatalogRefresher` generation-drift poller thread |
| `include/atx/server/surface_registry.hpp` + `src/surface_registry.cpp` | lazily-opened `SurfaceDb` per `db_id` |
| `include/atx/server/encode.hpp` + `src/encode.cpp` | `atx::vol` domain → proto. Pure free functions, no transport. |
| `include/atx/server/auth.hpp` + `src/auth.cpp` | token sha256 → `Entitlements` |
| `include/atx/server/service_error.hpp` + `src/service_error.cpp` | `atx::core::Error` → `RpcStatus` |
| `include/atx/server/service_admin.hpp` + `src/service_admin.cpp` | `AdminServiceImpl` |
| `include/atx/server/service_surface.hpp` + `src/service_surface.cpp` | `SurfaceServiceImpl` |
| `include/atx/server/methods.hpp` | the declared method-name list, single source of truth |
| `include/atx/server/server.hpp` + `src/server.cpp` | `Server`: build / start / wait / shutdown |
| `tools/main.cpp` | the `atx-server` binary |
| `tools/cli.cpp` | the `atx-server-cli` client |
| `tests/*_test.cpp` | one per header |

---

## Task Dependency Order

```
1  build wiring
2  proto: envelope/keys/common          <- 1
3  rpc: frame                            <- 2
4  rpc: socket                           <- 1
5  rpc: dispatcher + method table        <- 2,3
6  rpc: server runtime                   <- 4,5
7  rpc: client + loopback echo e2e       <- 6
8  proto: surface + admin messages       <- 2
9  server: config + realm                <- 1
10 server: catalog schema/state/auth     <- 9
11 server: catalog scan/refresh/warmstart<- 10
12 server: surface_registry + encode     <- 8,11
13 server: service_error + AdminService  <- 5,12
14 server: SurfaceService read RPCs      <- 13
15 server: GetSurfaceBlob + fidelity     <- 14
16 server: daemon main + realm import    <- 7,15
17 server: CLI + agreement + final gate  <- 16
```

---

### Task 1: Build wiring

Stands up three empty-but-real subprojects, the `ATX_BUILD_SERVER` option, and the `server` preset, and removes the unused `grpc` dependency. Deliverable: `cmake --preset server` configures and builds; `cmake --preset dev` compiles no new code.

**Files:**
- Modify: `vcpkg.json`
- Modify: `CMakeLists.txt:259-287`
- Modify: `CMakePresets.json`
- Create: `atx-proto/CMakeLists.txt`
- Create: `atx-rpc/CMakeLists.txt`, `atx-rpc/include/atx/rpc/limits.hpp`, `atx-rpc/src/limits.cpp`
- Create: `atx-server/CMakeLists.txt`, `atx-server/include/atx/server/version.hpp`, `atx-server/src/version.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: CMake targets `atx-proto`, `atx-rpc` (alias `atx::rpc`), `atx-server-lib` (alias `atx::server`). Constants `atx::rpc::kFrameMagic`, `atx::rpc::kProtocolVersion`, `atx::rpc::FrameLimits`. Function `atx::server::version_string()`.

- [ ] **Step 1: Remove `grpc` from the vcpkg manifest**

`vcpkg.json` — delete the `"grpc",` line only. Keep `protobuf`.

```json
{
  "name": "atx",
  "version-string": "0.1.0",
  "dependencies": [
    { "name": "arrow", "features": ["parquet"] },
    "zstd",
    "zlib-ng",
    "openssl",
    "gtest",
    "protobuf"
  ],
  "builtin-baseline": "9e9398f90a6c386bbd6ed89714ddb036b2e969eb"
}
```

- [ ] **Step 2: Write `atx-rpc/include/atx/rpc/limits.hpp`**

```cpp
#pragma once

// atx::rpc — shared wire limits and protocol constants.
//
// Every limit here is a hard bound checked BEFORE any allocation. An
// attacker-controlled length that is trusted before validation is the classic
// hand-rolled-framer defect; see frame.hpp for where these are enforced.

#include <cstddef>
#include <cstdint>

namespace atx::rpc {

// 'ATXR' little-endian. Present so a client pointed at the wrong port fails on
// byte 0 with a clear diagnostic rather than on a confusing protobuf parse.
inline constexpr std::uint32_t kFrameMagic = 0x41545852u;

// Bumped on any breaking wire change. Validated once, at handshake.
inline constexpr std::uint32_t kProtocolVersion = 1u;

// Bytes of fixed prefix ahead of the protobuf payload: magic + length.
inline constexpr std::size_t kFrameHeaderBytes = 8u;

struct FrameLimits {
  // Largest accepted protobuf payload. Checked against the wire length before
  // the receive buffer is sized.
  std::size_t max_frame_bytes{64u * 1024u * 1024u};
};

} // namespace atx::rpc
```

- [ ] **Step 3: Write `atx-rpc/src/limits.cpp`**

A translation unit is needed so `atx-rpc` is a real compiled library from task 1 rather than a header blob.

```cpp
#include "atx/rpc/limits.hpp"

namespace atx::rpc {

// Anchors the library's TU set and gives the constants external linkage for
// debuggers. Additional transport sources land in tasks 3-7.
std::uint32_t protocol_version() noexcept { return kProtocolVersion; }

} // namespace atx::rpc
```

Add the declaration to `limits.hpp` above the closing brace:

```cpp
[[nodiscard]] std::uint32_t protocol_version() noexcept;
```

- [ ] **Step 4: Write `atx-server/include/atx/server/version.hpp` and `src/version.cpp`**

```cpp
// include/atx/server/version.hpp
#pragma once

#include <string>

namespace atx::server {

// Reported by AdminService.GetServerInfo and by `atx-server --version`.
[[nodiscard]] std::string version_string();

} // namespace atx::server
```

```cpp
// src/version.cpp
#include "atx/server/version.hpp"

namespace atx::server {

std::string version_string() { return "atx-server 0.1.0"; }

} // namespace atx::server
```

- [ ] **Step 5: Write `atx-proto/CMakeLists.txt`**

Generated sources get their own target with warnings suppressed, so `/W4 /WX` still gates every hand-written file.

```cmake
# ---- atx-proto: the ATX wire message contract ------------------------------
#
# Messages only. There are no `service` blocks: with no gRPC plugin installed
# they would generate nothing. Method names are string constants owned by
# atx-server (include/atx/server/methods.hpp) and checked against the contract
# by the agreement test.
#
# Generated .pb.cc files live in their own target WITHOUT atx_warnings, the
# same treatment atx_sqlite3 gets, so the repo-wide /W4 /WX gate keeps applying
# to hand-written code only.

find_package(Protobuf CONFIG REQUIRED)

set(ATX_PROTO_FILES
    atx/rpc/v1/envelope.proto
)

add_library(atx-proto STATIC ${ATX_PROTO_FILES})
add_library(atx::proto ALIAS atx-proto)

target_link_libraries(atx-proto PUBLIC protobuf::libprotobuf)
target_include_directories(atx-proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_compile_features(atx-proto PUBLIC cxx_std_20)

protobuf_generate(
    TARGET atx-proto
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)

if(MSVC)
    # Generated protobuf code trips C4127/C4267/C4244 under /W4. Suppressing on
    # this target only keeps the gate intact everywhere else.
    target_compile_options(atx-proto PRIVATE /W0)
endif()
```

- [ ] **Step 6: Write `atx-rpc/CMakeLists.txt`**

```cmake
# ---- atx-rpc: framed-protobuf transport over TCP ---------------------------
#
# Shared by atx-server and by every client (atx-ui, atx-server-cli, any future
# Python or web client). It exists so framing is written once; a second copy in
# a client would drift from this one.

add_library(atx-rpc ${ATX_LIB_TYPE}
    src/limits.cpp
)
add_library(atx::rpc ALIAS atx-rpc)

target_include_directories(atx-rpc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(atx-rpc PUBLIC cxx_std_20)
target_link_libraries(atx-rpc
    PUBLIC atx::core atx::proto
    PRIVATE atx_warnings
)

if(WIN32)
    target_link_libraries(atx-rpc PUBLIC ws2_32)
endif()

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
```

- [ ] **Step 7: Write `atx-rpc/tests/CMakeLists.txt`**

```cmake
file(GLOB ATX_RPC_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

if(NOT ATX_RPC_TEST_SOURCES)
    return()
endif()

add_executable(atx-rpc-tests ${ATX_RPC_TEST_SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(atx-rpc-tests
    PRIVATE atx::rpc atx::core GTest::gtest_main Threads::Threads atx_warnings)
target_compile_features(atx-rpc-tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(atx-rpc-tests
    PROPERTIES LABELS atx_server
    DISCOVERY_MODE PRE_TEST)
```

- [ ] **Step 8: Write `atx-server/CMakeLists.txt`**

```cmake
# ---- atx-server: the ATX trading-platform daemon ---------------------------
#
# The central long-running process. Clients never touch the filesystem; this is
# the only process that knows where data lives. Bind is loopback-only: there is
# no TLS in this build, so a remote bind would ship plaintext tokens.

add_library(atx-server-lib ${ATX_LIB_TYPE}
    src/version.cpp
)
add_library(atx::server ALIAS atx-server-lib)

target_include_directories(atx-server-lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(atx-server-lib PUBLIC cxx_std_20)
target_link_libraries(atx-server-lib
    PUBLIC atx::rpc atx::proto atx::vol atx::core
    PRIVATE atx_warnings
)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
```

- [ ] **Step 9: Write `atx-server/tests/CMakeLists.txt`**

```cmake
file(GLOB ATX_SERVER_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

if(NOT ATX_SERVER_TEST_SOURCES)
    return()
endif()

add_executable(atx-server-tests ${ATX_SERVER_TEST_SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(atx-server-tests
    PRIVATE atx::server atx::vol atx::core
            GTest::gtest_main Threads::Threads atx-test-scratch atx_warnings)
target_compile_features(atx-server-tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(atx-server-tests
    PROPERTIES LABELS atx_server
    DISCOVERY_MODE PRE_TEST)
```

- [ ] **Step 10: Wire the option and subdirectories into the root `CMakeLists.txt`**

Replace lines 259-287 (the `ATX_BUILD_TESTS` / `ATX_BUILD_UI` options through the final `endif()`) with:

```cmake
option(ATX_BUILD_TESTS "Build ATX tests" ${PROJECT_IS_TOP_LEVEL})
option(ATX_BUILD_UI "Build the ATX volatility desktop workspace" OFF)
option(ATX_BUILD_SERVER "Build the ATX server daemon (atx-proto + atx-rpc + atx-server)" OFF)

if(ATX_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()

add_subdirectory(atx-core)
add_subdirectory(atx-vol)
add_subdirectory(atx-tsdb)
add_subdirectory(atx-kb)
add_subdirectory(atx-db)
add_subdirectory(atx-engine)
add_subdirectory(atx-options-engine)
add_subdirectory(atx-impl)

# atx-proto and atx-rpc are shared by the server and by every client, so the UI
# needs them too (see 2026-08-02-atx-ui-hub-spoke-shell-design.md). Neither is
# configured for a plain `dev` build: an atx-vol developer compiles no protobuf.
if(ATX_BUILD_SERVER OR ATX_BUILD_UI)
    add_subdirectory(atx-proto)
    add_subdirectory(atx-rpc)
endif()
if(ATX_BUILD_SERVER)
    add_subdirectory(atx-server)
endif()
if(ATX_BUILD_UI)
    add_subdirectory(atx-ui)
endif()
```

Keep the existing explanatory comments above `find_package(GTest ...)` and `add_subdirectory(tests)` — they are unrelated to this change and must survive it.

- [ ] **Step 11: Add the `server` preset to `CMakePresets.json`**

In `configurePresets`, after the `dev` entry, add:

```json
{
  "name": "server",
  "inherits": "dev",
  "displayName": "server (dev + atx-server daemon)",
  "cacheVariables": {
    "ATX_BUILD_SERVER": "ON"
  }
}
```

In `buildPresets`, add:

```json
{
  "name": "server",
  "configurePreset": "server"
}
```

- [ ] **Step 12: Configure and build the server preset**

Run: `cmake --preset server`
Expected: configures with no error; the output mentions `atx-proto`, `atx-rpc`, `atx-server-lib`.

Run: `cmake --build --preset server`
Expected: builds clean. `atx-proto` has no `.proto` sources yet beyond `envelope.proto`, which arrives in Task 2 — until then `ATX_PROTO_FILES` references a file that does not exist, so **Task 2 must land before this step passes**. Perform Step 12 after Task 2 Step 3 and record it as done then.

- [ ] **Step 13: Verify the default build is unaffected**

Run: `cmake --preset dev && cmake --build --preset dev --target atx-vol`
Expected: succeeds; no protobuf, `atx-rpc`, or `atx-server` compilation appears in the log.

Run: `cmake --build --preset dev 2>&1 | grep -ciE "atx-proto|atx-rpc|atx-server"`
Expected: `0`

- [ ] **Step 14: Commit**

```bash
git add vcpkg.json CMakeLists.txt CMakePresets.json atx-proto atx-rpc atx-server
git commit -m "build(server): add atx-proto, atx-rpc, and atx-server subprojects

Three new subprojects behind ATX_BUILD_SERVER (default OFF), plus a `server`
preset. atx-proto and atx-rpc are also configured when ATX_BUILD_UI is on,
because the UI is a client of the same wire and must not carry its own copy of
the framing code.

Drops grpc from the vcpkg manifest. It was added on the assumption of a gRPC
transport that the replacement design abandoned; nothing links it, and leaving
it in the manifest costs a ~45-minute cold build on every fresh worktree
configure. Protobuf stays and is already installed.

Generated protobuf sources compile in their own target with /W0, the same
treatment atx_sqlite3 gets, so the repo-wide /W4 /WX gate still covers every
hand-written file.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: `atx-proto` — envelope, keys, and common messages

**Files:**
- Create: `atx-proto/atx/rpc/v1/envelope.proto`
- Create: `atx-proto/atx/rpc/v1/keys.proto`
- Create: `atx-proto/atx/rpc/v1/common.proto`
- Modify: `atx-proto/CMakeLists.txt` (extend `ATX_PROTO_FILES`)
- Create: `atx-rpc/tests/envelope_test.cpp`

**Interfaces:**
- Consumes: Task 1's `atx-proto` target.
- Produces: C++ types in namespace `atx::rpc::v1`: `Envelope`, `RpcStatus`, `RpcCode` (with enumerators `RPC_CODE_OK`, `RPC_CODE_NOT_FOUND`, `RPC_CODE_INVALID_ARGUMENT`, `RPC_CODE_PERMISSION_DENIED`, `RPC_CODE_UNAUTHENTICATED`, `RPC_CODE_RESOURCE_EXHAUSTED`, `RPC_CODE_FAILED_PRECONDITION`, `RPC_CODE_UNAVAILABLE`, `RPC_CODE_DATA_LOSS`, `RPC_CODE_DEADLINE_EXCEEDED`, `RPC_CODE_UNIMPLEMENTED`, `RPC_CODE_BUSY`, `RPC_CODE_INTERNAL`), `Hello`, `HelloAck`, `SymbolKey`, `ExpiryKey`, `OptionKey`, `PartitionKey`, `SurfaceKey`, `ResponseMeta`, `Page`, `IsoDate`. Header `atx/rpc/v1/envelope.pb.h` etc.

- [ ] **Step 1: Write the failing test**

`atx-rpc/tests/envelope_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "atx/rpc/v1/envelope.pb.h"

namespace {

TEST(Envelope, RoundTripsThroughSerialization) {
  atx::rpc::v1::Envelope out;
  out.set_protocol_version(1);
  out.set_correlation_id(0xDEADBEEFCAFEULL);
  out.set_is_response(false);
  out.set_method("atx.rpc.v1.SurfaceService/GetCurve");
  out.set_payload("\x01\x02\x03", 3);
  out.set_auth_token("tok", 3);
  out.set_deadline_unix_ns(1'722'000'000'000'000'000LL);

  std::string wire;
  ASSERT_TRUE(out.SerializeToString(&wire));

  atx::rpc::v1::Envelope in;
  ASSERT_TRUE(in.ParseFromString(wire));

  EXPECT_EQ(in.protocol_version(), 1u);
  EXPECT_EQ(in.correlation_id(), 0xDEADBEEFCAFEULL);
  EXPECT_FALSE(in.is_response());
  EXPECT_EQ(in.method(), "atx.rpc.v1.SurfaceService/GetCurve");
  EXPECT_EQ(in.payload(), std::string("\x01\x02\x03", 3));
  EXPECT_EQ(in.auth_token(), std::string("tok", 3));
  EXPECT_EQ(in.deadline_unix_ns(), 1'722'000'000'000'000'000LL);
}

TEST(RpcStatus, DefaultCodeIsOk) {
  const atx::rpc::v1::RpcStatus status;
  EXPECT_EQ(status.code(), atx::rpc::v1::RPC_CODE_OK);
  EXPECT_TRUE(status.message().empty());
  EXPECT_TRUE(status.incident_id().empty());
}

// The wire code set is deliberately gRPC's, so a later gRPC adoption is a
// mechanical swap rather than a semantic redesign. Pin the numbers: changing
// one silently reinterprets every response an older client already parsed.
TEST(RpcCode, EnumeratorValuesArePinned) {
  EXPECT_EQ(atx::rpc::v1::RPC_CODE_OK, 0);
  EXPECT_EQ(atx::rpc::v1::RPC_CODE_NOT_FOUND, 1);
  EXPECT_EQ(atx::rpc::v1::RPC_CODE_INVALID_ARGUMENT, 2);
  EXPECT_EQ(atx::rpc::v1::RPC_CODE_PERMISSION_DENIED, 3);
  EXPECT_EQ(atx::rpc::v1::RPC_CODE_UNAUTHENTICATED, 4);
  EXPECT_EQ(atx::rpc::v1::RPC_CODE_RESOURCE_EXHAUSTED, 5);
  EXPECT_EQ(atx::rpc::v1::RPC_CODE_FAILED_PRECONDITION, 6);
  EXPECT_EQ(atx::rpc::v1::RPC_CODE_UNAVAILABLE, 7);
  EXPECT_EQ(atx::rpc::v1::RPC_CODE_DATA_LOSS, 8);
  EXPECT_EQ(atx::rpc::v1::RPC_CODE_DEADLINE_EXCEEDED, 9);
  EXPECT_EQ(atx::rpc::v1::RPC_CODE_UNIMPLEMENTED, 10);
  EXPECT_EQ(atx::rpc::v1::RPC_CODE_BUSY, 11);
  EXPECT_EQ(atx::rpc::v1::RPC_CODE_INTERNAL, 12);
}

TEST(Hello, CarriesVersionAndSchemaHashes) {
  atx::rpc::v1::Hello hello;
  hello.set_protocol_version(1);
  hello.set_client_build("atx-server-cli 0.1.0");

  atx::rpc::v1::HelloAck ack;
  ack.set_protocol_version(1);
  ack.set_server_build("atx-server 0.1.0");
  ack.set_atxvsa_schema_hash(0x1234'5678'9ABC'DEF0ULL);
  ack.mutable_status()->set_code(atx::rpc::v1::RPC_CODE_OK);

  std::string wire;
  ASSERT_TRUE(ack.SerializeToString(&wire));
  atx::rpc::v1::HelloAck parsed;
  ASSERT_TRUE(parsed.ParseFromString(wire));
  EXPECT_EQ(parsed.atxvsa_schema_hash(), 0x1234'5678'9ABC'DEF0ULL);
  EXPECT_EQ(parsed.status().code(), atx::rpc::v1::RPC_CODE_OK);
}

} // namespace
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset server --target atx-rpc-tests`
Expected: FAIL at compile — `atx/rpc/v1/envelope.pb.h: No such file or directory`.

- [ ] **Step 3: Write `atx-proto/atx/rpc/v1/envelope.proto`**

```protobuf
syntax = "proto3";

package atx.rpc.v1;

// The transport envelope. Every frame on the wire carries exactly one of these
// behind an 8-byte [magic][length] prefix; see atx-rpc/include/atx/rpc/frame.hpp.

// Deliberately gRPC's status code set. The server's error mapping is written
// against these names, so adopting gRPC later is a mechanical swap rather than
// a semantic redesign. Values are pinned by a test: renumbering one silently
// reinterprets every response an older client already parsed.
enum RpcCode {
  RPC_CODE_OK                  = 0;
  RPC_CODE_NOT_FOUND           = 1;
  RPC_CODE_INVALID_ARGUMENT    = 2;
  RPC_CODE_PERMISSION_DENIED   = 3;
  RPC_CODE_UNAUTHENTICATED     = 4;
  RPC_CODE_RESOURCE_EXHAUSTED  = 5;
  RPC_CODE_FAILED_PRECONDITION = 6;
  RPC_CODE_UNAVAILABLE         = 7;
  RPC_CODE_DATA_LOSS           = 8;
  RPC_CODE_DEADLINE_EXCEEDED   = 9;
  RPC_CODE_UNIMPLEMENTED       = 10;
  RPC_CODE_BUSY                = 11;
  RPC_CODE_INTERNAL            = 12;
}

// Named RpcStatus, not Status, because atx::core::Status already exists and
// means Result<void>. Two things named Status in one translation unit is a
// defect waiting to happen.
message RpcStatus {
  RpcCode code        = 1;
  string  message     = 2;
  // Set only on RPC_CODE_INTERNAL. Correlates a client-visible failure to a
  // server log line without putting exception text on the wire.
  string  incident_id = 3;
}

message Envelope {
  // Validated once at handshake, not per call.
  uint32    protocol_version = 1;
  // Echoed on the response. v1 serves one connection strictly serially so
  // ordering alone would suffice; the field exists so pipelining can be added
  // later without a wire break.
  uint64    correlation_id   = 2;
  bool      is_response      = 3;
  // "atx.rpc.v1.SurfaceService/GetCurve". A string, not an id: debuggable in a
  // hex dump, no registry to keep in sync, and the hash lookup is free next to
  // an ATXVSA decode.
  string    method           = 4;
  bytes     payload          = 5;
  RpcStatus status           = 6;  // response only
  bytes     auth_token       = 7;  // request only
  // Checked at dispatch and at cooperative checkpoints. A handler already
  // inside a decode is NOT preempted; this is weaker than gRPC cancellation
  // and is documented as such rather than implied.
  int64     deadline_unix_ns = 8;  // request only; 0 = none
}

// First frame in each direction. A version mismatch closes the connection with
// a populated RpcStatus naming both versions, rather than hanging or producing
// a garbled decode 200 frames later.
message Hello {
  uint32 protocol_version = 1;
  string client_build     = 2;
}

message HelloAck {
  uint32    protocol_version   = 1;
  string    server_build       = 2;
  // Lets a client detect archive-layout skew at connect time instead of on the
  // first GetSurfaceBlob.
  uint64    atxvsa_schema_hash = 3;
  RpcStatus status             = 4;
}
```

- [ ] **Step 4: Write `atx-proto/atx/rpc/v1/keys.proto`**

```protobuf
syntax = "proto3";

package atx.rpc.v1;

// Primary keys. No filesystem path appears in any of these, by design: a
// db_id is an opaque client-facing identifier and the realm is the only place
// roots exist. See the design doc, section 4.7.

message SymbolKey {
  string symbol = 1;  // canonical, upper-case
}

message ExpiryKey {
  SymbolKey symbol     = 1;
  string    expiry_iso = 2;  // YYYY-MM-DD
}

message OptionKey {
  ExpiryKey expiry = 1;
  double    strike = 2;
  string    side   = 3;  // "C" or "P"
}

message PartitionKey {
  string db_id = 1;
  string key   = 2;  // e.g. a trading date, max 32 chars (kSurfaceDbKeyMax)
}

message SurfaceKey {
  PartitionKey partition = 1;
  SymbolKey    symbol    = 2;
}
```

- [ ] **Step 5: Write `atx-proto/atx/rpc/v1/common.proto`**

```protobuf
syntax = "proto3";

package atx.rpc.v1;

// Stamped on every response so clients revalidate rather than refetch.
message ResponseMeta {
  // The SurfaceDb manifest generation this response was computed from. Exactly
  // the value the catalog re-indexes on, so client-side revalidation is
  // correct by construction.
  uint64 db_generation = 1;
  uint64 content_hash  = 2;
  int64  server_ns     = 3;
}

message Page {
  uint32 offset = 1;
  uint32 limit  = 2;  // 0 = server default
}

message IsoDate {
  string value = 1;  // YYYY-MM-DD
}
```

- [ ] **Step 6: Extend `ATX_PROTO_FILES` in `atx-proto/CMakeLists.txt`**

```cmake
set(ATX_PROTO_FILES
    atx/rpc/v1/envelope.proto
    atx/rpc/v1/keys.proto
    atx/rpc/v1/common.proto
)
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `cmake --build --preset server --target atx-rpc-tests`
Then: `ctest --preset server -R "Envelope|RpcStatus|RpcCode|Hello" --output-on-failure`
Expected: 4 tests, all PASS.

- [ ] **Step 8: Complete Task 1 Step 12, now that `envelope.proto` exists**

Run: `cmake --build --preset server`
Expected: full build succeeds.

- [ ] **Step 9: Commit**

```bash
git add atx-proto atx-rpc/tests/envelope_test.cpp
git commit -m "feat(proto): define the ATX wire envelope, keys, and common messages

The envelope carries protocol version, correlation id, method name, payload,
status, auth token, and deadline. RpcCode is deliberately gRPC's status set so
the server's error mapping transfers unchanged if gRPC is adopted later; a test
pins the enumerator values, because renumbering one silently reinterprets every
response an older client already parsed.

Named RpcStatus rather than Status: atx::core::Status already exists and means
Result<void>, and two things named Status in one translation unit is a defect
waiting to happen.

Method is a string rather than an integer id. It is readable in a hex dump,
needs no registry kept in sync across client and server, and the hash lookup
costs nothing next to an ATXVSA decode.

No key message carries a filesystem path. A db_id is opaque to clients and the
realm is the only place roots exist.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: `atx-rpc` framing

The highest-risk code in the project. A hand-rolled framer that trusts an attacker-controlled length before validating it is the classic defect; this task closes it by construction and proves it with a corpus.

**Files:**
- Create: `atx-rpc/include/atx/rpc/byte_stream.hpp`
- Create: `atx-rpc/include/atx/rpc/frame.hpp`, `atx-rpc/src/frame.cpp`
- Create: `atx-rpc/tests/frame_test.cpp`
- Modify: `atx-rpc/CMakeLists.txt` (add `src/frame.cpp`)

**Interfaces:**
- Consumes: `atx::rpc::kFrameMagic`, `kFrameHeaderBytes`, `FrameLimits` (Task 1).
- Produces:
  - `class atx::rpc::ByteStream` with `virtual atx::core::Result<std::size_t> read_some(std::span<std::byte>)` and `virtual atx::core::Status write_all(std::span<const std::byte>)`.
  - `atx::core::Status atx::rpc::read_frame(ByteStream&, std::vector<std::byte>& out, const FrameLimits&)`
  - `atx::core::Status atx::rpc::write_frame(ByteStream&, std::span<const std::byte> payload, const FrameLimits&)`
  - `class atx::rpc::MemoryStream : public ByteStream` — test double, also used by Task 5.

- [ ] **Step 1: Write `atx-rpc/include/atx/rpc/byte_stream.hpp`**

This is written before its test because it is a pure interface with no behaviour to assert. The behaviour under test lives in `frame.cpp`.

```cpp
#pragma once

// atx::rpc::ByteStream — the seam that keeps framing testable.
//
// frame.cpp must be exercised against truncated, oversized, and hostile inputs.
// Driving that through a real socket would make the tests slow, flaky, and
// unable to express "the peer sent 3 bytes and then stopped forever". So
// framing talks to this interface instead, and the socket is one implementation
// of it (socket.hpp) while a std::vector is another (MemoryStream, below).

#include <cstddef>
#include <span>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::rpc {

class ByteStream {
public:
  virtual ~ByteStream() = default;

  ByteStream() = default;
  ByteStream(const ByteStream &) = delete;
  ByteStream &operator=(const ByteStream &) = delete;
  ByteStream(ByteStream &&) = delete;
  ByteStream &operator=(ByteStream &&) = delete;

  // Reads at least one and at most out.size() bytes. Returns 0 only when the
  // peer closed cleanly. A short read is normal and is NOT an error; callers
  // that need exactly N bytes must loop (read_frame does).
  [[nodiscard]] virtual atx::core::Result<std::size_t> read_some(std::span<std::byte> out) = 0;

  // Writes every byte or fails. Partial writes are retried internally.
  [[nodiscard]] virtual atx::core::Status write_all(std::span<const std::byte> in) = 0;
};

// In-memory ByteStream for tests and for the dispatcher's unit tests. Reads
// drain `inbox` from the front; writes append to `outbox`.
class MemoryStream final : public ByteStream {
public:
  MemoryStream() = default;
  explicit MemoryStream(std::vector<std::byte> inbox) : inbox_{std::move(inbox)} {}

  [[nodiscard]] atx::core::Result<std::size_t> read_some(std::span<std::byte> out) override;
  [[nodiscard]] atx::core::Status write_all(std::span<const std::byte> in) override;

  [[nodiscard]] const std::vector<std::byte> &outbox() const noexcept { return outbox_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return inbox_.size() - read_pos_; }

  // Caps how many bytes a single read_some returns, so a test can force the
  // short-read path that a real network produces but a vector never would.
  void set_chunk_limit(std::size_t n) noexcept { chunk_limit_ = n; }

private:
  std::vector<std::byte> inbox_;
  std::vector<std::byte> outbox_;
  std::size_t read_pos_{0};
  std::size_t chunk_limit_{0}; // 0 = unlimited
};

} // namespace atx::rpc
```

- [ ] **Step 2: Write the failing test**

`atx-rpc/tests/frame_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/rpc/byte_stream.hpp"
#include "atx/rpc/frame.hpp"
#include "atx/rpc/limits.hpp"

namespace {

using atx::core::ErrorCode;
using atx::rpc::ByteStream;
using atx::rpc::FrameLimits;
using atx::rpc::kFrameMagic;
using atx::rpc::MemoryStream;
using atx::rpc::read_frame;
using atx::rpc::write_frame;

std::vector<std::byte> bytes_of(std::initializer_list<std::uint8_t> vals) {
  std::vector<std::byte> out;
  out.reserve(vals.size());
  for (const std::uint8_t v : vals) {
    out.push_back(static_cast<std::byte>(v));
  }
  return out;
}

// Builds a raw wire buffer with an arbitrary magic and length, so a test can
// state a length the payload does not actually have.
std::vector<std::byte> raw_frame(std::uint32_t magic, std::uint32_t length,
                                 std::span<const std::byte> payload) {
  std::vector<std::byte> out(8 + payload.size());
  std::memcpy(out.data() + 0, &magic, 4);
  std::memcpy(out.data() + 4, &length, 4);
  if (!payload.empty()) {
    std::memcpy(out.data() + 8, payload.data(), payload.size());
  }
  return out;
}

TEST(Frame, RoundTripsAPayload) {
  const std::vector<std::byte> payload = bytes_of({0xDE, 0xAD, 0xBE, 0xEF});
  MemoryStream sink;
  ASSERT_TRUE(write_frame(sink, payload, FrameLimits{}));

  MemoryStream source{sink.outbox()};
  std::vector<std::byte> got;
  ASSERT_TRUE(read_frame(source, got, FrameLimits{}));
  EXPECT_EQ(got, payload);
  EXPECT_EQ(source.remaining(), 0u);
}

TEST(Frame, RoundTripsAnEmptyPayload) {
  MemoryStream sink;
  ASSERT_TRUE(write_frame(sink, {}, FrameLimits{}));
  EXPECT_EQ(sink.outbox().size(), 8u);

  MemoryStream source{sink.outbox()};
  std::vector<std::byte> got;
  ASSERT_TRUE(read_frame(source, got, FrameLimits{}));
  EXPECT_TRUE(got.empty());
}

// A real socket delivers a frame in arbitrary pieces. Force every split point.
TEST(Frame, ReassemblesAcrossEveryShortReadBoundary) {
  const std::vector<std::byte> payload = bytes_of({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
  MemoryStream sink;
  ASSERT_TRUE(write_frame(sink, payload, FrameLimits{}));

  for (std::size_t chunk = 1; chunk <= sink.outbox().size(); ++chunk) {
    MemoryStream source{sink.outbox()};
    source.set_chunk_limit(chunk);
    std::vector<std::byte> got;
    ASSERT_TRUE(read_frame(source, got, FrameLimits{})) << "chunk=" << chunk;
    EXPECT_EQ(got, payload) << "chunk=" << chunk;
  }
}

TEST(Frame, RejectsBadMagicWithParseError) {
  const std::vector<std::byte> wire = raw_frame(0xFFFFFFFFu, 0u, {});
  MemoryStream source{wire};
  std::vector<std::byte> got;
  const auto status = read_frame(source, got, FrameLimits{});
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::ParseError);
}

// The defect this whole task exists to prevent: a declared length far larger
// than the limit must be refused BEFORE the receive buffer is sized.
TEST(Frame, RejectsDeclaredLengthOverTheLimitWithoutAllocating) {
  FrameLimits limits;
  limits.max_frame_bytes = 1024;

  const std::vector<std::byte> wire = raw_frame(kFrameMagic, 0xFFFF'FFFFu, {});
  MemoryStream source{wire};
  std::vector<std::byte> got;
  const auto status = read_frame(source, got, limits);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::OutOfRange);
  // Nothing was reserved on the strength of the attacker's number.
  EXPECT_EQ(got.capacity(), 0u);
}

TEST(Frame, RejectsLengthExactlyOneOverTheLimit) {
  FrameLimits limits;
  limits.max_frame_bytes = 16;
  const std::vector<std::byte> wire = raw_frame(kFrameMagic, 17u, {});
  MemoryStream source{wire};
  std::vector<std::byte> got;
  const auto status = read_frame(source, got, limits);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::OutOfRange);
}

TEST(Frame, AcceptsLengthExactlyAtTheLimit) {
  FrameLimits limits;
  limits.max_frame_bytes = 4;
  const std::vector<std::byte> payload = bytes_of({9, 9, 9, 9});
  MemoryStream sink;
  ASSERT_TRUE(write_frame(sink, payload, limits));

  MemoryStream source{sink.outbox()};
  std::vector<std::byte> got;
  ASSERT_TRUE(read_frame(source, got, limits));
  EXPECT_EQ(got, payload);
}

TEST(Frame, TruncatedHeaderIsUnavailableNotParseError) {
  MemoryStream source{bytes_of({0x52, 0x58})}; // 2 of the 8 prefix bytes
  std::vector<std::byte> got;
  const auto status = read_frame(source, got, FrameLimits{});
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::Unavailable);
}

TEST(Frame, TruncatedPayloadIsUnavailableNotParseError) {
  const std::vector<std::byte> wire = raw_frame(kFrameMagic, 8u, bytes_of({1, 2, 3}));
  MemoryStream source{wire};
  std::vector<std::byte> got;
  const auto status = read_frame(source, got, FrameLimits{});
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::Unavailable);
}

TEST(Frame, CleanCloseBeforeAnyByteIsUnavailable) {
  MemoryStream source; // empty inbox == peer closed
  std::vector<std::byte> got;
  const auto status = read_frame(source, got, FrameLimits{});
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::Unavailable);
}

TEST(Frame, WriteRefusesAPayloadOverTheLimit) {
  FrameLimits limits;
  limits.max_frame_bytes = 4;
  const std::vector<std::byte> payload = bytes_of({1, 2, 3, 4, 5});
  MemoryStream sink;
  const auto status = write_frame(sink, payload, limits);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::OutOfRange);
  EXPECT_TRUE(sink.outbox().empty()); // nothing half-written
}

TEST(Frame, ReadsTwoFramesBackToBack) {
  const std::vector<std::byte> a = bytes_of({0xAA});
  const std::vector<std::byte> b = bytes_of({0xBB, 0xCC});
  MemoryStream sink;
  ASSERT_TRUE(write_frame(sink, a, FrameLimits{}));
  ASSERT_TRUE(write_frame(sink, b, FrameLimits{}));

  MemoryStream source{sink.outbox()};
  std::vector<std::byte> got;
  ASSERT_TRUE(read_frame(source, got, FrameLimits{}));
  EXPECT_EQ(got, a);
  ASSERT_TRUE(read_frame(source, got, FrameLimits{}));
  EXPECT_EQ(got, b);
  EXPECT_EQ(source.remaining(), 0u);
}

// Hostile-input corpus. None of these may hang, over-read, or over-allocate;
// every one must produce a typed error.
TEST(Frame, HostileCorpusAlwaysFailsCleanly) {
  const FrameLimits limits{.max_frame_bytes = 4096};
  const std::vector<std::vector<std::byte>> corpus = {
      {},                                                    // nothing
      bytes_of({0x52}),                                      // 1 byte
      bytes_of({0x52, 0x58, 0x54, 0x41}),                    // header half only
      bytes_of({0x52, 0x58, 0x54, 0x41, 0xFF}),              // 5 bytes
      raw_frame(kFrameMagic, 0x8000'0000u, {}),              // length with high bit set
      raw_frame(kFrameMagic, 4095u, bytes_of({0x01})),       // huge claim, 1 byte
      raw_frame(0u, 0u, {}),                                 // zero magic
      raw_frame(kFrameMagic ^ 1u, 4u, bytes_of({1, 2, 3, 4})), // one-bit magic flip
  };

  for (std::size_t i = 0; i < corpus.size(); ++i) {
    MemoryStream source{corpus[i]};
    std::vector<std::byte> got;
    const auto status = read_frame(source, got, limits);
    EXPECT_FALSE(status) << "corpus index " << i << " was accepted";
    if (!status) {
      const ErrorCode code = status.error().code();
      EXPECT_TRUE(code == ErrorCode::ParseError || code == ErrorCode::Unavailable ||
                  code == ErrorCode::OutOfRange)
          << "corpus index " << i << " gave " << atx::core::to_string(code);
    }
  }
}

} // namespace
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --build --preset server --target atx-rpc-tests`
Expected: FAIL at compile — `atx/rpc/frame.hpp: No such file or directory`.

- [ ] **Step 4: Write `atx-rpc/include/atx/rpc/frame.hpp`**

```cpp
#pragma once

// atx::rpc framing — an 8-byte fixed prefix, then one protobuf payload.
//
//   [u32 magic = kFrameMagic][u32 payload_len]      little-endian, 8 bytes
//   [payload_len bytes]
//
// `payload_len` is validated against FrameLimits::max_frame_bytes BEFORE the
// receive buffer is sized. Trusting an attacker-controlled length ahead of that
// check is the classic hand-rolled-framer defect, and the corpus test in
// frame_test.cpp exists to keep it closed.
//
// Error vocabulary, chosen so a caller can act on it without parsing strings:
//   ParseError   the bytes are not an ATX frame (bad magic) -> close, log
//   OutOfRange   a well-formed frame that exceeds a limit   -> close, log
//   Unavailable  the peer closed or stalled mid-frame       -> close, no alarm

#include <cstddef>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/rpc/byte_stream.hpp"
#include "atx/rpc/limits.hpp"

namespace atx::rpc {

// Reads exactly one frame. `out` is resized to the payload length and
// overwritten; its prior contents are not preserved, so a caller may reuse one
// buffer across frames without reallocating.
[[nodiscard]] atx::core::Status read_frame(ByteStream &stream, std::vector<std::byte> &out,
                                           const FrameLimits &limits);

// Writes exactly one frame. Refuses an over-limit payload without emitting any
// bytes, so a rejected write never leaves a half-frame on the wire.
[[nodiscard]] atx::core::Status write_frame(ByteStream &stream,
                                            std::span<const std::byte> payload,
                                            const FrameLimits &limits);

} // namespace atx::rpc
```

- [ ] **Step 5: Write `atx-rpc/src/frame.cpp`**

```cpp
#include "atx/rpc/frame.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace atx::rpc {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Status;

// The wire is little-endian by definition, and every target is little-endian,
// but reading through memcpy on explicit byte offsets keeps that a property of
// this function rather than of the host.
[[nodiscard]] std::uint32_t load_u32_le(const std::byte *p) noexcept {
  std::uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

void store_u32_le(std::byte *p, std::uint32_t v) noexcept { std::memcpy(p, &v, sizeof(v)); }

// Fills `out` completely or fails. A zero-length read_some means the peer
// closed; mid-frame that is a truncation, which is Unavailable rather than
// ParseError -- the bytes were not malformed, they merely stopped.
[[nodiscard]] Status read_exact(ByteStream &stream, std::span<std::byte> out) {
  std::size_t filled = 0;
  while (filled < out.size()) {
    ATX_TRY(const std::size_t n, stream.read_some(out.subspan(filled)));
    if (n == 0) {
      return Err(ErrorCode::Unavailable, "peer closed after " + std::to_string(filled) +
                                             " of " + std::to_string(out.size()) + " bytes");
    }
    filled += n;
  }
  return Ok();
}

} // namespace

Status read_frame(ByteStream &stream, std::vector<std::byte> &out, const FrameLimits &limits) {
  std::array<std::byte, kFrameHeaderBytes> header{};
  ATX_TRY_VOID(read_exact(stream, header));

  const std::uint32_t magic = load_u32_le(header.data());
  if (magic != kFrameMagic) {
    return Err(ErrorCode::ParseError,
               "frame magic 0x" + std::to_string(magic) + " is not an ATX frame");
  }

  const std::uint32_t declared = load_u32_le(header.data() + 4);

  // THE check. Nothing is allocated on the strength of `declared` until it has
  // been proven to sit inside the configured bound.
  if (static_cast<std::size_t>(declared) > limits.max_frame_bytes) {
    return Err(ErrorCode::OutOfRange, "frame length " + std::to_string(declared) +
                                          " exceeds max_frame_bytes " +
                                          std::to_string(limits.max_frame_bytes));
  }

  out.clear();
  if (declared == 0) {
    return Ok();
  }
  out.resize(declared);

  const Status body = read_exact(stream, std::span<std::byte>{out});
  if (!body) {
    out.clear();
    return body;
  }
  return Ok();
}

Status write_frame(ByteStream &stream, std::span<const std::byte> payload,
                   const FrameLimits &limits) {
  if (payload.size() > limits.max_frame_bytes) {
    return Err(ErrorCode::OutOfRange, "payload " + std::to_string(payload.size()) +
                                          " exceeds max_frame_bytes " +
                                          std::to_string(limits.max_frame_bytes));
  }
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Err(ErrorCode::OutOfRange, "payload does not fit a u32 length prefix");
  }

  // One buffer, one write_all: a header write that succeeds followed by a body
  // write that fails would leave a half-frame the peer can never resynchronize
  // from.
  std::vector<std::byte> wire(kFrameHeaderBytes + payload.size());
  store_u32_le(wire.data(), kFrameMagic);
  store_u32_le(wire.data() + 4, static_cast<std::uint32_t>(payload.size()));
  if (!payload.empty()) {
    std::memcpy(wire.data() + kFrameHeaderBytes, payload.data(), payload.size());
  }
  return stream.write_all(wire);
}

} // namespace atx::rpc
```

- [ ] **Step 6: Write `atx-rpc/src/byte_stream.cpp` (the `MemoryStream` bodies)**

```cpp
#include "atx/rpc/byte_stream.hpp"

#include <algorithm>
#include <cstring>

namespace atx::rpc {

atx::core::Result<std::size_t> MemoryStream::read_some(std::span<std::byte> out) {
  const std::size_t available = inbox_.size() - read_pos_;
  std::size_t n = std::min(out.size(), available);
  if (chunk_limit_ != 0) {
    n = std::min(n, chunk_limit_);
  }
  if (n > 0) {
    std::memcpy(out.data(), inbox_.data() + read_pos_, n);
    read_pos_ += n;
  }
  // n == 0 with an exhausted inbox is a clean close, which is exactly what a
  // truncated-input test needs to express.
  return atx::core::Ok(n);
}

atx::core::Status MemoryStream::write_all(std::span<const std::byte> in) {
  outbox_.insert(outbox_.end(), in.begin(), in.end());
  return atx::core::Ok();
}

} // namespace atx::rpc
```

- [ ] **Step 7: Add the new sources to `atx-rpc/CMakeLists.txt`**

```cmake
add_library(atx-rpc ${ATX_LIB_TYPE}
    src/limits.cpp
    src/byte_stream.cpp
    src/frame.cpp
)
```

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cmake --build --preset server --target atx-rpc-tests`
Then: `ctest --preset server -R "^Frame\." --output-on-failure`
Expected: 13 tests, all PASS.

- [ ] **Step 9: Commit**

```bash
git add atx-rpc/include/atx/rpc/byte_stream.hpp atx-rpc/include/atx/rpc/frame.hpp \
        atx-rpc/src/byte_stream.cpp atx-rpc/src/frame.cpp \
        atx-rpc/tests/frame_test.cpp atx-rpc/CMakeLists.txt
git commit -m "feat(rpc): add length-prefixed framing with bounded allocation

An 8-byte little-endian [magic][length] prefix ahead of one protobuf payload.
The magic makes a client pointed at the wrong port fail on byte 0 with a clear
diagnostic instead of on a confusing parse error much later.

The declared length is checked against max_frame_bytes before the receive
buffer is sized. Trusting an attacker-controlled length ahead of that check is
the defect hand-rolled framers reliably ship, so the corpus test asserts that a
0xFFFFFFFF claim leaves the output buffer at zero capacity.

Framing talks to a ByteStream interface rather than a socket. Truncation,
stalls, and hostile lengths are then expressible as plain vectors, and the
tests are neither slow nor flaky. MemoryStream's chunk limit forces the
short-read path a real network produces and a vector never would; the
reassembly test walks every split point in the frame.

Errors are typed so a caller can act without parsing strings: ParseError means
these are not ATX bytes, OutOfRange means a well-formed frame broke a limit,
and Unavailable means the peer stopped mid-frame -- the last of which is normal
disconnection, not an alarm.

write_frame builds the whole frame in one buffer and issues a single write. A
header write that succeeded followed by a failed body write would leave a
half-frame the peer could never resynchronize from.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: `atx-rpc` socket shim

**Files:**
- Create: `atx-rpc/include/atx/rpc/socket.hpp`, `atx-rpc/src/socket.cpp`
- Create: `atx-rpc/tests/socket_test.cpp`
- Modify: `atx-rpc/CMakeLists.txt`

**Interfaces:**
- Consumes: `ByteStream` (Task 3).
- Produces:
  - `atx::rpc::WsaScope` — process-wide `WSAStartup`/`WSACleanup`, refcounted, no-op off Windows.
  - `atx::rpc::Socket` — move-only RAII handle. `static Result<Socket> connect(std::string_view host, std::uint16_t port, std::chrono::milliseconds timeout)`, `Status set_timeouts(ms recv, ms send)`, `void close() noexcept`, `bool valid() const noexcept`, `NativeHandle native() const noexcept`.
  - `atx::rpc::Listener` — `static Result<Listener> bind(std::string_view host, std::uint16_t port, int backlog)`, `Result<Socket> accept()`, `std::uint16_t port() const noexcept` (resolved, so an ephemeral `:0` bind reports its assigned port), `void close() noexcept`.
  - `atx::rpc::SocketStream : public ByteStream` — wraps a `Socket`.
  - `bool atx::rpc::is_loopback(std::string_view host)`.

- [ ] **Step 1: Write the failing test**

`atx-rpc/tests/socket_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/rpc/frame.hpp"
#include "atx/rpc/socket.hpp"

namespace {

using atx::core::ErrorCode;
using atx::rpc::FrameLimits;
using atx::rpc::is_loopback;
using atx::rpc::Listener;
using atx::rpc::read_frame;
using atx::rpc::Socket;
using atx::rpc::SocketStream;
using atx::rpc::write_frame;
using atx::rpc::WsaScope;

std::vector<std::byte> bytes_of(std::initializer_list<std::uint8_t> vals) {
  std::vector<std::byte> out;
  out.reserve(vals.size());
  for (const std::uint8_t v : vals) {
    out.push_back(static_cast<std::byte>(v));
  }
  return out;
}

// Every test binds :0 and asks the listener which port it got. A hard-coded
// port is the single most common source of flake in a socket suite.
TEST(Listener, EphemeralBindReportsItsAssignedPort) {
  const WsaScope wsa;
  auto listener = Listener::bind("127.0.0.1", 0, 16);
  ASSERT_TRUE(listener) << listener.error().to_string();
  EXPECT_NE(listener->port(), 0u);
}

TEST(Socket, ConnectAcceptAndFrameRoundTrip) {
  const WsaScope wsa;
  auto listener = Listener::bind("127.0.0.1", 0, 16);
  ASSERT_TRUE(listener);
  const std::uint16_t port = listener->port();

  const std::vector<std::byte> payload = bytes_of({0x11, 0x22, 0x33});

  std::thread server([&listener, &payload]() {
    auto conn = listener->accept();
    ASSERT_TRUE(conn);
    SocketStream stream{std::move(*conn)};
    std::vector<std::byte> got;
    ASSERT_TRUE(read_frame(stream, got, FrameLimits{}));
    EXPECT_EQ(got, payload);
    ASSERT_TRUE(write_frame(stream, got, FrameLimits{})); // echo
  });

  auto client = Socket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
  ASSERT_TRUE(client) << client.error().to_string();
  SocketStream stream{std::move(*client)};
  ASSERT_TRUE(write_frame(stream, payload, FrameLimits{}));

  std::vector<std::byte> echoed;
  ASSERT_TRUE(read_frame(stream, echoed, FrameLimits{}));
  EXPECT_EQ(echoed, payload);

  server.join();
}

TEST(Socket, ConnectToAClosedPortFails) {
  const WsaScope wsa;
  std::uint16_t port = 0;
  {
    auto listener = Listener::bind("127.0.0.1", 0, 16);
    ASSERT_TRUE(listener);
    port = listener->port();
  } // listener destroyed; the port is now refusing

  auto client = Socket::connect("127.0.0.1", port, std::chrono::milliseconds{500});
  EXPECT_FALSE(client);
}

// A peer that connects and then says nothing must not pin a reader forever.
// This is the slowloris bound the server runtime relies on in Task 6.
TEST(Socket, RecvTimeoutFiresOnASilentPeer) {
  const WsaScope wsa;
  auto listener = Listener::bind("127.0.0.1", 0, 16);
  ASSERT_TRUE(listener);
  const std::uint16_t port = listener->port();

  std::thread silent([port]() {
    auto client = Socket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
    ASSERT_TRUE(client);
    // Hold the connection open, send nothing.
    std::this_thread::sleep_for(std::chrono::milliseconds{600});
  });

  auto conn = listener->accept();
  ASSERT_TRUE(conn);
  ASSERT_TRUE(conn->set_timeouts(std::chrono::milliseconds{150}, std::chrono::milliseconds{150}));

  SocketStream stream{std::move(*conn)};
  std::vector<std::byte> got;
  const auto start = std::chrono::steady_clock::now();
  const auto status = read_frame(stream, got, FrameLimits{});
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::Unavailable);
  EXPECT_LT(elapsed, std::chrono::milliseconds{1500});

  silent.join();
}

TEST(Socket, MovedFromHandleIsInvalidAndDoesNotDoubleClose) {
  const WsaScope wsa;
  auto listener = Listener::bind("127.0.0.1", 0, 16);
  ASSERT_TRUE(listener);
  auto a = Socket::connect("127.0.0.1", listener->port(), std::chrono::milliseconds{2000});
  ASSERT_TRUE(a);
  Socket b{std::move(*a)};
  EXPECT_FALSE(a->valid());
  EXPECT_TRUE(b.valid());
}

TEST(IsLoopback, AcceptsLoopbackFormsAndRejectsEverythingElse) {
  EXPECT_TRUE(is_loopback("127.0.0.1"));
  EXPECT_TRUE(is_loopback("127.0.0.5"));
  EXPECT_TRUE(is_loopback("localhost"));
  EXPECT_TRUE(is_loopback("::1"));

  EXPECT_FALSE(is_loopback("0.0.0.0"));
  EXPECT_FALSE(is_loopback("192.168.1.10"));
  EXPECT_FALSE(is_loopback("10.0.0.1"));
  EXPECT_FALSE(is_loopback("::"));
  EXPECT_FALSE(is_loopback(""));
  // 127 in a later octet position is not loopback.
  EXPECT_FALSE(is_loopback("10.127.0.1"));
}

} // namespace
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset server --target atx-rpc-tests`
Expected: FAIL at compile — `atx/rpc/socket.hpp: No such file or directory`.

- [ ] **Step 3: Write `atx-rpc/include/atx/rpc/socket.hpp`**

```cpp
#pragma once

// atx::rpc socket shim.
//
// This is NOT an abstraction over I/O models. It exists so that `#ifdef _WIN32`
// appears in exactly one translation unit (socket.cpp) instead of being sprayed
// through the server runtime. The build targets Windows; the BSD path is kept
// compiling so a future Linux port is a build change rather than a rewrite, but
// it is not exercised by CI (there is none).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/rpc/byte_stream.hpp"

namespace atx::rpc {

#if defined(_WIN32)
using NativeHandle = std::uintptr_t; // SOCKET
inline constexpr NativeHandle kInvalidHandle = static_cast<NativeHandle>(~0ull);
#else
using NativeHandle = int;
inline constexpr NativeHandle kInvalidHandle = -1;
#endif

// Refcounted WSAStartup/WSACleanup. Construct one in main() and one in each
// test that touches sockets; nesting is safe. A no-op off Windows.
class WsaScope {
public:
  WsaScope();
  ~WsaScope();
  WsaScope(const WsaScope &) = delete;
  WsaScope &operator=(const WsaScope &) = delete;
};

// True for 127.0.0.0/8, ::1, and the literal "localhost". Everything else is
// false, including "0.0.0.0" and "::" -- a wildcard bind is not loopback, and
// treating it as one would defeat the startup gate it guards.
[[nodiscard]] bool is_loopback(std::string_view host);

class Socket {
public:
  Socket() = default;
  explicit Socket(NativeHandle handle) noexcept : handle_{handle} {}
  ~Socket();

  Socket(Socket &&other) noexcept;
  Socket &operator=(Socket &&other) noexcept;
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;

  [[nodiscard]] static atx::core::Result<Socket> connect(std::string_view host,
                                                         std::uint16_t port,
                                                         std::chrono::milliseconds timeout);

  // Bounds every blocking recv/send. This is what stops a peer that connects
  // and then stalls from pinning a worker forever (see server.hpp).
  [[nodiscard]] atx::core::Status set_timeouts(std::chrono::milliseconds recv,
                                               std::chrono::milliseconds send);

  [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidHandle; }
  [[nodiscard]] NativeHandle native() const noexcept { return handle_; }
  void close() noexcept;

private:
  NativeHandle handle_{kInvalidHandle};
};

class Listener {
public:
  Listener() = default;
  ~Listener();

  Listener(Listener &&other) noexcept;
  Listener &operator=(Listener &&other) noexcept;
  Listener(const Listener &) = delete;
  Listener &operator=(const Listener &) = delete;

  // Pass port 0 for an ephemeral port, then read it back with port(). Every
  // test does this: a hard-coded port is the commonest source of socket-suite
  // flake.
  [[nodiscard]] static atx::core::Result<Listener> bind(std::string_view host,
                                                        std::uint16_t port, int backlog);

  [[nodiscard]] atx::core::Result<Socket> accept();
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidHandle; }
  [[nodiscard]] NativeHandle native() const noexcept { return handle_; }
  void close() noexcept;

private:
  NativeHandle handle_{kInvalidHandle};
  std::uint16_t port_{0};
};

// Adapts a Socket to the ByteStream interface the framing layer speaks.
class SocketStream final : public ByteStream {
public:
  explicit SocketStream(Socket socket) noexcept : socket_{std::move(socket)} {}

  [[nodiscard]] atx::core::Result<std::size_t> read_some(std::span<std::byte> out) override;
  [[nodiscard]] atx::core::Status write_all(std::span<const std::byte> in) override;

  [[nodiscard]] Socket &socket() noexcept { return socket_; }

private:
  Socket socket_;
};

} // namespace atx::rpc
```

- [ ] **Step 4: Write `atx-rpc/src/socket.cpp`**

```cpp
#include "atx/rpc/socket.hpp"

#include <atomic>
#include <cstring>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace atx::rpc {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

std::atomic<int> g_wsa_refs{0};

[[nodiscard]] int last_error() noexcept {
#if defined(_WIN32)
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

[[nodiscard]] bool would_block(int err) noexcept {
#if defined(_WIN32)
  return err == WSAEWOULDBLOCK || err == WSAETIMEDOUT;
#else
  return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

void close_handle(NativeHandle h) noexcept {
  if (h == kInvalidHandle) {
    return;
  }
#if defined(_WIN32)
  ::closesocket(static_cast<SOCKET>(h));
#else
  ::close(h);
#endif
}

[[nodiscard]] std::string errno_text(std::string_view what, int err) {
  return std::string{what} + " failed: os error " + std::to_string(err);
}

// Resolves host:port to the first usable TCP address. AI_PASSIVE is not set;
// callers pass an explicit host, including for the loopback bind.
[[nodiscard]] Result<addrinfo *> resolve(std::string_view host, std::uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  const std::string host_str{host};
  const std::string port_str = std::to_string(port);
  addrinfo *result = nullptr;
  const int rc = ::getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &result);
  if (rc != 0 || result == nullptr) {
    return Err(ErrorCode::InvalidArgument,
               "cannot resolve " + host_str + ":" + port_str + " (getaddrinfo " +
                   std::to_string(rc) + ")");
  }
  return Ok(result);
}

// Reads the port actually assigned, so an ephemeral :0 bind can report it.
[[nodiscard]] Result<std::uint16_t> local_port(NativeHandle h) {
  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(static_cast<
#if defined(_WIN32)
                        SOCKET
#else
                        int
#endif
                        >(h),
                    reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    return Err(ErrorCode::IoError, errno_text("getsockname", last_error()));
  }
  if (addr.ss_family == AF_INET) {
    return Ok(static_cast<std::uint16_t>(
        ntohs(reinterpret_cast<const sockaddr_in *>(&addr)->sin_port)));
  }
  if (addr.ss_family == AF_INET6) {
    return Ok(static_cast<std::uint16_t>(
        ntohs(reinterpret_cast<const sockaddr_in6 *>(&addr)->sin6_port)));
  }
  return Err(ErrorCode::Internal, "socket bound to an unexpected address family");
}

} // namespace

WsaScope::WsaScope() {
#if defined(_WIN32)
  if (g_wsa_refs.fetch_add(1) == 0) {
    WSADATA data{};
    // A failure here means no networking at all; there is no useful recovery
    // and the constructor cannot return a Result, so the refcount is rolled
    // back and every later socket call fails with its own typed error.
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      g_wsa_refs.fetch_sub(1);
    }
  }
#else
  g_wsa_refs.fetch_add(1);
#endif
}

WsaScope::~WsaScope() {
#if defined(_WIN32)
  if (g_wsa_refs.fetch_sub(1) == 1) {
    ::WSACleanup();
  }
#else
  g_wsa_refs.fetch_sub(1);
#endif
}

bool is_loopback(std::string_view host) {
  if (host.empty()) {
    return false;
  }
  if (host == "localhost" || host == "::1" || host == "[::1]") {
    return true;
  }
  // 127.0.0.0/8. Parsed rather than prefix-matched so "10.127.0.1" is rejected
  // and "127.0.0.1.evil.com" cannot slip through.
  in_addr v4{};
  const std::string host_str{host};
  if (::inet_pton(AF_INET, host_str.c_str(), &v4) == 1) {
    const std::uint32_t bits = ntohl(v4.s_addr);
    return (bits >> 24) == 127u;
  }
  in6_addr v6{};
  if (::inet_pton(AF_INET6, host_str.c_str(), &v6) == 1) {
    static const in6_addr loop = IN6ADDR_LOOPBACK_INIT;
    return std::memcmp(&v6, &loop, sizeof(v6)) == 0;
  }
  return false;
}

Socket::~Socket() { close(); }

Socket::Socket(Socket &&other) noexcept : handle_{other.handle_} {
  other.handle_ = kInvalidHandle;
}

Socket &Socket::operator=(Socket &&other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalidHandle;
  }
  return *this;
}

void Socket::close() noexcept {
  close_handle(handle_);
  handle_ = kInvalidHandle;
}

Result<Socket> Socket::connect(std::string_view host, std::uint16_t port,
                               std::chrono::milliseconds timeout) {
  ATX_TRY(addrinfo * addrs, resolve(host, port));
  NativeHandle handle = kInvalidHandle;
  int saved = 0;
  for (const addrinfo *it = addrs; it != nullptr; it = it->ai_next) {
    const auto raw = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (raw == static_cast<decltype(raw)>(kInvalidHandle)) {
      saved = last_error();
      continue;
    }
    if (::connect(raw, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen)) == 0) {
      handle = static_cast<NativeHandle>(raw);
      break;
    }
    saved = last_error();
    close_handle(static_cast<NativeHandle>(raw));
  }
  ::freeaddrinfo(addrs);

  if (handle == kInvalidHandle) {
    return Err(ErrorCode::Unavailable,
               "connect to " + std::string{host} + ":" + std::to_string(port) +
                   " failed: os error " + std::to_string(saved));
  }

  Socket socket{handle};
  // TCP_NODELAY: every request is a single small frame followed by a blocking
  // wait for the response, so Nagle would add up to 40ms of pure latency to a
  // request that takes microseconds to serve.
  int one = 1;
  ::setsockopt(static_cast<
#if defined(_WIN32)
                   SOCKET
#else
                   int
#endif
                   >(handle),
               IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof(one));
  ATX_TRY_VOID(socket.set_timeouts(timeout, timeout));
  return Ok(std::move(socket));
}

Status Socket::set_timeouts(std::chrono::milliseconds recv, std::chrono::milliseconds send) {
  if (!valid()) {
    return Err(ErrorCode::InvalidArgument, "set_timeouts on a closed socket");
  }
#if defined(_WIN32)
  const DWORD r = static_cast<DWORD>(recv.count());
  const DWORD s = static_cast<DWORD>(send.count());
  const auto sock = static_cast<SOCKET>(handle_);
  if (::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&r),
                   sizeof(r)) != 0 ||
      ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&s),
                   sizeof(s)) != 0) {
    return Err(ErrorCode::IoError, errno_text("setsockopt(SO_*TIMEO)", last_error()));
  }
#else
  timeval r{};
  r.tv_sec = static_cast<time_t>(recv.count() / 1000);
  r.tv_usec = static_cast<suseconds_t>((recv.count() % 1000) * 1000);
  timeval s{};
  s.tv_sec = static_cast<time_t>(send.count() / 1000);
  s.tv_usec = static_cast<suseconds_t>((send.count() % 1000) * 1000);
  if (::setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof(r)) != 0 ||
      ::setsockopt(handle_, SOL_SOCKET, SO_SNDTIMEO, &s, sizeof(s)) != 0) {
    return Err(ErrorCode::IoError, errno_text("setsockopt(SO_*TIMEO)", last_error()));
  }
#endif
  return Ok();
}

Listener::~Listener() { close(); }

Listener::Listener(Listener &&other) noexcept : handle_{other.handle_}, port_{other.port_} {
  other.handle_ = kInvalidHandle;
  other.port_ = 0;
}

Listener &Listener::operator=(Listener &&other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    port_ = other.port_;
    other.handle_ = kInvalidHandle;
    other.port_ = 0;
  }
  return *this;
}

void Listener::close() noexcept {
  close_handle(handle_);
  handle_ = kInvalidHandle;
  port_ = 0;
}

Result<Listener> Listener::bind(std::string_view host, std::uint16_t port, int backlog) {
  ATX_TRY(addrinfo * addrs, resolve(host, port));
  NativeHandle handle = kInvalidHandle;
  int saved = 0;
  for (const addrinfo *it = addrs; it != nullptr; it = it->ai_next) {
    const auto raw = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (raw == static_cast<decltype(raw)>(kInvalidHandle)) {
      saved = last_error();
      continue;
    }
    // SO_REUSEADDR is deliberately NOT set on Windows: there it permits two
    // processes to bind the same port simultaneously, which would let a second
    // atx-server silently steal traffic from a running one.
    if (::bind(raw, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen)) == 0 &&
        ::listen(raw, backlog) == 0) {
      handle = static_cast<NativeHandle>(raw);
      break;
    }
    saved = last_error();
    close_handle(static_cast<NativeHandle>(raw));
  }
  ::freeaddrinfo(addrs);

  if (handle == kInvalidHandle) {
    return Err(ErrorCode::Unavailable, "bind " + std::string{host} + ":" +
                                           std::to_string(port) + " failed: os error " +
                                           std::to_string(saved));
  }

  Listener listener;
  listener.handle_ = handle;
  ATX_TRY(const std::uint16_t assigned, local_port(handle));
  listener.port_ = assigned;
  return Ok(std::move(listener));
}

Result<Socket> Listener::accept() {
  if (!valid()) {
    return Err(ErrorCode::InvalidArgument, "accept on a closed listener");
  }
  const auto raw = ::accept(static_cast<
#if defined(_WIN32)
                                SOCKET
#else
                                int
#endif
                                >(handle_),
                            nullptr, nullptr);
  if (raw == static_cast<decltype(raw)>(kInvalidHandle)) {
    return Err(ErrorCode::Unavailable, errno_text("accept", last_error()));
  }
  Socket socket{static_cast<NativeHandle>(raw)};
  int one = 1;
  ::setsockopt(raw, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one),
               sizeof(one));
  return Ok(std::move(socket));
}

Result<std::size_t> SocketStream::read_some(std::span<std::byte> out) {
  if (!socket_.valid()) {
    return Err(ErrorCode::Unavailable, "read on a closed socket");
  }
  if (out.empty()) {
    return Ok(std::size_t{0});
  }
  const auto n = ::recv(static_cast<
#if defined(_WIN32)
                            SOCKET
#else
                            int
#endif
                            >(socket_.native()),
                        reinterpret_cast<char *>(out.data()),
                        static_cast<int>(out.size()), 0);
  if (n < 0) {
    const int err = last_error();
    // A recv timeout is reported as Unavailable, matching a peer that stopped:
    // both mean "close this connection", and the framing layer already treats
    // Unavailable as ordinary disconnection rather than an alarm.
    if (would_block(err)) {
      return Err(ErrorCode::Unavailable, "recv timed out");
    }
    return Err(ErrorCode::Unavailable, errno_text("recv", err));
  }
  return Ok(static_cast<std::size_t>(n)); // 0 == clean close
}

Status SocketStream::write_all(std::span<const std::byte> in) {
  if (!socket_.valid()) {
    return Err(ErrorCode::Unavailable, "write on a closed socket");
  }
  std::size_t sent = 0;
  while (sent < in.size()) {
    const auto n = ::send(static_cast<
#if defined(_WIN32)
                              SOCKET
#else
                              int
#endif
                              >(socket_.native()),
                          reinterpret_cast<const char *>(in.data() + sent),
                          static_cast<int>(in.size() - sent), 0);
    if (n <= 0) {
      return Err(ErrorCode::Unavailable, errno_text("send", last_error()));
    }
    sent += static_cast<std::size_t>(n);
  }
  return Ok();
}

} // namespace atx::rpc
```

- [ ] **Step 5: Add `src/socket.cpp` to `atx-rpc/CMakeLists.txt`**

```cmake
add_library(atx-rpc ${ATX_LIB_TYPE}
    src/limits.cpp
    src/byte_stream.cpp
    src/frame.cpp
    src/socket.cpp
)
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset server --target atx-rpc-tests`
Then: `ctest --preset server -R "^Listener\.|^Socket\.|^IsLoopback\." --output-on-failure`
Expected: 6 tests, all PASS.

- [ ] **Step 7: Run the whole rpc suite to check for regressions**

Run: `ctest --preset server -L atx_server --output-on-failure`
Expected: all PASS (frame + envelope + socket).

- [ ] **Step 8: Commit**

```bash
git add atx-rpc/include/atx/rpc/socket.hpp atx-rpc/src/socket.cpp \
        atx-rpc/tests/socket_test.cpp atx-rpc/CMakeLists.txt
git commit -m "feat(rpc): add the socket shim and SocketStream

Not an abstraction over I/O models -- it exists so #ifdef _WIN32 appears in one
translation unit instead of being sprayed through the server runtime. The BSD
path is kept compiling so a future Linux port is a build change rather than a
rewrite, but there is no CI and it is not exercised.

Listener::bind takes port 0 and reports the assigned port, and every test uses
that. A hard-coded port is the commonest source of flake in a socket suite.

SO_REUSEADDR is deliberately not set. On Windows it permits two processes to
bind the same port at once, which would let a second atx-server silently steal
traffic from a running one.

TCP_NODELAY is set on both ends. Each request is one small frame followed by a
blocking wait for its response, which is precisely the pattern Nagle penalises;
without it a microsecond-scale query can pay tens of milliseconds.

set_timeouts is the slowloris bound the server runtime depends on in the next
tasks: a peer that connects and then stalls cannot pin a worker forever. A recv
timeout surfaces as Unavailable, the same code as a closed peer, because both
mean close the connection and the framing layer already treats Unavailable as
ordinary disconnection.

is_loopback parses rather than prefix-matches, so 10.127.0.1 is rejected and
0.0.0.0 is not mistaken for loopback. The startup gate in atx-server depends on
that distinction holding.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: `atx-rpc` method table and dispatcher

The modularity claim, made concrete: adding a service later is one class plus one registration line, with no change to transport, framing, or dispatch.

**Files:**
- Create: `atx-rpc/include/atx/rpc/call_context.hpp`
- Create: `atx-rpc/include/atx/rpc/method_table.hpp`, `atx-rpc/src/method_table.cpp`
- Create: `atx-rpc/include/atx/rpc/service.hpp`
- Create: `atx-rpc/include/atx/rpc/dispatcher.hpp`, `atx-rpc/src/dispatcher.cpp`
- Create: `atx-rpc/tests/dispatcher_test.cpp`
- Modify: `atx-rpc/CMakeLists.txt`

**Interfaces:**
- Consumes: `atx::rpc::v1::Envelope`, `RpcStatus`, `RpcCode` (Task 2).
- Produces:
  - `struct atx::rpc::Entitlements { bool authenticated; std::vector<std::string> readable_db_ids; std::string token_label; bool can_read(std::string_view db_id) const; }`
  - `struct atx::rpc::CallContext { Entitlements entitlements; std::int64_t deadline_unix_ns; std::string peer; std::string incident_id; bool deadline_passed(std::int64_t now_ns) const; }`
  - `class atx::rpc::MethodTable` with `template <class Req, class Resp> void add(std::string_view method, std::function<v1::RpcStatus(const CallContext&, const Req&, Resp&)> fn)`, `bool contains(std::string_view) const`, `std::vector<std::string> method_names() const`, `v1::RpcStatus invoke(std::string_view method, const CallContext&, const std::string& request_payload, std::string& response_payload) const`.
  - `class atx::rpc::Service` with `virtual std::string_view name() const noexcept` and `virtual void register_methods(MethodTable&)`.
  - `class atx::rpc::Dispatcher` with `explicit Dispatcher(MethodTable)`, `void set_authenticator(std::function<atx::core::Result<Entitlements>(std::string_view token)>)`, `v1::Envelope dispatch(const v1::Envelope& request, std::string_view peer) const`.
  - Free helpers: `v1::RpcStatus atx::rpc::ok_status()`, `v1::RpcStatus atx::rpc::make_status(v1::RpcCode, std::string message)`.

- [ ] **Step 1: Write the failing test**

`atx-rpc/tests/dispatcher_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "atx/core/error.hpp"
#include "atx/rpc/dispatcher.hpp"
#include "atx/rpc/method_table.hpp"
#include "atx/rpc/service.hpp"
#include "atx/rpc/v1/envelope.pb.h"

namespace {

using atx::rpc::CallContext;
using atx::rpc::Dispatcher;
using atx::rpc::Entitlements;
using atx::rpc::make_status;
using atx::rpc::MethodTable;
using atx::rpc::ok_status;
using atx::rpc::Service;
namespace v1 = atx::rpc::v1;

// Hello/HelloAck double as the test's request/response pair so this task needs
// no additional .proto. Their field shapes are irrelevant here; only the
// decode -> handler -> encode round trip is under test.
class EchoService final : public Service {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "atx.rpc.v1.Echo"; }

  void register_methods(MethodTable &table) override {
    table.add<v1::Hello, v1::HelloAck>(
        "atx.rpc.v1.Echo/Ping",
        [](const CallContext &, const v1::Hello &req, v1::HelloAck &resp) {
          resp.set_server_build(req.client_build());
          resp.set_protocol_version(req.protocol_version());
          return ok_status();
        });

    table.add<v1::Hello, v1::HelloAck>(
        "atx.rpc.v1.Echo/Boom",
        [](const CallContext &, const v1::Hello &, v1::HelloAck &) {
          return make_status(v1::RPC_CODE_NOT_FOUND, "nothing here");
        });

    table.add<v1::Hello, v1::HelloAck>(
        "atx.rpc.v1.Echo/Throws",
        [](const CallContext &, const v1::Hello &, v1::HelloAck &) -> v1::RpcStatus {
          throw std::runtime_error("handler blew up with a secret: hunter2");
        });

    table.add<v1::Hello, v1::HelloAck>(
        "atx.rpc.v1.Echo/WhoAmI",
        [](const CallContext &ctx, const v1::Hello &, v1::HelloAck &resp) {
          resp.set_server_build(ctx.entitlements.token_label);
          return ok_status();
        });
  }
};

MethodTable build_table() {
  MethodTable table;
  EchoService service;
  service.register_methods(table);
  return table;
}

Dispatcher build_dispatcher() {
  Dispatcher dispatcher{build_table()};
  dispatcher.set_authenticator(
      [](std::string_view token) -> atx::core::Result<Entitlements> {
        if (token == "good") {
          Entitlements ent;
          ent.authenticated = true;
          ent.token_label = "desk";
          ent.readable_db_ids = {"spy_prod"};
          return atx::core::Ok(ent);
        }
        return atx::core::Err(atx::core::ErrorCode::PermissionDenied, "unknown token");
      });
  return dispatcher;
}

v1::Envelope make_request(std::string method, std::string token) {
  v1::Hello hello;
  hello.set_client_build("test-client");
  hello.set_protocol_version(1);

  v1::Envelope env;
  env.set_protocol_version(1);
  env.set_correlation_id(77);
  env.set_is_response(false);
  env.set_method(std::move(method));
  env.set_auth_token(std::move(token));
  env.set_payload(hello.SerializeAsString());
  return env;
}

TEST(MethodTable, ReportsRegisteredNames) {
  const MethodTable table = build_table();
  EXPECT_TRUE(table.contains("atx.rpc.v1.Echo/Ping"));
  EXPECT_FALSE(table.contains("atx.rpc.v1.Echo/Missing"));
  EXPECT_EQ(table.method_names().size(), 4u);
}

TEST(Dispatcher, RoundTripsASuccessfulCall) {
  const Dispatcher dispatcher = build_dispatcher();
  const v1::Envelope response =
      dispatcher.dispatch(make_request("atx.rpc.v1.Echo/Ping", "good"), "test-peer");

  EXPECT_TRUE(response.is_response());
  EXPECT_EQ(response.correlation_id(), 77u);
  EXPECT_EQ(response.status().code(), v1::RPC_CODE_OK);

  v1::HelloAck ack;
  ASSERT_TRUE(ack.ParseFromString(response.payload()));
  EXPECT_EQ(ack.server_build(), "test-client");
}

TEST(Dispatcher, PropagatesAHandlerStatus) {
  const Dispatcher dispatcher = build_dispatcher();
  const v1::Envelope response =
      dispatcher.dispatch(make_request("atx.rpc.v1.Echo/Boom", "good"), "test-peer");
  EXPECT_EQ(response.status().code(), v1::RPC_CODE_NOT_FOUND);
  EXPECT_EQ(response.status().message(), "nothing here");
  EXPECT_TRUE(response.payload().empty());
}

TEST(Dispatcher, UnknownMethodIsUnimplemented) {
  const Dispatcher dispatcher = build_dispatcher();
  const v1::Envelope response =
      dispatcher.dispatch(make_request("atx.rpc.v1.Echo/Nope", "good"), "test-peer");
  EXPECT_EQ(response.status().code(), v1::RPC_CODE_UNIMPLEMENTED);
  EXPECT_EQ(response.correlation_id(), 77u);
}

TEST(Dispatcher, BadTokenIsUnauthenticatedAndNeverReachesTheHandler) {
  const Dispatcher dispatcher = build_dispatcher();
  const v1::Envelope response =
      dispatcher.dispatch(make_request("atx.rpc.v1.Echo/Ping", "bad"), "test-peer");
  EXPECT_EQ(response.status().code(), v1::RPC_CODE_UNAUTHENTICATED);
  EXPECT_TRUE(response.payload().empty());
}

TEST(Dispatcher, EntitlementsReachTheHandler) {
  const Dispatcher dispatcher = build_dispatcher();
  const v1::Envelope response =
      dispatcher.dispatch(make_request("atx.rpc.v1.Echo/WhoAmI", "good"), "test-peer");
  ASSERT_EQ(response.status().code(), v1::RPC_CODE_OK);
  v1::HelloAck ack;
  ASSERT_TRUE(ack.ParseFromString(response.payload()));
  EXPECT_EQ(ack.server_build(), "desk");
}

TEST(Dispatcher, UndecodablePayloadIsInvalidArgument) {
  const Dispatcher dispatcher = build_dispatcher();
  v1::Envelope request = make_request("atx.rpc.v1.Echo/Ping", "good");
  request.set_payload("\xFF\xFF\xFF\xFF\xFF\xFF", 6); // not a valid Hello
  const v1::Envelope response = dispatcher.dispatch(request, "test-peer");
  EXPECT_EQ(response.status().code(), v1::RPC_CODE_INVALID_ARGUMENT);
}

// A thrown exception must not escape into the transport, and its text must not
// reach the client: the message could carry a path, a query, or a credential.
TEST(Dispatcher, ThrownExceptionBecomesInternalWithoutLeakingItsText) {
  const Dispatcher dispatcher = build_dispatcher();
  const v1::Envelope response =
      dispatcher.dispatch(make_request("atx.rpc.v1.Echo/Throws", "good"), "test-peer");
  EXPECT_EQ(response.status().code(), v1::RPC_CODE_INTERNAL);
  EXPECT_FALSE(response.status().incident_id().empty());
  EXPECT_EQ(response.status().message().find("hunter2"), std::string::npos);
  EXPECT_EQ(response.status().message().find("blew up"), std::string::npos);
}

TEST(Dispatcher, IncidentIdsAreDistinctPerFailure) {
  const Dispatcher dispatcher = build_dispatcher();
  const v1::Envelope a =
      dispatcher.dispatch(make_request("atx.rpc.v1.Echo/Throws", "good"), "p");
  const v1::Envelope b =
      dispatcher.dispatch(make_request("atx.rpc.v1.Echo/Throws", "good"), "p");
  EXPECT_NE(a.status().incident_id(), b.status().incident_id());
}

TEST(Dispatcher, ExpiredDeadlineIsRejectedBeforeDispatch) {
  const Dispatcher dispatcher = build_dispatcher();
  v1::Envelope request = make_request("atx.rpc.v1.Echo/Ping", "good");
  request.set_deadline_unix_ns(1); // 1 ns after the epoch: long gone
  const v1::Envelope response = dispatcher.dispatch(request, "test-peer");
  EXPECT_EQ(response.status().code(), v1::RPC_CODE_DEADLINE_EXCEEDED);
}

TEST(Entitlements, CanReadOnlyGrantedDatabases) {
  Entitlements ent;
  ent.authenticated = true;
  ent.readable_db_ids = {"spy_prod", "sp100_dev"};
  EXPECT_TRUE(ent.can_read("spy_prod"));
  EXPECT_TRUE(ent.can_read("sp100_dev"));
  EXPECT_FALSE(ent.can_read("secret_db"));
  EXPECT_FALSE(ent.can_read(""));
}

} // namespace
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset server --target atx-rpc-tests`
Expected: FAIL at compile — `atx/rpc/dispatcher.hpp: No such file or directory`.

- [ ] **Step 3: Write `atx-rpc/include/atx/rpc/call_context.hpp`**

```cpp
#pragma once

// What a handler is allowed to know about its caller. Deliberately small: a
// handler sees identity and deadline, never a socket, an envelope, or bytes.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atx::rpc {

struct Entitlements {
  bool authenticated{false};
  // The db_ids this token may read. An id absent here is PERMISSION_DENIED, not
  // NOT_FOUND: realm ids are not secrets, and masking them makes a
  // misconfiguration undebuggable.
  std::vector<std::string> readable_db_ids;
  std::string token_label;

  [[nodiscard]] bool can_read(std::string_view db_id) const {
    if (db_id.empty()) {
      return false;
    }
    return std::find(readable_db_ids.begin(), readable_db_ids.end(), db_id) !=
           readable_db_ids.end();
  }
};

struct CallContext {
  Entitlements entitlements;
  // 0 means no deadline. Checked at dispatch and at cooperative checkpoints
  // inside long handlers; a handler already inside a decode is NOT preempted.
  std::int64_t deadline_unix_ns{0};
  std::string peer;
  // Filled only when a handler fails unexpectedly, so the server log and the
  // client's RpcStatus can be correlated without putting exception text on the
  // wire.
  std::string incident_id;

  [[nodiscard]] bool deadline_passed(std::int64_t now_unix_ns) const noexcept {
    return deadline_unix_ns != 0 && now_unix_ns >= deadline_unix_ns;
  }
};

} // namespace atx::rpc
```

- [ ] **Step 4: Write `atx-rpc/include/atx/rpc/method_table.hpp`**

```cpp
#pragma once

// MethodTable — typed handler registration.
//
// The table owns decode and encode, so a handler signature is
//   RpcStatus(const CallContext&, const Req&, Resp&)
// and a handler never sees bytes, an envelope, or a socket. That is what makes
// "adding a service is one class plus one registration line" true rather than
// aspirational: a new service touches no transport code at all.

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "atx/rpc/call_context.hpp"
#include "atx/rpc/v1/envelope.pb.h"

namespace atx::rpc {

[[nodiscard]] v1::RpcStatus ok_status();
[[nodiscard]] v1::RpcStatus make_status(v1::RpcCode code, std::string message);

class MethodTable {
public:
  // Erased form: payload in, payload out. Only add<Req,Resp> constructs these.
  using Handler =
      std::function<v1::RpcStatus(const CallContext &, const std::string &, std::string &)>;

  template <class Req, class Resp>
  void add(std::string_view method,
           std::function<v1::RpcStatus(const CallContext &, const Req &, Resp &)> fn) {
    handlers_.emplace(
        std::string{method},
        [fn = std::move(fn)](const CallContext &ctx, const std::string &in,
                             std::string &out) -> v1::RpcStatus {
          Req request;
          if (!request.ParseFromString(in)) {
            return make_status(v1::RPC_CODE_INVALID_ARGUMENT,
                               "request payload is not a valid " +
                                   std::string{Req::descriptor()->full_name()});
          }
          Resp response;
          v1::RpcStatus status = fn(ctx, request, response);
          if (status.code() == v1::RPC_CODE_OK && !response.SerializeToString(&out)) {
            return make_status(v1::RPC_CODE_INTERNAL, "response serialization failed");
          }
          if (status.code() != v1::RPC_CODE_OK) {
            out.clear();
          }
          return status;
        });
  }

  [[nodiscard]] bool contains(std::string_view method) const;
  [[nodiscard]] std::vector<std::string> method_names() const;

  // Returns UNIMPLEMENTED if `method` is not registered. Does not catch
  // exceptions; Dispatcher owns that boundary.
  [[nodiscard]] v1::RpcStatus invoke(std::string_view method, const CallContext &ctx,
                                     const std::string &request_payload,
                                     std::string &response_payload) const;

private:
  std::unordered_map<std::string, Handler, std::hash<std::string>, std::equal_to<>> handlers_;
};

} // namespace atx::rpc
```

- [ ] **Step 5: Write `atx-rpc/include/atx/rpc/service.hpp`**

```cpp
#pragma once

#include <string_view>

#include "atx/rpc/method_table.hpp"

namespace atx::rpc {

// A service is a named bundle of methods. Implementations live in atx-server
// (AdminServiceImpl, SurfaceServiceImpl); a later BacktestServiceImpl is one
// more class and one more registration line, with no transport change.
class Service {
public:
  virtual ~Service() = default;
  Service() = default;
  Service(const Service &) = delete;
  Service &operator=(const Service &) = delete;
  Service(Service &&) = delete;
  Service &operator=(Service &&) = delete;

  // Fully-qualified, e.g. "atx.rpc.v1.SurfaceService". Method strings on the
  // wire are "<name>/<Method>".
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  virtual void register_methods(MethodTable &table) = 0;
};

} // namespace atx::rpc
```

- [ ] **Step 6: Write `atx-rpc/include/atx/rpc/dispatcher.hpp`**

```cpp
#pragma once

// Dispatcher — Envelope in, Envelope out. No socket, no framing, no threads.
//
// Keeping this pure means the entire request path (auth, deadline, decode,
// handler, encode, error mapping) is unit-testable without binding a port.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/rpc/call_context.hpp"
#include "atx/rpc/method_table.hpp"
#include "atx/rpc/v1/envelope.pb.h"

namespace atx::rpc {

class Dispatcher {
public:
  using Authenticator = std::function<atx::core::Result<Entitlements>(std::string_view token)>;

  explicit Dispatcher(MethodTable table);

  Dispatcher(Dispatcher &&) noexcept;
  Dispatcher &operator=(Dispatcher &&) = delete;
  Dispatcher(const Dispatcher &) = delete;
  Dispatcher &operator=(const Dispatcher &) = delete;
  ~Dispatcher() = default;

  // Without an authenticator every call is UNAUTHENTICATED. Fail-closed is the
  // only safe default for a component whose whole job is gating access.
  void set_authenticator(Authenticator authenticator);

  // Never throws. A handler that throws becomes RPC_CODE_INTERNAL with a fresh
  // incident id; the exception text goes to the server log and never to the
  // wire, because it can contain paths, queries, or credentials.
  [[nodiscard]] v1::Envelope dispatch(const v1::Envelope &request, std::string_view peer) const;

  [[nodiscard]] const MethodTable &methods() const noexcept { return table_; }

private:
  [[nodiscard]] std::string next_incident_id() const;

  MethodTable table_;
  Authenticator authenticator_;
  mutable std::atomic<std::uint64_t> incident_counter_{0};
};

} // namespace atx::rpc
```

- [ ] **Step 7: Write `atx-rpc/src/method_table.cpp`**

```cpp
#include "atx/rpc/method_table.hpp"

#include <algorithm>

namespace atx::rpc {

v1::RpcStatus ok_status() {
  v1::RpcStatus status;
  status.set_code(v1::RPC_CODE_OK);
  return status;
}

v1::RpcStatus make_status(v1::RpcCode code, std::string message) {
  v1::RpcStatus status;
  status.set_code(code);
  status.set_message(std::move(message));
  return status;
}

bool MethodTable::contains(std::string_view method) const {
  return handlers_.find(method) != handlers_.end();
}

std::vector<std::string> MethodTable::method_names() const {
  std::vector<std::string> names;
  names.reserve(handlers_.size());
  for (const auto &[name, _] : handlers_) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

v1::RpcStatus MethodTable::invoke(std::string_view method, const CallContext &ctx,
                                  const std::string &request_payload,
                                  std::string &response_payload) const {
  const auto it = handlers_.find(method);
  if (it == handlers_.end()) {
    return make_status(v1::RPC_CODE_UNIMPLEMENTED, "unknown method " + std::string{method});
  }
  return it->second(ctx, request_payload, response_payload);
}

} // namespace atx::rpc
```

- [ ] **Step 8: Write `atx-rpc/src/dispatcher.cpp`**

```cpp
#include "atx/rpc/dispatcher.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <string>

namespace atx::rpc {
namespace {

[[nodiscard]] std::int64_t now_unix_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] v1::Envelope error_response(const v1::Envelope &request, v1::RpcStatus status) {
  v1::Envelope response;
  response.set_protocol_version(request.protocol_version());
  response.set_correlation_id(request.correlation_id());
  response.set_is_response(true);
  response.set_method(request.method());
  *response.mutable_status() = std::move(status);
  return response;
}

} // namespace

Dispatcher::Dispatcher(MethodTable table) : table_{std::move(table)} {}

Dispatcher::Dispatcher(Dispatcher &&other) noexcept
    : table_{std::move(other.table_)}, authenticator_{std::move(other.authenticator_)},
      incident_counter_{other.incident_counter_.load()} {}

void Dispatcher::set_authenticator(Authenticator authenticator) {
  authenticator_ = std::move(authenticator);
}

std::string Dispatcher::next_incident_id() const {
  const std::uint64_t n = incident_counter_.fetch_add(1) + 1;
  return "inc-" + std::to_string(now_unix_ns()) + "-" + std::to_string(n);
}

v1::Envelope Dispatcher::dispatch(const v1::Envelope &request, std::string_view peer) const {
  // Fail closed. A dispatcher with no authenticator gates nothing, and a
  // component whose entire job is access control must not default to open.
  if (!authenticator_) {
    return error_response(request,
                          make_status(v1::RPC_CODE_UNAUTHENTICATED, "no authenticator configured"));
  }

  auto entitlements = authenticator_(request.auth_token());
  if (!entitlements) {
    return error_response(request, make_status(v1::RPC_CODE_UNAUTHENTICATED,
                                               "token rejected: " +
                                                   std::string{atx::core::to_string(
                                                       entitlements.error().code())}));
  }

  CallContext ctx;
  ctx.entitlements = *std::move(entitlements);
  ctx.deadline_unix_ns = request.deadline_unix_ns();
  ctx.peer = std::string{peer};

  // Checked here and again at cooperative checkpoints inside long handlers. A
  // handler already inside a decode is not preempted; see call_context.hpp.
  if (ctx.deadline_passed(now_unix_ns())) {
    return error_response(request, make_status(v1::RPC_CODE_DEADLINE_EXCEEDED,
                                               "deadline passed before dispatch"));
  }

  if (!table_.contains(request.method())) {
    return error_response(
        request, make_status(v1::RPC_CODE_UNIMPLEMENTED, "unknown method " + request.method()));
  }

  std::string response_payload;
  v1::RpcStatus status;
  try {
    status = table_.invoke(request.method(), ctx, request.payload(), response_payload);
  } catch (const std::exception &error) {
    const std::string incident = next_incident_id();
    // Log locally, tell the client nothing but the incident id. Exception text
    // routinely contains paths, SQL, or credentials.
    std::cerr << "[atx-server] incident " << incident << " method=" << request.method()
              << " peer=" << peer << " what=" << error.what() << '\n';
    v1::RpcStatus internal = make_status(v1::RPC_CODE_INTERNAL, "internal error");
    internal.set_incident_id(incident);
    return error_response(request, std::move(internal));
  } catch (...) {
    const std::string incident = next_incident_id();
    std::cerr << "[atx-server] incident " << incident << " method=" << request.method()
              << " peer=" << peer << " what=<non-std exception>\n";
    v1::RpcStatus internal = make_status(v1::RPC_CODE_INTERNAL, "internal error");
    internal.set_incident_id(incident);
    return error_response(request, std::move(internal));
  }

  v1::Envelope response;
  response.set_protocol_version(request.protocol_version());
  response.set_correlation_id(request.correlation_id());
  response.set_is_response(true);
  response.set_method(request.method());
  *response.mutable_status() = std::move(status);
  if (response.status().code() == v1::RPC_CODE_OK) {
    response.set_payload(std::move(response_payload));
  }
  return response;
}

} // namespace atx::rpc
```

- [ ] **Step 9: Add the sources to `atx-rpc/CMakeLists.txt`**

```cmake
add_library(atx-rpc ${ATX_LIB_TYPE}
    src/limits.cpp
    src/byte_stream.cpp
    src/frame.cpp
    src/socket.cpp
    src/method_table.cpp
    src/dispatcher.cpp
)
```

- [ ] **Step 10: Run the tests to verify they pass**

Run: `cmake --build --preset server --target atx-rpc-tests`
Then: `ctest --preset server -R "^MethodTable\.|^Dispatcher\.|^Entitlements\." --output-on-failure`
Expected: 11 tests, all PASS.

- [ ] **Step 11: Commit**

```bash
git add atx-rpc/include/atx/rpc/call_context.hpp atx-rpc/include/atx/rpc/method_table.hpp \
        atx-rpc/include/atx/rpc/service.hpp atx-rpc/include/atx/rpc/dispatcher.hpp \
        atx-rpc/src/method_table.cpp atx-rpc/src/dispatcher.cpp \
        atx-rpc/tests/dispatcher_test.cpp atx-rpc/CMakeLists.txt
git commit -m "feat(rpc): add typed method registration and the dispatcher

MethodTable owns decode and encode, so a handler signature is
RpcStatus(const CallContext&, const Req&, Resp&) and a handler never sees bytes,
an envelope, or a socket. That is what makes 'adding a service is one class plus
one registration line' a fact rather than a claim -- a future BacktestService
touches no transport code.

Dispatcher is Envelope in, Envelope out, with no socket and no threads, so the
whole request path -- auth, deadline, decode, handler, encode, error mapping --
is testable without binding a port.

It fails closed: with no authenticator configured every call is UNAUTHENTICATED.
A component whose entire job is access control must not default to open.

A handler that throws becomes RPC_CODE_INTERNAL with a fresh incident id. The
exception text is logged and never sent, because it routinely contains paths,
SQL, or credentials; a test asserts a planted secret does not appear in the
response, and another asserts incident ids are distinct so two reports are
separable in the log.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: `atx-rpc` server runtime

Acceptor, readiness poller, bounded worker pool, connection limits. The poller is what stops an idle connection from consuming a worker: a worker blocked in `recv()` on a silent peer is a dead worker, and N idle clients would kill a pool of N.

**Files:**
- Create: `atx-rpc/include/atx/rpc/server.hpp`, `atx-rpc/src/server.cpp`
- Create: `atx-rpc/tests/server_test.cpp`
- Modify: `atx-rpc/CMakeLists.txt`

**Interfaces:**
- Consumes: `Listener`, `Socket`, `SocketStream` (Task 4); `Dispatcher` (Task 5); `read_frame`/`write_frame` (Task 3).
- Produces:
  - `struct atx::rpc::RpcServerConfig { std::string host{"127.0.0.1"}; std::uint16_t port{50051}; std::size_t worker_count{0}; std::size_t max_connections{256}; std::size_t ready_queue_depth{1024}; std::chrono::milliseconds recv_timeout{30000}; std::chrono::milliseconds send_timeout{30000}; FrameLimits frame_limits{}; std::string server_build; std::uint64_t atxvsa_schema_hash{0}; }`
  - `class atx::rpc::RpcServer` with `static Result<std::unique_ptr<RpcServer>> start(RpcServerConfig, Dispatcher)`, `std::uint16_t port() const noexcept`, `RpcServerStats stats() const`, `void shutdown() noexcept`, destructor calls `shutdown()`.
  - `struct atx::rpc::RpcServerStats { std::uint64_t accepted; std::uint64_t refused_max_connections; std::uint64_t rejected_busy; std::uint64_t requests_served; std::size_t live_connections; }`

- [ ] **Step 1: Write the failing test**

`atx-rpc/tests/server_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/rpc/dispatcher.hpp"
#include "atx/rpc/frame.hpp"
#include "atx/rpc/method_table.hpp"
#include "atx/rpc/server.hpp"
#include "atx/rpc/socket.hpp"
#include "atx/rpc/v1/envelope.pb.h"

namespace {

using atx::rpc::CallContext;
using atx::rpc::Dispatcher;
using atx::rpc::Entitlements;
using atx::rpc::FrameLimits;
using atx::rpc::MethodTable;
using atx::rpc::ok_status;
using atx::rpc::read_frame;
using atx::rpc::RpcServer;
using atx::rpc::RpcServerConfig;
using atx::rpc::Socket;
using atx::rpc::SocketStream;
using atx::rpc::write_frame;
using atx::rpc::WsaScope;
namespace v1 = atx::rpc::v1;

Dispatcher make_echo_dispatcher() {
  MethodTable table;
  table.add<v1::Hello, v1::HelloAck>(
      "atx.rpc.v1.Echo/Ping",
      [](const CallContext &, const v1::Hello &req, v1::HelloAck &resp) {
        resp.set_server_build(req.client_build());
        return ok_status();
      });
  Dispatcher dispatcher{std::move(table)};
  dispatcher.set_authenticator([](std::string_view) {
    Entitlements ent;
    ent.authenticated = true;
    return atx::core::Ok(ent);
  });
  return dispatcher;
}

RpcServerConfig test_config() {
  RpcServerConfig config;
  config.host = "127.0.0.1";
  config.port = 0; // ephemeral, so the suite never collides with a real service
  config.worker_count = 2;
  config.recv_timeout = std::chrono::milliseconds{300};
  config.send_timeout = std::chrono::milliseconds{300};
  config.server_build = "atx-server test";
  return config;
}

// Performs the handshake and returns a connected stream, or nullptr on failure.
std::unique_ptr<SocketStream> connect_and_handshake(std::uint16_t port) {
  auto socket = Socket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
  if (!socket) {
    return nullptr;
  }
  auto stream = std::make_unique<SocketStream>(std::move(*socket));

  v1::Hello hello;
  hello.set_protocol_version(atx::rpc::kProtocolVersion);
  hello.set_client_build("test");
  const std::string wire = hello.SerializeAsString();
  const auto *begin = reinterpret_cast<const std::byte *>(wire.data());
  if (!write_frame(*stream, {begin, wire.size()}, FrameLimits{})) {
    return nullptr;
  }

  std::vector<std::byte> ack_bytes;
  if (!read_frame(*stream, ack_bytes, FrameLimits{})) {
    return nullptr;
  }
  v1::HelloAck ack;
  if (!ack.ParseFromArray(ack_bytes.data(), static_cast<int>(ack_bytes.size()))) {
    return nullptr;
  }
  if (ack.status().code() != v1::RPC_CODE_OK) {
    return nullptr;
  }
  return stream;
}

bool send_ping(SocketStream &stream, std::uint64_t correlation, v1::Envelope &out) {
  v1::Hello hello;
  hello.set_client_build("ping-" + std::to_string(correlation));

  v1::Envelope request;
  request.set_protocol_version(atx::rpc::kProtocolVersion);
  request.set_correlation_id(correlation);
  request.set_method("atx.rpc.v1.Echo/Ping");
  request.set_payload(hello.SerializeAsString());
  const std::string wire = request.SerializeAsString();
  const auto *begin = reinterpret_cast<const std::byte *>(wire.data());
  if (!write_frame(stream, {begin, wire.size()}, FrameLimits{})) {
    return false;
  }
  std::vector<std::byte> response_bytes;
  if (!read_frame(stream, response_bytes, FrameLimits{})) {
    return false;
  }
  return out.ParseFromArray(response_bytes.data(), static_cast<int>(response_bytes.size()));
}

TEST(RpcServer, ServesARequestOverALoopbackSocket) {
  const WsaScope wsa;
  auto server = RpcServer::start(test_config(), make_echo_dispatcher());
  ASSERT_TRUE(server) << server.error().to_string();

  auto stream = connect_and_handshake((*server)->port());
  ASSERT_NE(stream, nullptr);

  v1::Envelope response;
  ASSERT_TRUE(send_ping(*stream, 42, response));
  EXPECT_EQ(response.correlation_id(), 42u);
  EXPECT_EQ(response.status().code(), v1::RPC_CODE_OK);

  v1::HelloAck ack;
  ASSERT_TRUE(ack.ParseFromString(response.payload()));
  EXPECT_EQ(ack.server_build(), "ping-42");
}

TEST(RpcServer, ServesManyRequestsOnOneConnection) {
  const WsaScope wsa;
  auto server = RpcServer::start(test_config(), make_echo_dispatcher());
  ASSERT_TRUE(server);
  auto stream = connect_and_handshake((*server)->port());
  ASSERT_NE(stream, nullptr);

  for (std::uint64_t i = 0; i < 20; ++i) {
    v1::Envelope response;
    ASSERT_TRUE(send_ping(*stream, i, response)) << "request " << i;
    EXPECT_EQ(response.correlation_id(), i);
  }
  EXPECT_GE((*server)->stats().requests_served, 20u);
}

// The reason the poller exists. Idle connections must not consume workers.
TEST(RpcServer, IdleConnectionsDoNotStarveTheWorkerPool) {
  const WsaScope wsa;
  RpcServerConfig config = test_config();
  config.worker_count = 2;
  auto server = RpcServer::start(config, make_echo_dispatcher());
  ASSERT_TRUE(server);
  const std::uint16_t port = (*server)->port();

  // Four idle connections against a two-worker pool. Under naive
  // thread-per-connection these would occupy every worker and the active
  // client below would never be served.
  std::vector<std::unique_ptr<SocketStream>> idle;
  for (int i = 0; i < 4; ++i) {
    auto stream = connect_and_handshake(port);
    ASSERT_NE(stream, nullptr) << "idle connection " << i;
    idle.push_back(std::move(stream));
  }

  auto active = connect_and_handshake(port);
  ASSERT_NE(active, nullptr);
  v1::Envelope response;
  ASSERT_TRUE(send_ping(*active, 7, response));
  EXPECT_EQ(response.status().code(), v1::RPC_CODE_OK);
}

TEST(RpcServer, RefusesConnectionsPastMaxConnections) {
  const WsaScope wsa;
  RpcServerConfig config = test_config();
  config.max_connections = 2;
  auto server = RpcServer::start(config, make_echo_dispatcher());
  ASSERT_TRUE(server);
  const std::uint16_t port = (*server)->port();

  std::vector<std::unique_ptr<SocketStream>> held;
  for (int i = 0; i < 2; ++i) {
    auto stream = connect_and_handshake(port);
    ASSERT_NE(stream, nullptr) << "connection " << i;
    held.push_back(std::move(stream));
  }

  // The third is accepted at the TCP layer and then closed by the server, so
  // the handshake cannot complete.
  auto rejected = connect_and_handshake(port);
  EXPECT_EQ(rejected, nullptr);
  EXPECT_GE((*server)->stats().refused_max_connections, 1u);
}

// A peer that connects and then stalls mid-frame must be dropped by the recv
// timeout rather than pinning a worker.
TEST(RpcServer, SlowlorisConnectionIsDroppedAndThePoolKeepsServing) {
  const WsaScope wsa;
  RpcServerConfig config = test_config();
  config.worker_count = 1;
  config.recv_timeout = std::chrono::milliseconds{200};
  auto server = RpcServer::start(config, make_echo_dispatcher());
  ASSERT_TRUE(server);
  const std::uint16_t port = (*server)->port();

  // Send a header claiming 64 bytes, then send nothing.
  auto slow_socket = Socket::connect("127.0.0.1", port, std::chrono::milliseconds{2000});
  ASSERT_TRUE(slow_socket);
  SocketStream slow{std::move(*slow_socket)};
  std::vector<std::byte> partial(8);
  const std::uint32_t magic = atx::rpc::kFrameMagic;
  const std::uint32_t length = 64;
  std::memcpy(partial.data(), &magic, 4);
  std::memcpy(partial.data() + 4, &length, 4);
  ASSERT_TRUE(slow.write_all(partial));

  // The one worker must be free again well before the test's patience runs out.
  auto healthy = connect_and_handshake(port);
  ASSERT_NE(healthy, nullptr);
  v1::Envelope response;
  ASSERT_TRUE(send_ping(*healthy, 1, response));
  EXPECT_EQ(response.status().code(), v1::RPC_CODE_OK);
}

TEST(RpcServer, RejectsAProtocolVersionMismatchAtHandshake) {
  const WsaScope wsa;
  auto server = RpcServer::start(test_config(), make_echo_dispatcher());
  ASSERT_TRUE(server);

  auto socket = Socket::connect("127.0.0.1", (*server)->port(), std::chrono::milliseconds{2000});
  ASSERT_TRUE(socket);
  SocketStream stream{std::move(*socket)};

  v1::Hello hello;
  hello.set_protocol_version(atx::rpc::kProtocolVersion + 99);
  hello.set_client_build("wrong-version");
  const std::string wire = hello.SerializeAsString();
  const auto *begin = reinterpret_cast<const std::byte *>(wire.data());
  ASSERT_TRUE(write_frame(stream, {begin, wire.size()}, FrameLimits{}));

  std::vector<std::byte> ack_bytes;
  ASSERT_TRUE(read_frame(stream, ack_bytes, FrameLimits{}));
  v1::HelloAck ack;
  ASSERT_TRUE(ack.ParseFromArray(ack_bytes.data(), static_cast<int>(ack_bytes.size())));
  EXPECT_EQ(ack.status().code(), v1::RPC_CODE_FAILED_PRECONDITION);
  // The message must name both versions so the mismatch is diagnosable from
  // the client alone.
  EXPECT_NE(ack.status().message().find(std::to_string(atx::rpc::kProtocolVersion)),
            std::string::npos);
  EXPECT_NE(ack.status().message().find(std::to_string(atx::rpc::kProtocolVersion + 99)),
            std::string::npos);
}

TEST(RpcServer, ShutdownIsIdempotentAndReleasesThePort) {
  const WsaScope wsa;
  auto server = RpcServer::start(test_config(), make_echo_dispatcher());
  ASSERT_TRUE(server);
  (*server)->shutdown();
  (*server)->shutdown(); // must not hang, crash, or double-join
  SUCCEED();
}

TEST(RpcServer, RefusesANonLoopbackBind) {
  const WsaScope wsa;
  RpcServerConfig config = test_config();
  config.host = "0.0.0.0";
  auto server = RpcServer::start(config, make_echo_dispatcher());
  ASSERT_FALSE(server);
  EXPECT_EQ(server.error().code(), atx::core::ErrorCode::PermissionDenied);
  EXPECT_NE(server.error().message().find("loopback"), std::string::npos);
}

} // namespace
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset server --target atx-rpc-tests`
Expected: FAIL at compile — `atx/rpc/server.hpp: No such file or directory`.

- [ ] **Step 3: Write `atx-rpc/include/atx/rpc/server.hpp`**

```cpp
#pragma once

// RpcServer — acceptor + readiness poller + bounded worker pool.
//
// Why a poller rather than plain thread-per-connection handed to a pool:
// a worker blocked in recv() on an idle connection is a dead worker, so N idle
// clients would kill a pool of N. The poller watches every idle connection with
// one poll() call and hands a worker only sockets already known to be readable.
// Reads therefore stay blocking -- no async state machine -- while an idle
// connection costs one pollfd instead of one thread.
//
//   accept thread : enforces max_connections, registers with the poller
//   poller thread : poll() over idle conns; readable -> bounded ready queue,
//                   marks the conn in-flight so it is not re-dispatched
//   worker 0..N   : pop, blocking-read one frame, dispatch, write, un-mark
//
// A connection that becomes readable and then stalls mid-frame (slowloris) is
// bounded by the socket's recv timeout, not by the poller. The ready queue is
// bounded and overflow answers RPC_CODE_BUSY rather than growing.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "atx/core/error.hpp"
#include "atx/rpc/dispatcher.hpp"
#include "atx/rpc/limits.hpp"

namespace atx::rpc {

struct RpcServerConfig {
  // Loopback only. RpcServer::start refuses anything else: there is no TLS in
  // this build, so a remote bind would ship plaintext tokens.
  std::string host{"127.0.0.1"};
  std::uint16_t port{50051}; // 0 = ephemeral; read it back with port()
  std::size_t worker_count{0}; // 0 = std::thread::hardware_concurrency()
  std::size_t max_connections{256};
  std::size_t ready_queue_depth{1024};
  std::chrono::milliseconds recv_timeout{30000};
  std::chrono::milliseconds send_timeout{30000};
  FrameLimits frame_limits{};
  std::string server_build;
  std::uint64_t atxvsa_schema_hash{0};
};

struct RpcServerStats {
  std::uint64_t accepted{0};
  std::uint64_t refused_max_connections{0};
  std::uint64_t rejected_busy{0};
  std::uint64_t requests_served{0};
  std::size_t live_connections{0};
};

class RpcServer {
public:
  // Fails with PermissionDenied if config.host is not loopback. This is a hard
  // gate, not a warning.
  [[nodiscard]] static atx::core::Result<std::unique_ptr<RpcServer>>
  start(RpcServerConfig config, Dispatcher dispatcher);

  ~RpcServer();
  RpcServer(const RpcServer &) = delete;
  RpcServer &operator=(const RpcServer &) = delete;
  RpcServer(RpcServer &&) = delete;
  RpcServer &operator=(RpcServer &&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] RpcServerStats stats() const;

  // Idempotent. Safe to call from any thread and from the destructor.
  void shutdown() noexcept;

private:
  class Impl;
  explicit RpcServer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::rpc
```

- [ ] **Step 4: Write `atx-rpc/src/server.cpp`**

```cpp
#include "atx/rpc/server.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "atx/rpc/frame.hpp"
#include "atx/rpc/socket.hpp"
#include "atx/rpc/v1/envelope.pb.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace atx::rpc {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Status;

#if defined(_WIN32)
using PollFd = WSAPOLLFD;
[[nodiscard]] int poll_wait(PollFd *fds, unsigned long count, int timeout_ms) {
  return ::WSAPoll(fds, count, timeout_ms);
}
#else
using PollFd = pollfd;
[[nodiscard]] int poll_wait(PollFd *fds, unsigned long count, int timeout_ms) {
  return ::poll(fds, static_cast<nfds_t>(count), timeout_ms);
}
#endif

// One live client connection. `in_flight` keeps the poller from handing the
// same connection to a second worker while the first is still reading it.
struct Connection {
  explicit Connection(Socket socket) : stream{std::move(socket)} {}
  SocketStream stream;
  std::atomic<bool> in_flight{false};
  std::atomic<bool> closed{false};
  bool handshaked{false};
};

} // namespace

class RpcServer::Impl {
public:
  Impl(RpcServerConfig config, Dispatcher dispatcher, Listener listener)
      : config_{std::move(config)}, dispatcher_{std::move(dispatcher)},
        listener_{std::move(listener)} {}

  ~Impl() { shutdown(); }

  void launch() {
    const std::size_t workers =
        config_.worker_count != 0
            ? config_.worker_count
            : std::max<std::size_t>(2, std::thread::hardware_concurrency());
    acceptor_ = std::thread{[this] { accept_loop(); }};
    poller_ = std::thread{[this] { poll_loop(); }};
    workers_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  void shutdown() noexcept {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) {
      return; // already stopping; idempotent by contract
    }
    listener_.close(); // unblocks accept()
    {
      const std::lock_guard<std::mutex> lock{mutex_};
      for (auto &[id, conn] : connections_) {
        conn->closed.store(true);
        conn->stream.socket().close(); // unblocks any worker inside recv()
      }
    }
    queue_cv_.notify_all();
    if (acceptor_.joinable()) {
      acceptor_.join();
    }
    if (poller_.joinable()) {
      poller_.join();
    }
    for (std::thread &worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
    const std::lock_guard<std::mutex> lock{mutex_};
    connections_.clear();
  }

  [[nodiscard]] std::uint16_t port() const noexcept { return listener_.port(); }

  [[nodiscard]] RpcServerStats stats() const {
    RpcServerStats out;
    out.accepted = accepted_.load();
    out.refused_max_connections = refused_.load();
    out.rejected_busy = busy_.load();
    out.requests_served = served_.load();
    const std::lock_guard<std::mutex> lock{mutex_};
    out.live_connections = connections_.size();
    return out;
  }

private:
  void accept_loop() {
    while (!stopping_.load()) {
      auto socket = listener_.accept();
      if (!socket) {
        if (stopping_.load()) {
          return;
        }
        continue;
      }
      if (!socket->set_timeouts(config_.recv_timeout, config_.send_timeout)) {
        continue; // socket closes with the Result
      }
      {
        const std::lock_guard<std::mutex> lock{mutex_};
        if (connections_.size() >= config_.max_connections) {
          refused_.fetch_add(1);
          continue; // socket closes here; the client sees a reset
        }
        const std::uint64_t id = next_id_++;
        connections_.emplace(id, std::make_shared<Connection>(std::move(*socket)));
      }
      accepted_.fetch_add(1);
    }
  }

  void poll_loop() {
    std::vector<PollFd> fds;
    std::vector<std::shared_ptr<Connection>> polled;
    while (!stopping_.load()) {
      fds.clear();
      polled.clear();
      {
        const std::lock_guard<std::mutex> lock{mutex_};
        for (auto it = connections_.begin(); it != connections_.end();) {
          const std::shared_ptr<Connection> &conn = it->second;
          if (conn->closed.load()) {
            it = connections_.erase(it);
            continue;
          }
          // An in-flight connection belongs to a worker right now; polling it
          // would hand the same socket to a second worker.
          if (!conn->in_flight.load()) {
            PollFd entry{};
            entry.fd = static_cast<decltype(entry.fd)>(conn->stream.socket().native());
            entry.events = POLLRDNORM;
            fds.push_back(entry);
            polled.push_back(conn);
          }
          ++it;
        }
      }

      if (fds.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        continue;
      }

      const int ready = poll_wait(fds.data(), static_cast<unsigned long>(fds.size()), 50);
      if (ready <= 0) {
        continue;
      }
      for (std::size_t i = 0; i < fds.size(); ++i) {
        const bool readable = (fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR)) != 0;
        if (!readable) {
          continue;
        }
        bool expected = false;
        if (!polled[i]->in_flight.compare_exchange_strong(expected, true)) {
          continue;
        }
        std::unique_lock<std::mutex> lock{queue_mutex_};
        if (ready_.size() >= config_.ready_queue_depth) {
          lock.unlock();
          busy_.fetch_add(1);
          send_busy(*polled[i]);
          polled[i]->in_flight.store(false);
          continue;
        }
        ready_.push_back(polled[i]);
        lock.unlock();
        queue_cv_.notify_one();
      }
    }
  }

  void worker_loop() {
    while (true) {
      std::shared_ptr<Connection> conn;
      {
        std::unique_lock<std::mutex> lock{queue_mutex_};
        queue_cv_.wait(lock, [this] { return stopping_.load() || !ready_.empty(); });
        if (stopping_.load() && ready_.empty()) {
          return;
        }
        conn = ready_.front();
        ready_.pop_front();
      }
      serve_one(*conn);
      conn->in_flight.store(false);
    }
  }

  // Reads exactly one frame and answers it. Returning after a single frame is
  // what lets the poller round-robin connections instead of one chatty client
  // monopolising a worker.
  void serve_one(Connection &conn) {
    std::vector<std::byte> payload;
    const Status read = read_frame(conn.stream, payload, config_.frame_limits);
    if (!read) {
      conn.closed.store(true);
      conn.stream.socket().close();
      return;
    }

    if (!conn.handshaked) {
      handle_handshake(conn, payload);
      return;
    }

    v1::Envelope request;
    if (!request.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
      conn.closed.store(true);
      conn.stream.socket().close();
      return;
    }

    const v1::Envelope response = dispatcher_.dispatch(request, "loopback");
    served_.fetch_add(1);
    if (!write_envelope(conn, response)) {
      conn.closed.store(true);
      conn.stream.socket().close();
    }
  }

  void handle_handshake(Connection &conn, const std::vector<std::byte> &payload) {
    v1::Hello hello;
    v1::HelloAck ack;
    ack.set_protocol_version(kProtocolVersion);
    ack.set_server_build(config_.server_build);
    ack.set_atxvsa_schema_hash(config_.atxvsa_schema_hash);

    if (!hello.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
      *ack.mutable_status() =
          make_status(v1::RPC_CODE_INVALID_ARGUMENT, "first frame is not a Hello");
    } else if (hello.protocol_version() != kProtocolVersion) {
      // Name BOTH versions: a client must be able to diagnose the mismatch
      // without server log access.
      *ack.mutable_status() = make_status(
          v1::RPC_CODE_FAILED_PRECONDITION,
          "protocol version mismatch: server speaks " + std::to_string(kProtocolVersion) +
              ", client offered " + std::to_string(hello.protocol_version()));
    } else {
      *ack.mutable_status() = ok_status();
      conn.handshaked = true;
    }

    const bool accepted = ack.status().code() == v1::RPC_CODE_OK;
    const std::string wire = ack.SerializeAsString();
    const auto *begin = reinterpret_cast<const std::byte *>(wire.data());
    const Status written =
        write_frame(conn.stream, {begin, wire.size()}, config_.frame_limits);
    if (!accepted || !written) {
      conn.closed.store(true);
      conn.stream.socket().close();
    }
  }

  void send_busy(Connection &conn) {
    v1::Envelope response;
    response.set_protocol_version(kProtocolVersion);
    response.set_is_response(true);
    *response.mutable_status() =
        make_status(v1::RPC_CODE_BUSY, "server ready queue is full; retry with backoff");
    (void)write_envelope(conn, response);
  }

  [[nodiscard]] bool write_envelope(Connection &conn, const v1::Envelope &envelope) {
    const std::string wire = envelope.SerializeAsString();
    const auto *begin = reinterpret_cast<const std::byte *>(wire.data());
    return static_cast<bool>(
        write_frame(conn.stream, {begin, wire.size()}, config_.frame_limits));
  }

  RpcServerConfig config_;
  Dispatcher dispatcher_;
  Listener listener_;

  mutable std::mutex mutex_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Connection>> connections_;
  std::uint64_t next_id_{1};

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<std::shared_ptr<Connection>> ready_;

  std::thread acceptor_;
  std::thread poller_;
  std::vector<std::thread> workers_;
  std::atomic<bool> stopping_{false};

  std::atomic<std::uint64_t> accepted_{0};
  std::atomic<std::uint64_t> refused_{0};
  std::atomic<std::uint64_t> busy_{0};
  std::atomic<std::uint64_t> served_{0};
};

RpcServer::RpcServer(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
RpcServer::~RpcServer() = default;

atx::core::Result<std::unique_ptr<RpcServer>> RpcServer::start(RpcServerConfig config,
                                                               Dispatcher dispatcher) {
  // The hard gate. There is no TLS in this build, so a non-loopback bind would
  // put plaintext auth tokens and unencrypted market data on the network.
  if (!is_loopback(config.host)) {
    return Err(ErrorCode::PermissionDenied,
               "refusing to bind non-loopback address '" + config.host +
                   "': this build has no transport encryption, so a remote bind would "
                   "expose plaintext auth tokens. Bind a loopback address instead.");
  }

  ATX_TRY(Listener listener, Listener::bind(config.host, config.port, 128));
  auto impl = std::make_unique<Impl>(std::move(config), std::move(dispatcher),
                                     std::move(listener));
  impl->launch();
  return Ok(std::unique_ptr<RpcServer>{new RpcServer{std::move(impl)}});
}

std::uint16_t RpcServer::port() const noexcept { return impl_->port(); }
RpcServerStats RpcServer::stats() const { return impl_->stats(); }
void RpcServer::shutdown() noexcept { impl_->shutdown(); }

} // namespace atx::rpc
```

- [ ] **Step 5: Add `src/server.cpp` to `atx-rpc/CMakeLists.txt`**

```cmake
add_library(atx-rpc ${ATX_LIB_TYPE}
    src/limits.cpp
    src/byte_stream.cpp
    src/frame.cpp
    src/socket.cpp
    src/method_table.cpp
    src/dispatcher.cpp
    src/server.cpp
)
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset server --target atx-rpc-tests`
Then: `ctest --preset server -R "^RpcServer\." --output-on-failure`
Expected: 8 tests, all PASS.

- [ ] **Step 7: Run the suite three times to catch ordering-dependent flake**

Run: `ctest --preset server -L atx_server --repeat until-fail:3 --output-on-failure`
Expected: all PASS on every repetition. A socket suite that passes once and fails on the third run is not passing.

- [ ] **Step 8: Commit**

```bash
git add atx-rpc/include/atx/rpc/server.hpp atx-rpc/src/server.cpp \
        atx-rpc/tests/server_test.cpp atx-rpc/CMakeLists.txt
git commit -m "feat(rpc): add the server runtime: acceptor, poller, bounded worker pool

A readiness poller sits between the acceptor and the worker pool. Without it, a
worker blocked in recv() on an idle connection is a dead worker and N idle
clients kill a pool of N; the test that proves this holds runs four idle
connections against a two-worker pool and then requires a fifth client to be
served. The poller watches every idle connection with one poll() call and hands
a worker only sockets already known readable, so reads stay blocking and no
async state machine is needed.

A worker serves exactly one frame and returns the connection to the poller.
That is what keeps one chatty client from monopolising a worker.

An in-flight flag stops the poller handing the same socket to a second worker,
which would interleave two readers on one stream.

Slowloris is bounded by the socket recv timeout rather than by the poller: a
connection that becomes readable and then stalls mid-frame is dropped, and the
test asserts a single-worker pool still serves a healthy client afterwards.

Ready-queue overflow answers RPC_CODE_BUSY instead of growing without bound.
Connections past max_connections are refused at the accept boundary.

start() refuses a non-loopback bind with PermissionDenied. This build has no
transport encryption, so a remote bind would put plaintext auth tokens on the
network; the error names the reason so the operator is not left guessing.

The handshake names both protocol versions on mismatch, so a client can
diagnose it without server log access.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: `atx-rpc` client

**Files:**
- Create: `atx-rpc/include/atx/rpc/client.hpp`, `atx-rpc/src/client.cpp`
- Create: `atx-rpc/tests/client_test.cpp`
- Modify: `atx-rpc/CMakeLists.txt`

**Interfaces:**
- Consumes: `Socket`, `SocketStream` (Task 4); `read_frame`/`write_frame` (Task 3); `v1::Envelope` (Task 2).
- Produces:
  - `struct atx::rpc::RpcClientConfig { std::string host{"127.0.0.1"}; std::uint16_t port{50051}; std::string auth_token; std::chrono::milliseconds connect_timeout{5000}; std::chrono::milliseconds call_timeout{30000}; FrameLimits frame_limits{}; std::string client_build; }`
  - `struct atx::rpc::ServerIdentity { std::uint32_t protocol_version; std::string server_build; std::uint64_t atxvsa_schema_hash; }`
  - `class atx::rpc::RpcClient` with `static Result<RpcClient> connect(RpcClientConfig)`, `template <class Req, class Resp> Result<Resp> call(std::string_view method, const Req&)`, `const ServerIdentity& identity() const noexcept`, `bool connected() const noexcept`, `void close() noexcept`.
  - `struct atx::rpc::CallError` is **not** introduced; a failed call returns `atx::core::Err` whose `ErrorCode` is mapped from `RpcCode` by `atx::rpc::error_code_for(v1::RpcCode)`, and whose message carries the wire message plus any incident id.

- [ ] **Step 1: Write the failing test**

`atx-rpc/tests/client_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/rpc/client.hpp"
#include "atx/rpc/dispatcher.hpp"
#include "atx/rpc/method_table.hpp"
#include "atx/rpc/server.hpp"
#include "atx/rpc/socket.hpp"
#include "atx/rpc/v1/envelope.pb.h"

namespace {

using atx::core::ErrorCode;
using atx::rpc::CallContext;
using atx::rpc::Dispatcher;
using atx::rpc::Entitlements;
using atx::rpc::make_status;
using atx::rpc::MethodTable;
using atx::rpc::ok_status;
using atx::rpc::RpcClient;
using atx::rpc::RpcClientConfig;
using atx::rpc::RpcServer;
using atx::rpc::RpcServerConfig;
using atx::rpc::WsaScope;
namespace v1 = atx::rpc::v1;

Dispatcher make_dispatcher() {
  MethodTable table;
  table.add<v1::Hello, v1::HelloAck>(
      "atx.rpc.v1.Echo/Ping",
      [](const CallContext &, const v1::Hello &req, v1::HelloAck &resp) {
        resp.set_server_build(req.client_build());
        return ok_status();
      });
  table.add<v1::Hello, v1::HelloAck>(
      "atx.rpc.v1.Echo/Missing",
      [](const CallContext &, const v1::Hello &, v1::HelloAck &) {
        return make_status(v1::RPC_CODE_NOT_FOUND, "no such surface");
      });
  Dispatcher dispatcher{std::move(table)};
  dispatcher.set_authenticator([](std::string_view token) -> atx::core::Result<Entitlements> {
    if (token != "good") {
      return atx::core::Err(ErrorCode::PermissionDenied, "bad token");
    }
    Entitlements ent;
    ent.authenticated = true;
    return atx::core::Ok(ent);
  });
  return dispatcher;
}

std::unique_ptr<RpcServer> start_server(std::uint16_t &port_out) {
  RpcServerConfig config;
  config.host = "127.0.0.1";
  config.port = 0;
  config.worker_count = 2;
  config.server_build = "atx-server test";
  config.atxvsa_schema_hash = 0xABCD'1234'5678'9ABCULL;
  auto server = RpcServer::start(config, make_dispatcher());
  if (!server) {
    return nullptr;
  }
  port_out = (*server)->port();
  return std::move(*server);
}

RpcClientConfig client_config(std::uint16_t port, std::string token) {
  RpcClientConfig config;
  config.host = "127.0.0.1";
  config.port = port;
  config.auth_token = std::move(token);
  config.client_build = "test-client";
  config.call_timeout = std::chrono::milliseconds{3000};
  return config;
}

TEST(RpcClient, ConnectsHandshakesAndCalls) {
  const WsaScope wsa;
  std::uint16_t port = 0;
  auto server = start_server(port);
  ASSERT_NE(server, nullptr);

  auto client = RpcClient::connect(client_config(port, "good"));
  ASSERT_TRUE(client) << client.error().to_string();
  EXPECT_EQ(client->identity().protocol_version, atx::rpc::kProtocolVersion);
  EXPECT_EQ(client->identity().server_build, "atx-server test");
  EXPECT_EQ(client->identity().atxvsa_schema_hash, 0xABCD'1234'5678'9ABCULL);

  v1::Hello request;
  request.set_client_build("hello-from-test");
  auto response = client->call<v1::Hello, v1::HelloAck>("atx.rpc.v1.Echo/Ping", request);
  ASSERT_TRUE(response) << response.error().to_string();
  EXPECT_EQ(response->server_build(), "hello-from-test");
}

TEST(RpcClient, MapsWireStatusOntoTheCoreErrorDomain) {
  const WsaScope wsa;
  std::uint16_t port = 0;
  auto server = start_server(port);
  ASSERT_NE(server, nullptr);

  auto client = RpcClient::connect(client_config(port, "good"));
  ASSERT_TRUE(client);

  v1::Hello request;
  auto response = client->call<v1::Hello, v1::HelloAck>("atx.rpc.v1.Echo/Missing", request);
  ASSERT_FALSE(response);
  EXPECT_EQ(response.error().code(), ErrorCode::NotFound);
  EXPECT_NE(response.error().message().find("no such surface"), std::string::npos);
}

TEST(RpcClient, UnknownMethodIsNotImplemented) {
  const WsaScope wsa;
  std::uint16_t port = 0;
  auto server = start_server(port);
  ASSERT_NE(server, nullptr);
  auto client = RpcClient::connect(client_config(port, "good"));
  ASSERT_TRUE(client);

  v1::Hello request;
  auto response = client->call<v1::Hello, v1::HelloAck>("atx.rpc.v1.Echo/Nope", request);
  ASSERT_FALSE(response);
  EXPECT_EQ(response.error().code(), ErrorCode::NotImplemented);
}

TEST(RpcClient, BadTokenSurfacesAsPermissionDenied) {
  const WsaScope wsa;
  std::uint16_t port = 0;
  auto server = start_server(port);
  ASSERT_NE(server, nullptr);
  auto client = RpcClient::connect(client_config(port, "wrong"));
  ASSERT_TRUE(client) << "the handshake does not authenticate; only calls do";

  v1::Hello request;
  auto response = client->call<v1::Hello, v1::HelloAck>("atx.rpc.v1.Echo/Ping", request);
  ASSERT_FALSE(response);
  EXPECT_EQ(response.error().code(), ErrorCode::PermissionDenied);
}

TEST(RpcClient, ConnectToANonListeningPortFails) {
  const WsaScope wsa;
  std::uint16_t port = 0;
  {
    auto server = start_server(port);
    ASSERT_NE(server, nullptr);
  } // server destroyed; port is free

  auto client = RpcClient::connect(client_config(port, "good"));
  EXPECT_FALSE(client);
}

TEST(RpcClient, CorrelationIdsAdvanceAndResponsesMatchTheirRequests) {
  const WsaScope wsa;
  std::uint16_t port = 0;
  auto server = start_server(port);
  ASSERT_NE(server, nullptr);
  auto client = RpcClient::connect(client_config(port, "good"));
  ASSERT_TRUE(client);

  for (int i = 0; i < 25; ++i) {
    v1::Hello request;
    request.set_client_build("seq-" + std::to_string(i));
    auto response = client->call<v1::Hello, v1::HelloAck>("atx.rpc.v1.Echo/Ping", request);
    ASSERT_TRUE(response) << "iteration " << i;
    EXPECT_EQ(response->server_build(), "seq-" + std::to_string(i));
  }
}

// Concurrent clients on separate connections must not interfere.
TEST(RpcClient, ManyConcurrentClientsAreServedCorrectly) {
  const WsaScope wsa;
  std::uint16_t port = 0;
  auto server = start_server(port);
  ASSERT_NE(server, nullptr);

  constexpr int kClients = 8;
  constexpr int kCallsEach = 10;
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};

  for (int c = 0; c < kClients; ++c) {
    threads.emplace_back([port, c, &failures]() {
      auto client = RpcClient::connect(client_config(port, "good"));
      if (!client) {
        failures.fetch_add(1);
        return;
      }
      for (int i = 0; i < kCallsEach; ++i) {
        const std::string tag = "c" + std::to_string(c) + "-" + std::to_string(i);
        v1::Hello request;
        request.set_client_build(tag);
        auto response =
            client->call<v1::Hello, v1::HelloAck>("atx.rpc.v1.Echo/Ping", request);
        if (!response || response->server_build() != tag) {
          failures.fetch_add(1);
          return;
        }
      }
    });
  }
  for (std::thread &t : threads) {
    t.join();
  }
  EXPECT_EQ(failures.load(), 0);
  EXPECT_GE(server->stats().requests_served,
            static_cast<std::uint64_t>(kClients * kCallsEach));
}

} // namespace
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset server --target atx-rpc-tests`
Expected: FAIL at compile — `atx/rpc/client.hpp: No such file or directory`.

- [ ] **Step 3: Write `atx-rpc/include/atx/rpc/client.hpp`**

```cpp
#pragma once

// RpcClient — connect, handshake, call.
//
// One connection, requests issued strictly serially. call() is NOT thread-safe:
// two threads sharing one client would interleave frames on one stream. Give
// each thread its own client, or serialize externally. The correlation id is
// still carried so pipelining can be added later without a wire break.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/rpc/limits.hpp"
#include "atx/rpc/socket.hpp"
#include "atx/rpc/v1/envelope.pb.h"

namespace atx::rpc {

struct RpcClientConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{50051};
  // Sent on every request. Required: the server rejects an unknown token with
  // UNAUTHENTICATED.
  std::string auth_token;
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds call_timeout{30000};
  FrameLimits frame_limits{};
  std::string client_build;
};

struct ServerIdentity {
  std::uint32_t protocol_version{0};
  std::string server_build;
  // Lets a client detect archive-layout skew at connect time rather than on its
  // first GetSurfaceBlob.
  std::uint64_t atxvsa_schema_hash{0};
};

// Maps a wire code onto the core error domain so callers use one vocabulary.
// RPC_CODE_BUSY and RPC_CODE_UNAVAILABLE both become Unavailable: both mean
// "retry with backoff", and a caller that needs to distinguish them can read
// the message.
[[nodiscard]] atx::core::ErrorCode error_code_for(v1::RpcCode code) noexcept;

class RpcClient {
public:
  [[nodiscard]] static atx::core::Result<RpcClient> connect(RpcClientConfig config);

  RpcClient(RpcClient &&) noexcept;
  RpcClient &operator=(RpcClient &&) noexcept;
  RpcClient(const RpcClient &) = delete;
  RpcClient &operator=(const RpcClient &) = delete;
  ~RpcClient();

  template <class Req, class Resp>
  [[nodiscard]] atx::core::Result<Resp> call(std::string_view method, const Req &request) {
    std::string response_payload;
    ATX_TRY_VOID(call_raw(method, request.SerializeAsString(), response_payload));
    Resp response;
    if (!response.ParseFromString(response_payload)) {
      return atx::core::Err(atx::core::ErrorCode::ParseError,
                            "response payload is not a valid " +
                                std::string{Resp::descriptor()->full_name()});
    }
    return atx::core::Ok(std::move(response));
  }

  [[nodiscard]] const ServerIdentity &identity() const noexcept;
  [[nodiscard]] bool connected() const noexcept;
  void close() noexcept;

private:
  class Impl;
  explicit RpcClient(std::unique_ptr<Impl> impl);

  // Writes one request frame and reads its response. Non-OK wire status becomes
  // an Err carrying the wire message and, when present, the incident id.
  [[nodiscard]] atx::core::Status call_raw(std::string_view method,
                                           std::string request_payload,
                                           std::string &response_payload);

  std::unique_ptr<Impl> impl_;
};

} // namespace atx::rpc
```

- [ ] **Step 4: Write `atx-rpc/src/client.cpp`**

```cpp
#include "atx/rpc/client.hpp"

#include <chrono>
#include <vector>

#include "atx/rpc/frame.hpp"

namespace atx::rpc {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Status;

[[nodiscard]] std::int64_t deadline_from(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0) {
    return 0;
  }
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now + timeout).count();
}

} // namespace

atx::core::ErrorCode error_code_for(v1::RpcCode code) noexcept {
  switch (code) {
  case v1::RPC_CODE_OK:
    return ErrorCode::Unknown; // never mapped; OK is not an error
  case v1::RPC_CODE_NOT_FOUND:
    return ErrorCode::NotFound;
  case v1::RPC_CODE_INVALID_ARGUMENT:
    return ErrorCode::InvalidArgument;
  case v1::RPC_CODE_PERMISSION_DENIED:
  case v1::RPC_CODE_UNAUTHENTICATED:
    return ErrorCode::PermissionDenied;
  case v1::RPC_CODE_RESOURCE_EXHAUSTED:
    return ErrorCode::OutOfRange;
  case v1::RPC_CODE_FAILED_PRECONDITION:
    return ErrorCode::InvalidArgument;
  case v1::RPC_CODE_UNAVAILABLE:
  case v1::RPC_CODE_BUSY:
  case v1::RPC_CODE_DEADLINE_EXCEEDED:
    return ErrorCode::Unavailable;
  case v1::RPC_CODE_DATA_LOSS:
    return ErrorCode::ParseError;
  case v1::RPC_CODE_UNIMPLEMENTED:
    return ErrorCode::NotImplemented;
  case v1::RPC_CODE_INTERNAL:
    return ErrorCode::Internal;
  default:
    return ErrorCode::Unknown;
  }
}

class RpcClient::Impl {
public:
  Impl(RpcClientConfig config, SocketStream stream)
      : config_{std::move(config)}, stream_{std::move(stream)} {}

  RpcClientConfig config_;
  SocketStream stream_;
  ServerIdentity identity_;
  std::uint64_t next_correlation_{1};
  bool connected_{true};
};

RpcClient::RpcClient(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
RpcClient::RpcClient(RpcClient &&) noexcept = default;
RpcClient &RpcClient::operator=(RpcClient &&) noexcept = default;
RpcClient::~RpcClient() = default;

atx::core::Result<RpcClient> RpcClient::connect(RpcClientConfig config) {
  ATX_TRY(Socket socket,
          Socket::connect(config.host, config.port, config.connect_timeout));
  ATX_TRY_VOID(socket.set_timeouts(config.call_timeout, config.call_timeout));

  auto impl = std::make_unique<Impl>(std::move(config), SocketStream{std::move(socket)});

  v1::Hello hello;
  hello.set_protocol_version(kProtocolVersion);
  hello.set_client_build(impl->config_.client_build);
  const std::string wire = hello.SerializeAsString();
  const auto *begin = reinterpret_cast<const std::byte *>(wire.data());
  ATX_TRY_VOID(
      write_frame(impl->stream_, {begin, wire.size()}, impl->config_.frame_limits));

  std::vector<std::byte> ack_bytes;
  ATX_TRY_VOID(read_frame(impl->stream_, ack_bytes, impl->config_.frame_limits));
  v1::HelloAck ack;
  if (!ack.ParseFromArray(ack_bytes.data(), static_cast<int>(ack_bytes.size()))) {
    return Err(ErrorCode::ParseError, "handshake reply is not a HelloAck");
  }
  if (ack.status().code() != v1::RPC_CODE_OK) {
    return Err(error_code_for(ack.status().code()),
               "handshake rejected: " + ack.status().message());
  }

  impl->identity_.protocol_version = ack.protocol_version();
  impl->identity_.server_build = ack.server_build();
  impl->identity_.atxvsa_schema_hash = ack.atxvsa_schema_hash();
  return Ok(RpcClient{std::move(impl)});
}

const ServerIdentity &RpcClient::identity() const noexcept { return impl_->identity_; }
bool RpcClient::connected() const noexcept { return impl_ && impl_->connected_; }

void RpcClient::close() noexcept {
  if (impl_) {
    impl_->stream_.socket().close();
    impl_->connected_ = false;
  }
}

Status RpcClient::call_raw(std::string_view method, std::string request_payload,
                           std::string &response_payload) {
  if (!connected()) {
    return Err(ErrorCode::Unavailable, "client is not connected");
  }

  const std::uint64_t correlation = impl_->next_correlation_++;
  v1::Envelope request;
  request.set_protocol_version(kProtocolVersion);
  request.set_correlation_id(correlation);
  request.set_is_response(false);
  request.set_method(std::string{method});
  request.set_payload(std::move(request_payload));
  request.set_auth_token(impl_->config_.auth_token);
  request.set_deadline_unix_ns(deadline_from(impl_->config_.call_timeout));

  const std::string wire = request.SerializeAsString();
  const auto *begin = reinterpret_cast<const std::byte *>(wire.data());
  const Status written =
      write_frame(impl_->stream_, {begin, wire.size()}, impl_->config_.frame_limits);
  if (!written) {
    impl_->connected_ = false;
    return written;
  }

  std::vector<std::byte> response_bytes;
  const Status read =
      read_frame(impl_->stream_, response_bytes, impl_->config_.frame_limits);
  if (!read) {
    impl_->connected_ = false;
    return read;
  }

  v1::Envelope response;
  if (!response.ParseFromArray(response_bytes.data(),
                               static_cast<int>(response_bytes.size()))) {
    impl_->connected_ = false;
    return Err(ErrorCode::ParseError, "response frame is not an Envelope");
  }

  // Requests are issued strictly serially on this connection, so a mismatched
  // correlation id means the stream has desynchronized and nothing further on
  // it can be trusted.
  if (response.correlation_id() != correlation) {
    impl_->connected_ = false;
    return Err(ErrorCode::Internal, "correlation id mismatch: expected " +
                                        std::to_string(correlation) + ", got " +
                                        std::to_string(response.correlation_id()));
  }

  if (response.status().code() != v1::RPC_CODE_OK) {
    std::string message = response.status().message();
    if (!response.status().incident_id().empty()) {
      message += " (incident " + response.status().incident_id() + ")";
    }
    return Err(error_code_for(response.status().code()), std::move(message));
  }

  response_payload = response.payload();
  return Ok();
}

} // namespace atx::rpc
```

- [ ] **Step 5: Add `src/client.cpp` to `atx-rpc/CMakeLists.txt`**

```cmake
add_library(atx-rpc ${ATX_LIB_TYPE}
    src/limits.cpp
    src/byte_stream.cpp
    src/frame.cpp
    src/socket.cpp
    src/method_table.cpp
    src/dispatcher.cpp
    src/server.cpp
    src/client.cpp
)
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset server --target atx-rpc-tests`
Then: `ctest --preset server -R "^RpcClient\." --output-on-failure`
Expected: 7 tests, all PASS.

- [ ] **Step 7: Run the whole rpc suite repeatedly**

Run: `ctest --preset server -L atx_server --repeat until-fail:3 --output-on-failure`
Expected: all PASS on every repetition.

- [ ] **Step 8: Commit**

```bash
git add atx-rpc/include/atx/rpc/client.hpp atx-rpc/src/client.cpp \
        atx-rpc/tests/client_test.cpp atx-rpc/CMakeLists.txt
git commit -m "feat(rpc): add the client and close the wire end to end

connect() performs the handshake and captures the server's build id and ATXVSA
schema hash, so a client can detect archive-layout skew at connect time rather
than on its first GetSurfaceBlob.

call<Req,Resp>() is typed at the call site and hides framing entirely. Wire
status maps onto the core error domain so callers use one vocabulary: BUSY,
UNAVAILABLE, and DEADLINE_EXCEEDED all become Unavailable because all three mean
retry with backoff, and a caller that needs to tell them apart can read the
message. An incident id, when present, is appended so a user-visible failure can
be matched to a server log line.

A mismatched correlation id marks the client disconnected rather than retrying.
Requests are issued strictly serially on one connection, so a mismatch means the
stream has desynchronized and nothing further on it can be trusted.

call() is documented as not thread-safe: two threads sharing one client would
interleave frames on one stream. The concurrency test therefore gives each
thread its own client, and asserts eight clients times ten calls all come back
with their own payloads.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: `atx-proto` surface and admin messages

**Files:**
- Create: `atx-proto/atx/rpc/v1/surface.proto`
- Create: `atx-proto/atx/rpc/v1/surface_service.proto`
- Create: `atx-proto/atx/rpc/v1/admin.proto`
- Modify: `atx-proto/CMakeLists.txt`
- Create: `atx-rpc/tests/surface_message_test.cpp`

**Interfaces:**
- Consumes: `keys.proto`, `common.proto` (Task 2).
- Produces (all in `atx::rpc::v1`): `SymbolFitConfig`, `ExpirySummary`, `VolCurvePoint`, `VolQuotePoint`, `SurfaceDiagnostics`, `VolCurveSlice`, `SurfaceMeta`, `SurfaceBlob`, `CoverageCell`, `CoverageState`; request/response pairs `ListDatabasesRequest/Response`, `ListSymbolsRequest/Response`, `ListPartitionsRequest/Response`, `ListSurfacesRequest/Response`, `GetCoverageRequest/Response`, `GetSymbolConfigRequest/Response`, `GetSurfaceMetaRequest`, `GetCurveRequest`, `GetSurfaceBlobRequest`; admin types `HealthRequest/Response`, `GetServerInfoRequest`, `ServerInfo`, `GetStatsRequest`, `ServerStats`, `RegisterDatabaseRequest/Response`, `ExportRealmRequest`, `RealmConfig`, `RealmEntry`, `DatabaseInfo`.

- [ ] **Step 1: Write the failing test**

`atx-rpc/tests/surface_message_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "atx/rpc/v1/admin.pb.h"
#include "atx/rpc/v1/surface.pb.h"
#include "atx/rpc/v1/surface_service.pb.h"

namespace v1 = atx::rpc::v1;

namespace {

TEST(SurfaceMessages, CurveSliceRoundTrips) {
  v1::VolCurveSlice slice;
  slice.set_symbol("SPY");
  slice.set_expiry_iso("2024-06-21");
  slice.set_spot(512.30);
  slice.set_forward(513.11);
  slice.set_years(0.25);
  slice.set_atm_vol(0.1734);
  auto *point = slice.add_curve();
  point->set_z(-0.5);
  point->set_strike(490.0);
  point->set_model_iv(0.201);
  auto *quote = slice.add_quotes();
  quote->set_strike(490.0);
  quote->set_side("P");
  quote->set_mid_iv(0.2005);
  quote->set_delta(-0.31);

  std::string wire;
  ASSERT_TRUE(slice.SerializeToString(&wire));
  v1::VolCurveSlice parsed;
  ASSERT_TRUE(parsed.ParseFromString(wire));
  EXPECT_EQ(parsed.symbol(), "SPY");
  ASSERT_EQ(parsed.curve_size(), 1);
  EXPECT_DOUBLE_EQ(parsed.curve(0).strike(), 490.0);
  ASSERT_EQ(parsed.quotes_size(), 1);
  EXPECT_EQ(parsed.quotes(0).side(), "P");
}

TEST(SurfaceMessages, BlobCarriesBothHashes) {
  v1::SurfaceBlob blob;
  blob.set_db_id("spy_prod");
  blob.set_partition_key("2024-06-14");
  blob.set_symbol("SPY");
  blob.set_atxvsa_schema_hash(0x1111'2222'3333'4444ULL);
  blob.set_bytes("\x00\x01\x02", 3);
  blob.mutable_meta()->set_db_generation(9);
  blob.mutable_meta()->set_content_hash(0x5555ULL);

  std::string wire;
  ASSERT_TRUE(blob.SerializeToString(&wire));
  v1::SurfaceBlob parsed;
  ASSERT_TRUE(parsed.ParseFromString(wire));
  EXPECT_EQ(parsed.atxvsa_schema_hash(), 0x1111'2222'3333'4444ULL);
  EXPECT_EQ(parsed.meta().content_hash(), 0x5555ULL);
  EXPECT_EQ(parsed.bytes().size(), 3u);
}

TEST(SurfaceMessages, CoverageStateEnumeratorsArePinned) {
  EXPECT_EQ(v1::COVERAGE_STATE_ABSENT, 0);
  EXPECT_EQ(v1::COVERAGE_STATE_CONFIGURED_NOT_FITTED, 1);
  EXPECT_EQ(v1::COVERAGE_STATE_FITTED, 2);
}

// No request or response message may carry a filesystem path. RegisterDatabase
// is the sole exception, and it is loopback-only administration.
TEST(SurfaceMessages, NoSurfaceServiceMessageExposesAPath) {
  const auto *pool = v1::GetCurveRequest::descriptor()->file()->pool();
  for (const std::string &name :
       {std::string{"atx.rpc.v1.ListDatabasesResponse"},
        std::string{"atx.rpc.v1.ListPartitionsResponse"},
        std::string{"atx.rpc.v1.ListSurfacesResponse"},
        std::string{"atx.rpc.v1.GetCoverageResponse"},
        std::string{"atx.rpc.v1.SurfaceMeta"}, std::string{"atx.rpc.v1.SurfaceBlob"},
        std::string{"atx.rpc.v1.DatabaseInfo"}}) {
    const auto *descriptor = pool->FindMessageTypeByName(name);
    ASSERT_NE(descriptor, nullptr) << name;
    for (int i = 0; i < descriptor->field_count(); ++i) {
      const std::string field = descriptor->field(i)->name();
      EXPECT_EQ(field.find("path"), std::string::npos) << name << "." << field;
      EXPECT_EQ(field.find("root"), std::string::npos) << name << "." << field;
      EXPECT_EQ(field.find("directory"), std::string::npos) << name << "." << field;
    }
  }
}

TEST(AdminMessages, RealmConfigRoundTrips) {
  v1::RealmConfig config;
  config.set_realm_id("prod");
  auto *entry = config.add_entries();
  entry->set_db_id("spy_prod");
  entry->set_kind("surface_db");
  entry->set_root("C:/atx/db/spy");

  std::string wire;
  ASSERT_TRUE(config.SerializeToString(&wire));
  v1::RealmConfig parsed;
  ASSERT_TRUE(parsed.ParseFromString(wire));
  ASSERT_EQ(parsed.entries_size(), 1);
  EXPECT_EQ(parsed.entries(0).db_id(), "spy_prod");
  EXPECT_EQ(parsed.entries(0).root(), "C:/atx/db/spy");
}

} // namespace
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset server --target atx-rpc-tests`
Expected: FAIL at compile — `atx/rpc/v1/surface.pb.h: No such file or directory`.

- [ ] **Step 3: Write `atx-proto/atx/rpc/v1/surface.proto`**

```protobuf
syntax = "proto3";

package atx.rpc.v1;

import "atx/rpc/v1/common.proto";
import "atx/rpc/v1/keys.proto";

// Mirrors atx::vol::SymbolFitConfig as stored in the SurfaceDb manifest. Read
// only: how a stored surface was fit is server data, not a client control.
message SymbolFitConfig {
  string symbol      = 1;
  bool   enabled     = 2;
  uint32 preset      = 3;
  uint32 curve_kind  = 4;
  uint32 flags       = 5;  // the kDbSym* bitset
}

message ExpirySummary {
  string expiry_iso       = 1;
  double years            = 2;
  double forward          = 3;
  double atm_vol          = 4;
  double carry            = 5;
  double total_variance   = 6;
  double forward_variance = 7;
  uint32 strike_count     = 8;
}

message VolCurvePoint {
  double z        = 1;  // normalized strike
  double strike   = 2;
  double model_iv = 3;
}

message VolQuotePoint {
  double z                 = 1;
  double strike            = 2;
  string side              = 3;  // "C" or "P"
  double bid_price         = 4;
  double mid_price         = 5;
  double ask_price         = 6;
  double theoretical_price = 7;
  double bid_iv            = 8;
  double ask_iv            = 9;
  double mid_iv            = 10;
  double model_iv          = 11;
  double delta             = 12;
  double gamma             = 13;
  double theta             = 14;
  double vega              = 15;
}

message SurfaceDiagnostics {
  double worst_in_band          = 1;
  double mean_in_band           = 2;
  double mean_rmse_vol          = 3;
  uint32 fitted_slices          = 4;
  uint32 fitted_quotes          = 5;
  uint32 calendar_violations    = 6;
  bool   calendar_arb_free      = 7;
  string risk_state             = 8;
  // Display only. The fit that produced a stored surface already happened; this
  // reports it rather than selecting it.
  string quality_mode           = 9;
  string risk_model             = 10;
  string mark_model             = 11;
  bool   carry_confident        = 12;
  bool   inversion_certified    = 13;
  uint32 butterfly_violations   = 14;
  uint32 inversion_fallbacks    = 15;
  double carry_dispersion       = 16;
  double carry_leave_one_out    = 17;
}

message VolCurveSlice {
  string                symbol           = 1;
  string                partition_key    = 2;
  string                expiry_iso       = 3;
  string                model_name       = 4;
  double                spot             = 5;
  double                forward          = 6;
  double                years            = 7;
  double                rate             = 8;
  double                carry            = 9;
  double                atm_vol          = 10;
  double                fraction_in_band = 11;
  double                rmse_iv          = 12;
  double                max_abs_error    = 13;
  uint32                observations     = 14;
  repeated VolCurvePoint curve             = 15;
  repeated VolCurvePoint market_mark_curve = 16;
  repeated VolQuotePoint quotes            = 17;
  ResponseMeta          meta             = 18;
}

message SurfaceMeta {
  SurfaceKey              key         = 1;
  double                  spot        = 2;
  uint32                  model_kind  = 3;
  uint32                  risk_state  = 4;
  repeated ExpirySummary  expiries    = 5;
  SurfaceDiagnostics      diagnostics = 6;
  ResponseMeta            meta        = 7;
}

// A single-symbol ATXVSA v2 archive. Never a whole partition.
message SurfaceBlob {
  string       db_id              = 1;
  string       partition_key      = 2;
  string       symbol             = 3;
  // Checked against the client's build. A mismatch is FAILED_PRECONDITION
  // carrying both hashes, never a silent mis-read of the bytes.
  uint64       atxvsa_schema_hash = 4;
  bytes        bytes              = 5;
  ResponseMeta meta               = 6;
}

enum CoverageState {
  COVERAGE_STATE_ABSENT                = 0;
  // Configured in the manifest symbol table but never written into a
  // partition. The data-quality question the surface-database work keeps
  // running into, answerable only because the catalog indexes both.
  COVERAGE_STATE_CONFIGURED_NOT_FITTED = 1;
  COVERAGE_STATE_FITTED                = 2;
}

message CoverageCell {
  string        symbol        = 1;
  string        partition_key = 2;
  CoverageState state         = 3;
  uint32        expiry_count  = 4;
}
```

- [ ] **Step 4: Write `atx-proto/atx/rpc/v1/surface_service.proto`**

```protobuf
syntax = "proto3";

package atx.rpc.v1;

import "atx/rpc/v1/common.proto";
import "atx/rpc/v1/keys.proto";
import "atx/rpc/v1/surface.proto";

// Request/response pairs for SurfaceService. There is no `service` block: with
// no gRPC plugin it would generate nothing. Method strings live in
// atx-server/include/atx/server/methods.hpp and are checked against this file
// by the agreement test.

message DatabaseInfo {
  string db_id           = 1;
  string kind            = 2;  // "surface_db" in v1
  uint64 generation      = 3;
  uint32 symbol_count    = 4;
  uint32 partition_count = 5;
  // No root, no path. A db_id is opaque and the realm is the only path holder.
}

message ListDatabasesRequest {}

message ListDatabasesResponse {
  repeated DatabaseInfo databases = 1;  // only ids this token may read
  ResponseMeta          meta      = 2;
}

message ListSymbolsRequest {
  string db_id  = 1;
  string prefix = 2;  // optional filter
  Page   page   = 3;
}

message ListSymbolsResponse {
  repeated SymbolFitConfig symbols = 1;
  uint32                   total   = 2;
  ResponseMeta             meta    = 3;
}

message ListPartitionsRequest {
  string db_id     = 1;
  string key_begin = 2;  // inclusive; empty = unbounded
  string key_end   = 3;  // exclusive; empty = unbounded
  Page   page      = 4;
}

message PartitionInfo {
  string key           = 1;
  uint32 surface_count = 2;
  uint64 file_size     = 3;
  int64  created_ns    = 4;
}

message ListPartitionsResponse {
  repeated PartitionInfo partitions = 1;
  uint32                 total      = 2;
  ResponseMeta           meta       = 3;
}

message ListSurfacesRequest {
  string db_id     = 1;
  string symbol    = 2;  // optional
  string key_begin = 3;
  string key_end   = 4;
  Page   page      = 5;
}

message SurfaceSummary {
  string symbol        = 1;
  string partition_key = 2;
  uint32 expiry_count  = 3;
  double spot          = 4;
  uint32 model_kind    = 5;
  uint32 risk_state    = 6;
}

message ListSurfacesResponse {
  repeated SurfaceSummary surfaces = 1;
  uint32                  total    = 2;
  ResponseMeta            meta     = 3;
}

message GetCoverageRequest {
  string          db_id     = 1;
  repeated string symbols   = 2;  // empty = every symbol in the database
  string          key_begin = 3;
  string          key_end   = 4;
}

message GetCoverageResponse {
  repeated string       symbols        = 1;  // row order
  repeated string       partition_keys = 2;  // column order
  repeated CoverageCell cells          = 3;  // sparse; ABSENT cells are omitted
  ResponseMeta          meta           = 4;
}

message GetSymbolConfigRequest {
  string db_id  = 1;
  string symbol = 2;
}

message GetSymbolConfigResponse {
  SymbolFitConfig config = 1;
  ResponseMeta    meta   = 2;
}

message GetSurfaceMetaRequest {
  SurfaceKey key = 1;
}

message GetCurveRequest {
  SurfaceKey key             = 1;
  string     expiry_iso      = 2;
  // 0 uses the server default (2.0). Bounded server-side so a client cannot
  // request an unbounded strike expansion.
  double     z_window        = 3;
  uint32     curve_points    = 4;  // 0 = server default
  bool       include_quotes  = 5;
}

message GetSurfaceBlobRequest {
  SurfaceKey key = 1;
  // The client's ATXVSA schema hash. A mismatch is answered with
  // FAILED_PRECONDITION carrying both values rather than bytes the client would
  // mis-read.
  uint64 client_atxvsa_schema_hash = 2;
}
```

- [ ] **Step 5: Write `atx-proto/atx/rpc/v1/admin.proto`**

```protobuf
syntax = "proto3";

package atx.rpc.v1;

import "atx/rpc/v1/common.proto";

message HealthRequest {}

message HealthResponse {
  bool   serving   = 1;
  int64  uptime_ns = 2;
}

message GetServerInfoRequest {}

message ServerInfo {
  string version              = 1;
  string realm_id             = 2;
  int64  uptime_ns            = 3;
  uint64 manifest_schema_hash = 4;
  uint64 atxvsa_schema_hash   = 5;
  string server_uuid          = 6;
}

message GetStatsRequest {}

message DatabaseCacheStats {
  string db_id                    = 1;
  uint32 partition_cache_resident = 2;
  uint32 partition_cache_capacity = 3;
  uint64 generation               = 4;
}

message ServerStats {
  uint64 accepted                = 1;
  uint64 refused_max_connections = 2;
  uint64 rejected_busy           = 3;
  uint64 requests_served         = 4;
  uint32 live_connections        = 5;
  repeated DatabaseCacheStats databases = 6;
}

// The one place a filesystem path legitimately crosses the wire: registering a
// database IS the act of telling the server where data lives. Loopback-only,
// like every operation in v1.
message RegisterDatabaseRequest {
  string db_id = 1;
  string kind  = 2;  // "surface_db" in v1
  string root  = 3;
}

message RegisterDatabaseResponse {
  bool         registered = 1;
  uint64       generation = 2;
  ResponseMeta meta       = 3;
}

message ExportRealmRequest {}

message RealmEntry {
  string db_id    = 1;
  string kind     = 2;
  string root     = 3;
  int64  added_ns = 4;
}

// Doubles as the --realm-import file format, parsed with
// google::protobuf::util::JsonStringToMessage. This is why the server needs no
// JSON dependency.
message RealmConfig {
  string              realm_id = 1;
  repeated RealmEntry entries  = 2;
}
```

- [ ] **Step 6: Extend `ATX_PROTO_FILES`**

```cmake
set(ATX_PROTO_FILES
    atx/rpc/v1/envelope.proto
    atx/rpc/v1/keys.proto
    atx/rpc/v1/common.proto
    atx/rpc/v1/surface.proto
    atx/rpc/v1/surface_service.proto
    atx/rpc/v1/admin.proto
)
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset server --target atx-rpc-tests`
Then: `ctest --preset server -R "^SurfaceMessages\.|^AdminMessages\." --output-on-failure`
Expected: 5 tests, all PASS.

- [ ] **Step 8: Commit**

```bash
git add atx-proto atx-rpc/tests/surface_message_test.cpp
git commit -m "feat(proto): define surface, coverage, and admin messages

The nine SurfaceService request/response pairs plus the five AdminService ones.
No service blocks: with no gRPC plugin they generate nothing, so method strings
live in atx-server and an agreement test checks the two against each other.

A descriptor-walking test asserts no SurfaceService message has a field whose
name contains path, root, or directory. RegisterDatabaseRequest is the single
deliberate exception -- registering a database IS the act of telling the server
where data lives -- and it is loopback-only administration like everything else
in v1.

CoverageState distinguishes ABSENT from CONFIGURED_NOT_FITTED. That distinction
is the data-quality question the surface-database work keeps running into, and
it is answerable only because the catalog indexes the manifest symbol table and
the partition contents separately.

SurfaceBlob carries the archive schema hash so a client built against a
different layout gets FAILED_PRECONDITION with both values rather than bytes it
would mis-read.

SurfaceDiagnostics.quality_mode is display-only. The fit that produced a stored
surface already happened; reporting it is not the same as selecting it, and the
UI spec removes the corresponding control.

RealmConfig doubles as the --realm-import file format via
JsonStringToMessage, which is why the server needs no JSON dependency.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 9: `atx-server` config and realm

**Files:**
- Create: `atx-server/include/atx/server/config.hpp`, `atx-server/src/config.cpp`
- Create: `atx-server/include/atx/server/realm.hpp`, `atx-server/src/realm.cpp`
- Create: `atx-server/tests/config_test.cpp`, `atx-server/tests/realm_test.cpp`
- Modify: `atx-server/CMakeLists.txt`

**Interfaces:**
- Consumes: `atx::rpc::is_loopback` (Task 4), `v1::RealmConfig` (Task 8).
- Produces:
  - `struct atx::server::ServerConfig { std::string listen_host{"127.0.0.1"}; std::uint16_t listen_port{50051}; std::string state_path; std::string realm_import_path; std::size_t worker_count{0}; std::size_t max_connections{256}; std::size_t ready_queue_depth{1024}; std::size_t max_blob_bytes{16u<<20}; std::size_t max_frame_bytes{64u<<20}; std::size_t partition_cache_capacity{16}; std::chrono::milliseconds refresh_interval{5000}; std::chrono::milliseconds recv_timeout{30000}; std::chrono::milliseconds send_timeout{30000}; bool print_help{false}; bool print_version{false}; }`
  - `atx::core::Result<ServerConfig> atx::server::parse_args(int argc, const char* const* argv)` — returns `PermissionDenied` for a non-loopback `--listen`.
  - `std::string atx::server::usage_text()`
  - `struct atx::server::RealmEntry { std::string db_id; std::string kind; std::string root; std::int64_t added_ns; }`
  - `class atx::server::Realm` with `Status add(RealmEntry)`, `Result<RealmEntry> find(std::string_view db_id) const`, `std::vector<RealmEntry> entries() const`, `bool contains(std::string_view) const`, `std::size_t size() const`, `static Result<Realm> from_config(const v1::RealmConfig&)`, `v1::RealmConfig to_config(std::string_view realm_id) const`, `static Result<v1::RealmConfig> parse_json(std::string_view json)`.

- [ ] **Step 1: Write the failing config test**

`atx-server/tests/config_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/server/config.hpp"

namespace {

using atx::core::ErrorCode;
using atx::server::parse_args;

atx::core::Result<atx::server::ServerConfig> parse(std::vector<const char *> args) {
  args.insert(args.begin(), "atx-server");
  return parse_args(static_cast<int>(args.size()), args.data());
}

TEST(ServerConfig, DefaultsToLoopbackPort50051) {
  auto config = parse({});
  ASSERT_TRUE(config) << config.error().to_string();
  EXPECT_EQ(config->listen_host, "127.0.0.1");
  EXPECT_EQ(config->listen_port, 50051u);
  EXPECT_EQ(config->max_blob_bytes, 16u * 1024u * 1024u);
  EXPECT_EQ(config->max_frame_bytes, 64u * 1024u * 1024u);
}

TEST(ServerConfig, AcceptsAnExplicitLoopbackListen) {
  auto config = parse({"--listen", "127.0.0.1:9000"});
  ASSERT_TRUE(config);
  EXPECT_EQ(config->listen_host, "127.0.0.1");
  EXPECT_EQ(config->listen_port, 9000u);
}

TEST(ServerConfig, AcceptsLocalhostAndAnEphemeralPort) {
  auto config = parse({"--listen", "localhost:0"});
  ASSERT_TRUE(config);
  EXPECT_EQ(config->listen_host, "localhost");
  EXPECT_EQ(config->listen_port, 0u);
}

// The hard gate. This build has no TLS, so a remote bind would ship plaintext
// auth tokens; startup must refuse rather than warn.
TEST(ServerConfig, RefusesAWildcardListen) {
  auto config = parse({"--listen", "0.0.0.0:50051"});
  ASSERT_FALSE(config);
  EXPECT_EQ(config.error().code(), ErrorCode::PermissionDenied);
  EXPECT_NE(config.error().message().find("loopback"), std::string::npos);
  // The diagnostic must say WHY, not just refuse.
  EXPECT_NE(config.error().message().find("encryption"), std::string::npos);
}

TEST(ServerConfig, RefusesARoutableListen) {
  for (const char *addr : {"192.168.1.10:50051", "10.0.0.1:50051", "8.8.8.8:50051",
                           "::" , "10.127.0.1:50051"}) {
    auto config = parse({"--listen", addr});
    ASSERT_FALSE(config) << addr;
    EXPECT_EQ(config.error().code(), ErrorCode::PermissionDenied) << addr;
  }
}

TEST(ServerConfig, ParsesLimitsAndPaths) {
  auto config = parse({"--state", "C:/tmp/state.db", "--realm-import", "C:/tmp/realm.json",
                       "--workers", "4", "--max-connections", "32", "--max-blob-mb", "8",
                       "--refresh-ms", "1000"});
  ASSERT_TRUE(config) << config.error().to_string();
  EXPECT_EQ(config->state_path, "C:/tmp/state.db");
  EXPECT_EQ(config->realm_import_path, "C:/tmp/realm.json");
  EXPECT_EQ(config->worker_count, 4u);
  EXPECT_EQ(config->max_connections, 32u);
  EXPECT_EQ(config->max_blob_bytes, 8u * 1024u * 1024u);
  EXPECT_EQ(config->refresh_interval.count(), 1000);
}

TEST(ServerConfig, RejectsUnknownFlags) {
  auto config = parse({"--nope"});
  ASSERT_FALSE(config);
  EXPECT_EQ(config.error().code(), ErrorCode::InvalidArgument);
}

TEST(ServerConfig, RejectsAFlagMissingItsValue) {
  auto config = parse({"--listen"});
  ASSERT_FALSE(config);
  EXPECT_EQ(config.error().code(), ErrorCode::InvalidArgument);
}

TEST(ServerConfig, RejectsAMalformedListen) {
  for (const char *addr : {"127.0.0.1", "127.0.0.1:", ":50051", "127.0.0.1:abc",
                           "127.0.0.1:99999"}) {
    auto config = parse({"--listen", addr});
    ASSERT_FALSE(config) << addr;
    EXPECT_EQ(config.error().code(), ErrorCode::InvalidArgument) << addr;
  }
}

// max_blob_bytes must stay under max_frame_bytes or an in-range blob would be
// rejected by the framer instead of by the blob limit, with a confusing error.
TEST(ServerConfig, RejectsABlobLimitAtOrAboveTheFrameLimit) {
  auto config = parse({"--max-blob-mb", "64"});
  ASSERT_FALSE(config);
  EXPECT_EQ(config.error().code(), ErrorCode::InvalidArgument);
}

TEST(ServerConfig, HelpAndVersionShortCircuit) {
  auto help = parse({"--help"});
  ASSERT_TRUE(help);
  EXPECT_TRUE(help->print_help);

  auto version = parse({"--version"});
  ASSERT_TRUE(version);
  EXPECT_TRUE(version->print_version);
}

TEST(UsageText, MentionsTheLoopbackRestriction) {
  const std::string usage = atx::server::usage_text();
  EXPECT_NE(usage.find("--listen"), std::string::npos);
  EXPECT_NE(usage.find("loopback"), std::string::npos);
}

} // namespace
```

- [ ] **Step 2: Write the failing realm test**

`atx-server/tests/realm_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "atx/core/error.hpp"
#include "atx/server/realm.hpp"

namespace {

using atx::core::ErrorCode;
using atx::server::Realm;
using atx::server::RealmEntry;
namespace v1 = atx::rpc::v1;

RealmEntry entry(std::string db_id, std::string root) {
  RealmEntry e;
  e.db_id = std::move(db_id);
  e.kind = "surface_db";
  e.root = std::move(root);
  e.added_ns = 1;
  return e;
}

TEST(Realm, AddsAndFindsByDbId) {
  Realm realm;
  ASSERT_TRUE(realm.add(entry("spy_prod", "C:/atx/db/spy")));
  ASSERT_TRUE(realm.contains("spy_prod"));
  auto found = realm.find("spy_prod");
  ASSERT_TRUE(found);
  EXPECT_EQ(found->root, "C:/atx/db/spy");
  EXPECT_EQ(realm.size(), 1u);
}

TEST(Realm, MissingDbIdIsNotFound) {
  const Realm realm;
  auto found = realm.find("nope");
  ASSERT_FALSE(found);
  EXPECT_EQ(found.error().code(), ErrorCode::NotFound);
}

TEST(Realm, RejectsADuplicateDbId) {
  Realm realm;
  ASSERT_TRUE(realm.add(entry("spy_prod", "C:/a")));
  const auto second = realm.add(entry("spy_prod", "C:/b"));
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().code(), ErrorCode::AlreadyExists);
  // The first registration must survive the rejected one.
  auto found = realm.find("spy_prod");
  ASSERT_TRUE(found);
  EXPECT_EQ(found->root, "C:/a");
}

TEST(Realm, RejectsAnEmptyDbIdOrRoot) {
  Realm realm;
  EXPECT_FALSE(realm.add(entry("", "C:/a")));
  EXPECT_FALSE(realm.add(entry("x", "")));
}

// v1 serves surface databases only. The closed domain is enforced here and by a
// CHECK constraint in the catalog, so a typo cannot register an unusable kind.
TEST(Realm, RejectsAnUnknownKind) {
  Realm realm;
  RealmEntry e = entry("runs", "C:/atx/runs");
  e.kind = "run_archive";
  const auto added = realm.add(e);
  ASSERT_FALSE(added);
  EXPECT_EQ(added.error().code(), ErrorCode::InvalidArgument);
}

TEST(Realm, RoundTripsThroughRealmConfig) {
  Realm realm;
  ASSERT_TRUE(realm.add(entry("spy_prod", "C:/atx/db/spy")));
  ASSERT_TRUE(realm.add(entry("sp100_dev", "C:/atx/db/sp100")));

  const v1::RealmConfig config = realm.to_config("prod");
  EXPECT_EQ(config.realm_id(), "prod");
  ASSERT_EQ(config.entries_size(), 2);

  auto restored = Realm::from_config(config);
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored->size(), 2u);
  auto found = restored->find("sp100_dev");
  ASSERT_TRUE(found);
  EXPECT_EQ(found->root, "C:/atx/db/sp100");
}

TEST(Realm, ParsesJsonRealmConfig) {
  const std::string json = R"({
    "realmId": "prod",
    "entries": [
      {"dbId": "spy_prod", "kind": "surface_db", "root": "C:/atx/db/spy"}
    ]
  })";
  auto config = Realm::parse_json(json);
  ASSERT_TRUE(config) << config.error().to_string();
  EXPECT_EQ(config->realm_id(), "prod");
  ASSERT_EQ(config->entries_size(), 1);
  EXPECT_EQ(config->entries(0).db_id(), "spy_prod");
}

TEST(Realm, RejectsMalformedJson) {
  auto config = Realm::parse_json("{ this is not json ");
  ASSERT_FALSE(config);
  EXPECT_EQ(config.error().code(), ErrorCode::ParseError);
}

TEST(Realm, EntriesAreSortedByDbIdForStableOutput) {
  Realm realm;
  ASSERT_TRUE(realm.add(entry("zeta", "C:/z")));
  ASSERT_TRUE(realm.add(entry("alpha", "C:/a")));
  const auto all = realm.entries();
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].db_id, "alpha");
  EXPECT_EQ(all[1].db_id, "zeta");
}

} // namespace
```

- [ ] **Step 3: Run both tests to verify they fail**

Run: `cmake --build --preset server --target atx-server-tests`
Expected: FAIL at compile — `atx/server/config.hpp: No such file or directory`.

- [ ] **Step 4: Write `atx-server/include/atx/server/config.hpp`**

```cpp
#pragma once

// ServerConfig — everything the daemon needs, and the argv parser that
// enforces the loopback bind restriction before a socket is ever created.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "atx/core/error.hpp"

namespace atx::server {

struct ServerConfig {
  // Loopback only. parse_args refuses anything else; RpcServer::start refuses
  // again at bind time. Two gates because this is the one mistake that turns a
  // local tool into an unauthenticated public data service.
  std::string listen_host{"127.0.0.1"};
  std::uint16_t listen_port{50051};

  // Where the authoritative state database lives (realm, tokens,
  // entitlements). Empty means "<cwd>/atx-server-state.db".
  std::string state_path;
  // Optional one-shot realm import, applied at startup.
  std::string realm_import_path;

  std::size_t worker_count{0}; // 0 = hardware_concurrency
  std::size_t max_connections{256};
  std::size_t ready_queue_depth{1024};
  // Must stay strictly below max_frame_bytes: otherwise an in-range blob would
  // be rejected by the framer with a confusing error instead of by the blob
  // limit with an actionable one.
  std::size_t max_blob_bytes{16u * 1024u * 1024u};
  std::size_t max_frame_bytes{64u * 1024u * 1024u};
  std::size_t partition_cache_capacity{16};

  std::chrono::milliseconds refresh_interval{5000};
  std::chrono::milliseconds recv_timeout{30000};
  std::chrono::milliseconds send_timeout{30000};

  bool print_help{false};
  bool print_version{false};
};

[[nodiscard]] std::string usage_text();

// PermissionDenied for a non-loopback --listen; InvalidArgument for anything
// malformed. Never partially applies a bad command line.
[[nodiscard]] atx::core::Result<ServerConfig> parse_args(int argc, const char *const *argv);

} // namespace atx::server
```

- [ ] **Step 5: Write `atx-server/src/config.cpp`**

```cpp
#include "atx/server/config.hpp"

#include <charconv>
#include <string_view>

#include "atx/rpc/socket.hpp"

namespace atx::server {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

[[nodiscard]] Result<std::uint64_t> parse_u64(std::string_view text) {
  std::uint64_t value = 0;
  const char *begin = text.data();
  const char *end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return Err(ErrorCode::InvalidArgument, "'" + std::string{text} + "' is not a number");
  }
  return Ok(value);
}

// "host:port". IPv6 literals must be bracketed, e.g. "[::1]:50051", so the
// colon split is unambiguous.
[[nodiscard]] atx::core::Status parse_listen(std::string_view text, ServerConfig &config) {
  std::string_view host;
  std::string_view port_text;

  if (!text.empty() && text.front() == '[') {
    const std::size_t close = text.find(']');
    if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':') {
      return Err(ErrorCode::InvalidArgument, "malformed bracketed address '" +
                                                 std::string{text} + "'; expected [host]:port");
    }
    host = text.substr(1, close - 1);
    port_text = text.substr(close + 2);
  } else {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
      return Err(ErrorCode::InvalidArgument,
                 "--listen '" + std::string{text} + "' is missing a port; expected host:port");
    }
    host = text.substr(0, colon);
    port_text = text.substr(colon + 1);
  }

  if (host.empty() || port_text.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "--listen '" + std::string{text} + "' must be host:port");
  }
  ATX_TRY(const std::uint64_t port, parse_u64(port_text));
  if (port > 65535) {
    return Err(ErrorCode::InvalidArgument, "port " + std::string{port_text} + " is out of range");
  }

  // The gate. Checked here so the process exits before a listening socket
  // exists, and checked again in RpcServer::start so a programmatic caller
  // cannot bypass it.
  if (!atx::rpc::is_loopback(host)) {
    return Err(ErrorCode::PermissionDenied,
               "refusing to listen on non-loopback address '" + std::string{host} +
                   "'. This build has no transport encryption, so a remote bind would "
                   "expose plaintext auth tokens and unencrypted market data. Use a "
                   "loopback address (127.0.0.1 or localhost).");
  }

  config.listen_host = std::string{host};
  config.listen_port = static_cast<std::uint16_t>(port);
  return Ok();
}

} // namespace

std::string usage_text() {
  return "atx-server - the ATX trading platform data daemon\n"
         "\n"
         "  --listen HOST:PORT       bind address (default 127.0.0.1:50051).\n"
         "                           LOOPBACK ONLY: this build has no transport\n"
         "                           encryption, so a non-loopback address is refused.\n"
         "  --state PATH             authoritative state database\n"
         "                           (default ./atx-server-state.db)\n"
         "  --realm-import PATH      import a RealmConfig JSON file at startup\n"
         "  --workers N              worker threads (default: hardware concurrency)\n"
         "  --max-connections N      concurrent connection cap (default 256)\n"
         "  --queue-depth N          ready-queue depth before BUSY (default 1024)\n"
         "  --max-blob-mb N          per-surface blob cap in MiB (default 16)\n"
         "  --cache-partitions N     partition mmap LRU bound per database (default 16)\n"
         "  --refresh-ms N           generation-drift poll interval (default 5000)\n"
         "  --recv-timeout-ms N      per-socket receive timeout (default 30000)\n"
         "  --send-timeout-ms N      per-socket send timeout (default 30000)\n"
         "  --version                print the version and exit\n"
         "  --help                   print this text and exit\n";
}

Result<ServerConfig> parse_args(int argc, const char *const *argv) {
  ServerConfig config;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      config.print_help = true;
      return Ok(config);
    }
    if (arg == "--version") {
      config.print_version = true;
      return Ok(config);
    }

    const auto next = [&](std::string_view flag) -> Result<std::string_view> {
      if (i + 1 >= argc) {
        return Err(ErrorCode::InvalidArgument, std::string{flag} + " requires a value");
      }
      return Ok(std::string_view{argv[++i]});
    };

    if (arg == "--listen") {
      ATX_TRY(const std::string_view value, next(arg));
      ATX_TRY_VOID(parse_listen(value, config));
    } else if (arg == "--state") {
      ATX_TRY(const std::string_view value, next(arg));
      config.state_path = std::string{value};
    } else if (arg == "--realm-import") {
      ATX_TRY(const std::string_view value, next(arg));
      config.realm_import_path = std::string{value};
    } else if (arg == "--workers") {
      ATX_TRY(const std::string_view value, next(arg));
      ATX_TRY(const std::uint64_t n, parse_u64(value));
      config.worker_count = static_cast<std::size_t>(n);
    } else if (arg == "--max-connections") {
      ATX_TRY(const std::string_view value, next(arg));
      ATX_TRY(const std::uint64_t n, parse_u64(value));
      if (n == 0) {
        return Err(ErrorCode::InvalidArgument, "--max-connections must be at least 1");
      }
      config.max_connections = static_cast<std::size_t>(n);
    } else if (arg == "--queue-depth") {
      ATX_TRY(const std::string_view value, next(arg));
      ATX_TRY(const std::uint64_t n, parse_u64(value));
      if (n == 0) {
        return Err(ErrorCode::InvalidArgument, "--queue-depth must be at least 1");
      }
      config.ready_queue_depth = static_cast<std::size_t>(n);
    } else if (arg == "--max-blob-mb") {
      ATX_TRY(const std::string_view value, next(arg));
      ATX_TRY(const std::uint64_t n, parse_u64(value));
      config.max_blob_bytes = static_cast<std::size_t>(n) * 1024u * 1024u;
    } else if (arg == "--cache-partitions") {
      ATX_TRY(const std::string_view value, next(arg));
      ATX_TRY(const std::uint64_t n, parse_u64(value));
      config.partition_cache_capacity = static_cast<std::size_t>(n == 0 ? 1 : n);
    } else if (arg == "--refresh-ms") {
      ATX_TRY(const std::string_view value, next(arg));
      ATX_TRY(const std::uint64_t n, parse_u64(value));
      config.refresh_interval = std::chrono::milliseconds{static_cast<std::int64_t>(n)};
    } else if (arg == "--recv-timeout-ms") {
      ATX_TRY(const std::string_view value, next(arg));
      ATX_TRY(const std::uint64_t n, parse_u64(value));
      config.recv_timeout = std::chrono::milliseconds{static_cast<std::int64_t>(n)};
    } else if (arg == "--send-timeout-ms") {
      ATX_TRY(const std::string_view value, next(arg));
      ATX_TRY(const std::uint64_t n, parse_u64(value));
      config.send_timeout = std::chrono::milliseconds{static_cast<std::int64_t>(n)};
    } else {
      return Err(ErrorCode::InvalidArgument, "unknown argument '" + std::string{arg} + "'");
    }
  }

  if (config.max_blob_bytes >= config.max_frame_bytes) {
    return Err(ErrorCode::InvalidArgument,
               "--max-blob-mb must stay below the frame limit (" +
                   std::to_string(config.max_frame_bytes / (1024 * 1024)) +
                   " MiB); otherwise an in-range blob is rejected by the framer rather "
                   "than by the blob limit, and the client sees the wrong error");
  }

  if (config.state_path.empty()) {
    config.state_path = "atx-server-state.db";
  }
  return Ok(config);
}

} // namespace atx::server
```

- [ ] **Step 6: Write `atx-server/include/atx/server/realm.hpp`**

```cpp
#pragma once

// Realm — the set of databases this server serves, and the ONLY place in the
// process where a filesystem root exists.
//
// A db_id is opaque to clients. Nothing outside this type may hold or hand out
// a root, which is what makes "no filesystem path crosses the wire" enforceable
// rather than a convention. It is also the entitlement boundary: a token grants
// access to db_ids, never to directories.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/rpc/v1/admin.pb.h"

namespace atx::server {

struct RealmEntry {
  std::string db_id;
  std::string kind{"surface_db"}; // v1 serves surface databases only
  std::string root;
  std::int64_t added_ns{0};
};

class Realm {
public:
  Realm() = default;

  // AlreadyExists on a duplicate db_id -- silently replacing a registration
  // would repoint every client of that id at different data.
  // InvalidArgument on an empty id or root, or on a kind other than
  // "surface_db".
  [[nodiscard]] atx::core::Status add(RealmEntry entry);

  [[nodiscard]] atx::core::Result<RealmEntry> find(std::string_view db_id) const;
  [[nodiscard]] bool contains(std::string_view db_id) const;
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  // Sorted by db_id so exports, logs, and tests are stable.
  [[nodiscard]] std::vector<RealmEntry> entries() const;

  [[nodiscard]] static atx::core::Result<Realm> from_config(const atx::rpc::v1::RealmConfig &config);
  [[nodiscard]] atx::rpc::v1::RealmConfig to_config(std::string_view realm_id) const;

  // Parses the --realm-import file with protobuf's JSON mapping. This is why
  // the server needs no JSON dependency.
  [[nodiscard]] static atx::core::Result<atx::rpc::v1::RealmConfig> parse_json(std::string_view json);

private:
  std::unordered_map<std::string, RealmEntry> entries_;
};

} // namespace atx::server
```

- [ ] **Step 7: Write `atx-server/src/realm.cpp`**

```cpp
#include "atx/server/realm.hpp"

#include <algorithm>

#include <google/protobuf/util/json_util.h>

namespace atx::server {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

} // namespace

atx::core::Status Realm::add(RealmEntry entry) {
  if (entry.db_id.empty()) {
    return Err(ErrorCode::InvalidArgument, "realm entry has an empty db_id");
  }
  if (entry.root.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "realm entry '" + entry.db_id + "' has an empty root");
  }
  // Closed domain, mirrored by a CHECK constraint in the catalog so a typo
  // cannot register a kind nothing can serve.
  if (entry.kind != "surface_db") {
    return Err(ErrorCode::InvalidArgument, "realm entry '" + entry.db_id + "' has kind '" +
                                               entry.kind +
                                               "'; v1 serves 'surface_db' only");
  }
  if (entries_.find(entry.db_id) != entries_.end()) {
    return Err(ErrorCode::AlreadyExists,
               "db_id '" + entry.db_id + "' is already registered");
  }
  const std::string key = entry.db_id;
  entries_.emplace(key, std::move(entry));
  return Ok();
}

atx::core::Result<RealmEntry> Realm::find(std::string_view db_id) const {
  const auto it = entries_.find(std::string{db_id});
  if (it == entries_.end()) {
    return Err(ErrorCode::NotFound, "no database registered as '" + std::string{db_id} + "'");
  }
  return Ok(it->second);
}

bool Realm::contains(std::string_view db_id) const {
  return entries_.find(std::string{db_id}) != entries_.end();
}

std::vector<RealmEntry> Realm::entries() const {
  std::vector<RealmEntry> out;
  out.reserve(entries_.size());
  for (const auto &[id, entry] : entries_) {
    out.push_back(entry);
  }
  std::sort(out.begin(), out.end(),
            [](const RealmEntry &a, const RealmEntry &b) { return a.db_id < b.db_id; });
  return out;
}

atx::core::Result<Realm> Realm::from_config(const atx::rpc::v1::RealmConfig &config) {
  Realm realm;
  for (const atx::rpc::v1::RealmEntry &entry : config.entries()) {
    RealmEntry converted;
    converted.db_id = entry.db_id();
    converted.kind = entry.kind().empty() ? std::string{"surface_db"} : entry.kind();
    converted.root = entry.root();
    converted.added_ns = entry.added_ns();
    ATX_TRY_VOID(realm.add(std::move(converted)));
  }
  return Ok(std::move(realm));
}

atx::rpc::v1::RealmConfig Realm::to_config(std::string_view realm_id) const {
  atx::rpc::v1::RealmConfig config;
  config.set_realm_id(std::string{realm_id});
  for (const RealmEntry &entry : entries()) {
    atx::rpc::v1::RealmEntry *out = config.add_entries();
    out->set_db_id(entry.db_id);
    out->set_kind(entry.kind);
    out->set_root(entry.root);
    out->set_added_ns(entry.added_ns);
  }
  return config;
}

atx::core::Result<atx::rpc::v1::RealmConfig> Realm::parse_json(std::string_view json) {
  atx::rpc::v1::RealmConfig config;
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = false;
  const auto status =
      google::protobuf::util::JsonStringToMessage(std::string{json}, &config, options);
  if (!status.ok()) {
    return Err(ErrorCode::ParseError,
               "realm JSON is not a RealmConfig: " + std::string{status.message()});
  }
  return Ok(std::move(config));
}

} // namespace atx::server
```

- [ ] **Step 8: Add the sources to `atx-server/CMakeLists.txt`**

```cmake
add_library(atx-server-lib ${ATX_LIB_TYPE}
    src/version.cpp
    src/config.cpp
    src/realm.cpp
)
```

- [ ] **Step 9: Run the tests to verify they pass**

Run: `cmake --build --preset server --target atx-server-tests`
Then: `ctest --preset server -R "^ServerConfig\.|^UsageText\.|^Realm\." --output-on-failure`
Expected: 19 tests, all PASS.

- [ ] **Step 10: Commit**

```bash
git add atx-server/include/atx/server/config.hpp atx-server/include/atx/server/realm.hpp \
        atx-server/src/config.cpp atx-server/src/realm.cpp \
        atx-server/tests/config_test.cpp atx-server/tests/realm_test.cpp \
        atx-server/CMakeLists.txt
git commit -m "feat(server): add ServerConfig with the loopback gate, and the Realm

parse_args refuses a non-loopback --listen with PermissionDenied before any
socket exists, and RpcServer::start refuses again at bind time. Two gates for
one mistake, because this is the mistake that turns a local tool into an
unauthenticated public data service. The diagnostic says why -- no transport
encryption -- rather than just refusing, so the operator is not left guessing.

The parser also rejects a blob limit at or above the frame limit. Without that
check an in-range blob would be refused by the framer instead of by the blob
limit, and the client would see the wrong error with no size in it.

Realm is the only place in the process where a filesystem root exists. Nothing
outside it may hold or hand out a root, which is what makes 'no filesystem path
crosses the wire' enforceable rather than a convention. It is also the
entitlement boundary: a token grants access to db_ids, never to directories.

add() rejects a duplicate db_id rather than replacing it. A silent replacement
would repoint every client of that id at different data.

Realm kind is a closed domain ('surface_db' in v1), mirrored by a CHECK
constraint in the catalog, so a typo cannot register a kind nothing can serve.

RealmConfig is parsed with protobuf's JSON mapping, which is why --realm-import
adds no JSON dependency.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 10: `atx-server` catalog — schema, state, tokens, entitlements

**Files:**
- Create: `atx-server/include/atx/server/catalog.hpp`, `atx-server/src/catalog.cpp`
- Create: `atx-server/include/atx/server/auth.hpp`, `atx-server/src/auth.cpp`
- Create: `atx-server/tests/catalog_test.cpp`, `atx-server/tests/auth_test.cpp`
- Modify: `atx-server/CMakeLists.txt`

**Interfaces:**
- Consumes: `atx::core::db::Database/Statement/Transaction`, `atx::core::sha256_hex`, `Realm`/`RealmEntry` (Task 9).
- Produces:
  - `class atx::server::Catalog`: `static Result<std::unique_ptr<Catalog>> open(std::string_view state_path)`, `Status upsert_realm_entry(const RealmEntry&)`, `Result<Realm> load_realm() const`, `Status put_token(std::string_view token, std::string label, std::vector<std::string> db_ids)`, `Status disable_token(std::string_view token)`, `Result<atx::rpc::Entitlements> resolve_token(std::string_view token) const`, `Result<std::string> server_uuid()`, plus the `main`-tier writers/readers used by Task 11: `Status replace_db_index(std::string_view db_id, const DbIndex&)`, `Result<std::vector<DbSummary>> list_databases() const`, `Result<std::vector<SymbolRow>> list_symbols(...) const`, `Result<std::vector<PartitionRow>> list_partitions(...) const`, `Result<std::vector<SurfaceRow>> list_surfaces(...) const`, `Result<std::vector<CoverageRow>> coverage(...) const`, `Result<std::uint64_t> generation_of(std::string_view db_id) const`, `Status snapshot_to(std::string_view path)`, `Status restore_from(std::string_view path)`, `Result<std::vector<std::string>> main_table_digest() const`.
  - Row structs `DbSummary`, `SymbolRow`, `PartitionRow`, `SurfaceRow`, `CoverageRow`, and the aggregate `DbIndex { std::uint64_t generation; std::vector<SymbolRow> symbols; std::vector<PartitionRow> partitions; std::vector<SurfaceRow> surfaces; }`.
  - `atx::core::Result<std::string> atx::server::token_digest_hex(std::string_view token)`.
  - `bool atx::server::constant_time_equals(std::string_view a, std::string_view b)`.

- [ ] **Step 1: Write the failing auth test**

`atx-server/tests/auth_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "atx/server/auth.hpp"

namespace {

using atx::server::constant_time_equals;
using atx::server::token_digest_hex;

TEST(TokenDigest, IsStableAndSixtyFourHexChars) {
  auto a = token_digest_hex("hunter2");
  auto b = token_digest_hex("hunter2");
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_EQ(*a, *b);
  EXPECT_EQ(a->size(), 64u);
  EXPECT_EQ(a->find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(TokenDigest, DiffersForDifferentTokens) {
  auto a = token_digest_hex("token-a");
  auto b = token_digest_hex("token-b");
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_NE(*a, *b);
}

// The stored form must never be the token itself. A catalog dump must not hand
// an attacker working credentials.
TEST(TokenDigest, DoesNotContainThePlaintextToken) {
  const std::string token = "super-secret-token";
  auto digest = token_digest_hex(token);
  ASSERT_TRUE(digest);
  EXPECT_EQ(digest->find(token), std::string::npos);
}

TEST(ConstantTimeEquals, MatchesOnlyIdenticalStrings) {
  EXPECT_TRUE(constant_time_equals("abc", "abc"));
  EXPECT_FALSE(constant_time_equals("abc", "abd"));
  EXPECT_FALSE(constant_time_equals("abc", "ab"));
  EXPECT_FALSE(constant_time_equals("", "a"));
  EXPECT_TRUE(constant_time_equals("", ""));
}

} // namespace
```

- [ ] **Step 2: Write the failing catalog test**

`atx-server/tests/catalog_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/server/catalog.hpp"
#include "atx/test/scratch.hpp"

namespace {

using atx::core::ErrorCode;
using atx::server::Catalog;
using atx::server::DbIndex;
using atx::server::PartitionRow;
using atx::server::RealmEntry;
using atx::server::SurfaceRow;
using atx::server::SymbolRow;

std::string scratch_state(const std::string &name) {
  const std::filesystem::path dir = atx::test::scratch_dir("atx-server-catalog");
  return (dir / name).string();
}

std::unique_ptr<Catalog> open_catalog(const std::string &name) {
  auto catalog = Catalog::open(scratch_state(name));
  EXPECT_TRUE(catalog) << (catalog ? "" : catalog.error().to_string());
  return catalog ? std::move(*catalog) : nullptr;
}

RealmEntry entry(std::string db_id, std::string root) {
  RealmEntry e;
  e.db_id = std::move(db_id);
  e.kind = "surface_db";
  e.root = std::move(root);
  e.added_ns = 42;
  return e;
}

DbIndex sample_index() {
  DbIndex index;
  index.generation = 7;

  SymbolRow spy;
  spy.symbol = "SPY";
  spy.enabled = true;
  spy.preset = 2;
  spy.curve_kind = 1;
  spy.flags = 0x0F;
  SymbolRow aapl;
  aapl.symbol = "AAPL";
  aapl.enabled = true;
  index.symbols = {spy, aapl};

  PartitionRow p1;
  p1.key = "2024-03-14";
  p1.surface_count = 2;
  p1.file_size = 4096;
  p1.created_ns = 100;
  PartitionRow p2;
  p2.key = "2024-03-15";
  p2.surface_count = 1;
  p2.file_size = 2048;
  p2.created_ns = 200;
  index.partitions = {p1, p2};

  SurfaceRow s1;
  s1.partition_key = "2024-03-14";
  s1.symbol = "SPY";
  s1.expiry_count = 18;
  s1.spot = 512.30;
  SurfaceRow s2;
  s2.partition_key = "2024-03-14";
  s2.symbol = "AAPL";
  s2.expiry_count = 14;
  s2.spot = 172.11;
  SurfaceRow s3;
  s3.partition_key = "2024-03-15";
  s3.symbol = "SPY";
  s3.expiry_count = 18;
  s3.spot = 513.00;
  index.surfaces = {s1, s2, s3};
  return index;
}

TEST(Catalog, OpensAndCreatesItsSchema) {
  auto catalog = open_catalog("open.db");
  ASSERT_NE(catalog, nullptr);
  auto uuid = catalog->server_uuid();
  ASSERT_TRUE(uuid);
  EXPECT_FALSE(uuid->empty());
}

TEST(Catalog, ServerUuidIsStableAcrossReopen) {
  const std::string path = scratch_state("uuid.db");
  std::string first;
  {
    auto catalog = Catalog::open(path);
    ASSERT_TRUE(catalog);
    auto uuid = (*catalog)->server_uuid();
    ASSERT_TRUE(uuid);
    first = *uuid;
  }
  auto reopened = Catalog::open(path);
  ASSERT_TRUE(reopened);
  auto second = (*reopened)->server_uuid();
  ASSERT_TRUE(second);
  EXPECT_EQ(*second, first);
}

TEST(Catalog, RealmSurvivesRestart) {
  const std::string path = scratch_state("realm.db");
  {
    auto catalog = Catalog::open(path);
    ASSERT_TRUE(catalog);
    ASSERT_TRUE((*catalog)->upsert_realm_entry(entry("spy_prod", "C:/atx/db/spy")));
  }
  auto reopened = Catalog::open(path);
  ASSERT_TRUE(reopened);
  auto realm = (*reopened)->load_realm();
  ASSERT_TRUE(realm);
  EXPECT_EQ(realm->size(), 1u);
  auto found = realm->find("spy_prod");
  ASSERT_TRUE(found);
  EXPECT_EQ(found->root, "C:/atx/db/spy");
}

// The state tier is authoritative and not reconstructible, so a bad kind must
// be impossible to write rather than merely discouraged.
TEST(Catalog, RejectsAnUnknownRealmKind) {
  auto catalog = open_catalog("kind.db");
  ASSERT_NE(catalog, nullptr);
  RealmEntry bad = entry("runs", "C:/atx/runs");
  bad.kind = "run_archive";
  EXPECT_FALSE(catalog->upsert_realm_entry(bad));
}

TEST(Catalog, ResolvesATokenToItsEntitlements) {
  auto catalog = open_catalog("token.db");
  ASSERT_NE(catalog, nullptr);
  ASSERT_TRUE(catalog->upsert_realm_entry(entry("spy_prod", "C:/a")));
  ASSERT_TRUE(catalog->upsert_realm_entry(entry("sp100_dev", "C:/b")));
  ASSERT_TRUE(catalog->put_token("desk-token", "desk", {"spy_prod"}));

  auto ent = catalog->resolve_token("desk-token");
  ASSERT_TRUE(ent) << ent.error().to_string();
  EXPECT_TRUE(ent->authenticated);
  EXPECT_EQ(ent->token_label, "desk");
  EXPECT_TRUE(ent->can_read("spy_prod"));
  EXPECT_FALSE(ent->can_read("sp100_dev"));
}

TEST(Catalog, UnknownTokenIsPermissionDenied) {
  auto catalog = open_catalog("unknown_token.db");
  ASSERT_NE(catalog, nullptr);
  auto ent = catalog->resolve_token("never-issued");
  ASSERT_FALSE(ent);
  EXPECT_EQ(ent.error().code(), ErrorCode::PermissionDenied);
}

TEST(Catalog, DisabledTokenIsRejected) {
  auto catalog = open_catalog("disabled_token.db");
  ASSERT_NE(catalog, nullptr);
  ASSERT_TRUE(catalog->upsert_realm_entry(entry("spy_prod", "C:/a")));
  ASSERT_TRUE(catalog->put_token("temp", "temp", {"spy_prod"}));
  ASSERT_TRUE(catalog->resolve_token("temp"));
  ASSERT_TRUE(catalog->disable_token("temp"));
  auto ent = catalog->resolve_token("temp");
  ASSERT_FALSE(ent);
  EXPECT_EQ(ent.error().code(), ErrorCode::PermissionDenied);
}

// A dump of the state database must not yield working credentials.
TEST(Catalog, NeverStoresThePlaintextToken) {
  const std::string path = scratch_state("plaintext.db");
  {
    auto catalog = Catalog::open(path);
    ASSERT_TRUE(catalog);
    ASSERT_TRUE((*catalog)->upsert_realm_entry(entry("spy_prod", "C:/a")));
    ASSERT_TRUE((*catalog)->put_token("PLAINTEXT-CANARY", "desk", {"spy_prod"}));
  }
  std::ifstream file{path, std::ios::binary};
  ASSERT_TRUE(file.is_open());
  const std::string contents{std::istreambuf_iterator<char>{file},
                             std::istreambuf_iterator<char>{}};
  EXPECT_EQ(contents.find("PLAINTEXT-CANARY"), std::string::npos);
}

TEST(Catalog, IndexesADatabaseAndAnswersCoverage) {
  auto catalog = open_catalog("index.db");
  ASSERT_NE(catalog, nullptr);
  ASSERT_TRUE(catalog->upsert_realm_entry(entry("spy_prod", "C:/a")));
  ASSERT_TRUE(catalog->replace_db_index("spy_prod", sample_index()));

  auto databases = catalog->list_databases();
  ASSERT_TRUE(databases);
  ASSERT_EQ(databases->size(), 1u);
  EXPECT_EQ((*databases)[0].db_id, "spy_prod");
  EXPECT_EQ((*databases)[0].generation, 7u);
  EXPECT_EQ((*databases)[0].symbol_count, 2u);
  EXPECT_EQ((*databases)[0].partition_count, 2u);

  auto surfaces = catalog->list_surfaces("spy_prod", "SPY", "", "", 0, 100);
  ASSERT_TRUE(surfaces);
  EXPECT_EQ(surfaces->size(), 2u); // SPY on both partitions

  auto coverage = catalog->coverage("spy_prod", {}, "", "");
  ASSERT_TRUE(coverage);
  // AAPL is configured but absent from 2024-03-15: exactly the data-quality
  // hole this table exists to expose.
  bool found_gap = false;
  for (const auto &cell : *coverage) {
    if (cell.symbol == "AAPL" && cell.partition_key == "2024-03-15") {
      EXPECT_FALSE(cell.fitted);
      EXPECT_TRUE(cell.configured);
      found_gap = true;
    }
  }
  EXPECT_TRUE(found_gap);
}

TEST(Catalog, ReindexingReplacesRatherThanAccumulates) {
  auto catalog = open_catalog("reindex.db");
  ASSERT_NE(catalog, nullptr);
  ASSERT_TRUE(catalog->upsert_realm_entry(entry("spy_prod", "C:/a")));
  ASSERT_TRUE(catalog->replace_db_index("spy_prod", sample_index()));

  DbIndex smaller;
  smaller.generation = 8;
  SymbolRow spy;
  spy.symbol = "SPY";
  spy.enabled = true;
  smaller.symbols = {spy};
  PartitionRow p;
  p.key = "2024-03-15";
  p.surface_count = 1;
  smaller.partitions = {p};
  SurfaceRow s;
  s.partition_key = "2024-03-15";
  s.symbol = "SPY";
  smaller.surfaces = {s};
  ASSERT_TRUE(catalog->replace_db_index("spy_prod", smaller));

  auto surfaces = catalog->list_surfaces("spy_prod", "", "", "", 0, 100);
  ASSERT_TRUE(surfaces);
  EXPECT_EQ(surfaces->size(), 1u) << "stale rows from the previous scan survived";
  auto generation = catalog->generation_of("spy_prod");
  ASSERT_TRUE(generation);
  EXPECT_EQ(*generation, 8u);
}

TEST(Catalog, ScanningTwiceIsIdempotent) {
  auto catalog = open_catalog("idempotent.db");
  ASSERT_NE(catalog, nullptr);
  ASSERT_TRUE(catalog->upsert_realm_entry(entry("spy_prod", "C:/a")));
  ASSERT_TRUE(catalog->replace_db_index("spy_prod", sample_index()));
  auto first = catalog->main_table_digest();
  ASSERT_TRUE(first);
  ASSERT_TRUE(catalog->replace_db_index("spy_prod", sample_index()));
  auto second = catalog->main_table_digest();
  ASSERT_TRUE(second);
  EXPECT_EQ(*first, *second);
}

TEST(Catalog, ListSymbolsHonoursPrefixAndPaging) {
  auto catalog = open_catalog("paging.db");
  ASSERT_NE(catalog, nullptr);
  ASSERT_TRUE(catalog->upsert_realm_entry(entry("spy_prod", "C:/a")));
  ASSERT_TRUE(catalog->replace_db_index("spy_prod", sample_index()));

  auto all = catalog->list_symbols("spy_prod", "", 0, 100);
  ASSERT_TRUE(all);
  EXPECT_EQ(all->size(), 2u);

  auto filtered = catalog->list_symbols("spy_prod", "SP", 0, 100);
  ASSERT_TRUE(filtered);
  ASSERT_EQ(filtered->size(), 1u);
  EXPECT_EQ((*filtered)[0].symbol, "SPY");

  auto paged = catalog->list_symbols("spy_prod", "", 1, 1);
  ASSERT_TRUE(paged);
  EXPECT_EQ(paged->size(), 1u);
}

} // namespace
```

Add `#include <fstream>` and `#include <iterator>` at the top of the file for the plaintext-canary test.

- [ ] **Step 3: Run both tests to verify they fail**

Run: `cmake --build --preset server --target atx-server-tests`
Expected: FAIL at compile — `atx/server/catalog.hpp: No such file or directory`.

- [ ] **Step 4: Write `atx-server/include/atx/server/auth.hpp` and `src/auth.cpp`**

```cpp
// include/atx/server/auth.hpp
#pragma once

#include <string>
#include <string_view>

#include "atx/core/error.hpp"

namespace atx::server {

// Only sha256(token) is ever stored. A dump of the state database must not hand
// an attacker working credentials.
[[nodiscard]] atx::core::Result<std::string> token_digest_hex(std::string_view token);

// Compares without an early exit on the first differing byte, so comparison
// time does not leak how much of a guessed token was correct.
[[nodiscard]] bool constant_time_equals(std::string_view a, std::string_view b);

} // namespace atx::server
```

```cpp
// src/auth.cpp
#include "atx/server/auth.hpp"

#include <cstddef>
#include <span>

#include "atx/core/sha256.hpp"

namespace atx::server {

atx::core::Result<std::string> token_digest_hex(std::string_view token) {
  return atx::core::sha256_hex(token);
}

bool constant_time_equals(std::string_view a, std::string_view b) {
  // Length is not secret (digests are fixed width), but the comparison itself
  // must not short-circuit.
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

} // namespace atx::server
```

- [ ] **Step 5: Write `atx-server/include/atx/server/catalog.hpp`**

```cpp
#pragma once

// Catalog — the coverage index and the server's authoritative state.
//
// Two tiers on ONE connection, joined by ATTACH:
//
//   main  (":memory:")      DERIVED.       Rebuilt at start from the databases
//                                          on disk. Losing it costs a rescan.
//   state (state.db, WAL)   AUTHORITATIVE. Realm, tokens, entitlements. Not
//                                          reconstructible; survives restart.
//
// Threading. SQLITE_THREADSAFE=2 forbids using ONE connection from two threads
// SIMULTANEOUSLY. sqlite.hpp states a stricter house rule (never share a
// Database across threads at all), which cannot hold here: a :memory: database
// is private to its connection, so per-thread connections would produce N
// separate indexes rather than one shared one. This class therefore guards a
// single Database with a mutex, and every expensive operation -- ATXVSA decode,
// curve evaluation, blob encode -- runs OUTSIDE that lock, after the catalog
// has returned identifiers. Catalog queries themselves are microsecond-scale
// lookups against an in-memory database.
//
// Escape hatch if contention is ever measured: move `main` to a WAL file
// database and give each worker its own connection. No API change is required.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/rpc/call_context.hpp"
#include "atx/server/realm.hpp"

namespace atx::server {

struct SymbolRow {
  std::string symbol;
  bool enabled{false};
  std::uint32_t preset{0};
  std::uint32_t curve_kind{0};
  std::uint32_t flags{0};
};

struct PartitionRow {
  std::string key;
  std::uint32_t surface_count{0};
  std::uint64_t file_size{0};
  std::int64_t created_ns{0};
};

struct SurfaceRow {
  std::string partition_key;
  std::string symbol;
  std::uint32_t expiry_count{0};
  double spot{0.0};
  std::uint32_t model_kind{0};
  std::uint32_t risk_state{0};
};

struct DbSummary {
  std::string db_id;
  std::string kind;
  std::uint64_t generation{0};
  std::uint32_t symbol_count{0};
  std::uint32_t partition_count{0};
};

struct CoverageRow {
  std::string symbol;
  std::string partition_key;
  bool configured{false};
  bool fitted{false};
  std::uint32_t expiry_count{0};
};

// One database's complete derived index. replace_db_index swaps it atomically,
// so a rescan never leaves a half-updated view visible to a reader.
struct DbIndex {
  std::uint64_t generation{0};
  std::vector<SymbolRow> symbols;
  std::vector<PartitionRow> partitions;
  std::vector<SurfaceRow> surfaces;
};

class Catalog {
public:
  [[nodiscard]] static atx::core::Result<std::unique_ptr<Catalog>>
  open(std::string_view state_path);

  ~Catalog();
  Catalog(const Catalog &) = delete;
  Catalog &operator=(const Catalog &) = delete;
  Catalog(Catalog &&) = delete;
  Catalog &operator=(Catalog &&) = delete;

  // --- state tier (authoritative) ---
  [[nodiscard]] atx::core::Status upsert_realm_entry(const RealmEntry &entry);
  [[nodiscard]] atx::core::Result<Realm> load_realm() const;
  [[nodiscard]] atx::core::Status put_token(std::string_view token, std::string label,
                                            std::vector<std::string> db_ids);
  [[nodiscard]] atx::core::Status disable_token(std::string_view token);
  // PermissionDenied for unknown or disabled. The dispatcher turns that into
  // UNAUTHENTICATED on the wire.
  [[nodiscard]] atx::core::Result<atx::rpc::Entitlements>
  resolve_token(std::string_view token) const;
  [[nodiscard]] atx::core::Result<std::string> server_uuid();

  // --- main tier (derived) ---
  [[nodiscard]] atx::core::Status replace_db_index(std::string_view db_id,
                                                   const DbIndex &index);
  [[nodiscard]] atx::core::Result<std::vector<DbSummary>> list_databases() const;
  [[nodiscard]] atx::core::Result<std::vector<SymbolRow>>
  list_symbols(std::string_view db_id, std::string_view prefix, std::uint32_t offset,
               std::uint32_t limit) const;
  [[nodiscard]] atx::core::Result<std::vector<PartitionRow>>
  list_partitions(std::string_view db_id, std::string_view key_begin,
                  std::string_view key_end, std::uint32_t offset,
                  std::uint32_t limit) const;
  [[nodiscard]] atx::core::Result<std::vector<SurfaceRow>>
  list_surfaces(std::string_view db_id, std::string_view symbol, std::string_view key_begin,
                std::string_view key_end, std::uint32_t offset, std::uint32_t limit) const;
  // Cross-product of the requested symbols and the partitions in range. Cells
  // that are neither configured nor fitted are omitted; the caller renders
  // absence.
  [[nodiscard]] atx::core::Result<std::vector<CoverageRow>>
  coverage(std::string_view db_id, const std::vector<std::string> &symbols,
           std::string_view key_begin, std::string_view key_end) const;
  [[nodiscard]] atx::core::Result<std::uint64_t> generation_of(std::string_view db_id) const;

  // --- warm start ---
  [[nodiscard]] atx::core::Status snapshot_to(std::string_view path);
  [[nodiscard]] atx::core::Status restore_from(std::string_view path);

  // Every row of every `main` table, rendered as sorted text, EXCLUDING
  // db_source.scanned_ns. That column records when a row was indexed and
  // therefore differs between a warm start and a cold scan by design; every
  // other column must match, and the determinism tests compare exactly this.
  [[nodiscard]] atx::core::Result<std::vector<std::string>> main_table_digest() const;

private:
  explicit Catalog(atx::core::db::Database db);

  [[nodiscard]] atx::core::Status create_schema_locked();

  mutable std::mutex mutex_;
  atx::core::db::Database db_;
};

} // namespace atx::server
```

- [ ] **Step 6: Write `atx-server/src/catalog.cpp`**

Implement each declared method. The schema-creation body is the load-bearing part; write it exactly as below, then implement the accessors as straightforward prepared-statement loops under `std::lock_guard<std::mutex> lock{mutex_}`.

```cpp
#include "atx/server/catalog.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>

#include "atx/server/auth.hpp"

namespace atx::server {
namespace {

using atx::core::db::Database;
using atx::core::db::Statement;
using atx::core::db::Transaction;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

constexpr int kSchemaVersion = 1;

[[nodiscard]] std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

Catalog::Catalog(Database db) : db_{std::move(db)} {}
Catalog::~Catalog() = default;

atx::core::Status Catalog::create_schema_locked() {
  // main: DERIVED. Rebuilt from disk at start; losing it costs only a rescan.
  ATX_TRY_VOID(db_.exec(R"sql(
    CREATE TABLE IF NOT EXISTS db_source(
      db_id      TEXT PRIMARY KEY,
      kind       TEXT NOT NULL CHECK(kind IN ('surface_db')),
      generation INTEGER NOT NULL,
      scanned_ns INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS symbol(
      db_id      TEXT NOT NULL,
      symbol     TEXT NOT NULL,
      enabled    INTEGER NOT NULL,
      preset     INTEGER NOT NULL,
      curve_kind INTEGER NOT NULL,
      flags      INTEGER NOT NULL,
      PRIMARY KEY(db_id, symbol));
    CREATE TABLE IF NOT EXISTS partition(
      db_id         TEXT NOT NULL,
      key           TEXT NOT NULL,
      surface_count INTEGER NOT NULL,
      file_size     INTEGER NOT NULL,
      created_ns    INTEGER NOT NULL,
      PRIMARY KEY(db_id, key));
    CREATE TABLE IF NOT EXISTS surface(
      db_id        TEXT NOT NULL,
      part_key     TEXT NOT NULL,
      symbol       TEXT NOT NULL,
      expiry_count INTEGER NOT NULL,
      spot         REAL NOT NULL,
      model_kind   INTEGER NOT NULL,
      risk_state   INTEGER NOT NULL,
      PRIMARY KEY(db_id, part_key, symbol));
    CREATE INDEX IF NOT EXISTS surface_by_symbol ON surface(symbol, part_key);
  )sql"));

  // state: AUTHORITATIVE. CHECK constraints make an invalid row impossible to
  // write rather than merely discouraged, because nothing here is
  // reconstructible from disk if it goes wrong.
  ATX_TRY_VOID(db_.exec(R"sql(
    CREATE TABLE IF NOT EXISTS state.schema_version(version INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS state.realm(
      db_id    TEXT PRIMARY KEY,
      kind     TEXT NOT NULL CHECK(kind IN ('surface_db')),
      root     TEXT NOT NULL,
      added_ns INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS state.token(
      token_sha256 TEXT PRIMARY KEY,
      label        TEXT NOT NULL,
      created_ns   INTEGER NOT NULL,
      disabled     INTEGER NOT NULL DEFAULT 0 CHECK(disabled IN (0,1)));
    CREATE TABLE IF NOT EXISTS state.entitlement(
      token_sha256 TEXT NOT NULL,
      db_id        TEXT NOT NULL,
      mode         TEXT NOT NULL CHECK(mode IN ('read')),
      PRIMARY KEY(token_sha256, db_id));
    CREATE TABLE IF NOT EXISTS state.kv(key TEXT PRIMARY KEY, value BLOB);
  )sql"));

  ATX_TRY(Statement *version, db_.prepare_cached("SELECT version FROM state.schema_version"));
  ATX_TRY(const auto step, version->step());
  if (step == Statement::Step::Done) {
    ATX_TRY_VOID(version->reset());
    ATX_TRY_VOID(db_.exec("INSERT INTO state.schema_version(version) VALUES(" +
                          std::to_string(kSchemaVersion) + ")"));
  } else {
    const std::int64_t found = version->column_int(0);
    ATX_TRY_VOID(version->reset());
    if (found > kSchemaVersion) {
      return Err(ErrorCode::InvalidArgument,
                 "state database schema version " + std::to_string(found) +
                     " is newer than this build understands (" +
                     std::to_string(kSchemaVersion) +
                     "); refusing to open rather than corrupt it");
    }
  }
  return Ok();
}

atx::core::Result<std::unique_ptr<Catalog>> Catalog::open(std::string_view state_path) {
  ATX_TRY(Database db, Database::open_memory());
  // ATTACH the authoritative tier onto the same connection. This is what lets
  // one query join the derived index against realm/entitlement rows.
  ATX_TRY_VOID(db.exec("ATTACH DATABASE '" + std::string{state_path} + "' AS state"));
  // WAL and a busy timeout apply to the attached FILE database. `main` is
  // in-memory and has no journal mode to configure.
  ATX_TRY_VOID(db.exec("PRAGMA state.journal_mode = WAL"));
  ATX_TRY_VOID(db.set_busy_timeout(5000));
  ATX_TRY_VOID(db.exec("PRAGMA foreign_keys = ON"));

  auto catalog = std::unique_ptr<Catalog>{new Catalog{std::move(db)}};
  const std::lock_guard<std::mutex> lock{catalog->mutex_};
  ATX_TRY_VOID(catalog->create_schema_locked());
  return Ok(std::move(catalog));
}
```

Implement the remaining methods with the same discipline. Specifically:

- `upsert_realm_entry` — `INSERT INTO state.realm(db_id, kind, root, added_ns) VALUES(?1,?2,?3,?4) ON CONFLICT(db_id) DO UPDATE SET kind=?2, root=?3`. A `CHECK` violation surfaces as the `Status` from `step()`.
- `load_realm` — `SELECT db_id, kind, root, added_ns FROM state.realm ORDER BY db_id`, building a `Realm` via `Realm::add`.
- `put_token` — inside one `Transaction`: compute `token_digest_hex(token)`, `INSERT OR REPLACE INTO state.token(token_sha256,label,created_ns,disabled) VALUES(?1,?2,?3,0)`, `DELETE FROM state.entitlement WHERE token_sha256=?1`, then one `INSERT INTO state.entitlement(token_sha256,db_id,mode) VALUES(?1,?2,'read')` per id. Commit.
- `disable_token` — `UPDATE state.token SET disabled=1 WHERE token_sha256=?1`; `NotFound` if `changes() == 0`.
- `resolve_token` — digest the token, `SELECT label, disabled FROM state.token WHERE token_sha256=?1`. On no row or `disabled != 0`, return `Err(ErrorCode::PermissionDenied, "token is unknown or disabled")` — one message for both so a caller cannot distinguish "wrong token" from "revoked token" by probing. Then `SELECT db_id FROM state.entitlement WHERE token_sha256=?1 AND mode='read'` into `readable_db_ids`. Verify the stored digest against the computed one with `constant_time_equals` before accepting.
- `server_uuid` — `SELECT value FROM state.kv WHERE key='server_uuid'`; if absent, generate 32 hex chars from `std::random_device` + `std::mt19937_64`, insert, and return it.
- `replace_db_index` — one `Transaction`: `DELETE FROM symbol/partition/surface WHERE db_id=?1`, re-insert every row, then `INSERT INTO db_source(...) ON CONFLICT(db_id) DO UPDATE SET generation=?3, scanned_ns=?4`. Commit. The delete-then-insert inside one transaction is why a rescan never leaves stale rows and never exposes a half-updated view.
- `list_databases` — join `db_source` with counts from `symbol` and `partition`.
- `list_symbols` — `WHERE db_id=?1 AND (?2='' OR symbol LIKE ?2 || '%') ORDER BY symbol LIMIT ?3 OFFSET ?4`.
- `list_partitions` / `list_surfaces` — analogous, with `(?N='' OR key >= ?N)` and `(?M='' OR key < ?M)` range predicates, ordered by key then symbol.
- `coverage` — `SELECT s.symbol, p.key, sf.expiry_count IS NOT NULL, ... FROM symbol s CROSS JOIN partition p LEFT JOIN surface sf ON sf.db_id=s.db_id AND sf.symbol=s.symbol AND sf.part_key=p.key WHERE s.db_id=?1 AND (range predicates)`, filtered to the requested symbol set when non-empty. `configured` is true for every row (they come from the symbol table); `fitted` is true when the `LEFT JOIN` matched.
- `generation_of` — `SELECT generation FROM db_source WHERE db_id=?1`; `NotFound` if absent.
- `snapshot_to` — open the destination with `Database::open(path, OpenMode::ReadWriteCreate)` and `db_.backup_to(dest)`.
- `restore_from` — open the source read-only and back it up into `db_`.
- `main_table_digest` — `SELECT` every column of `db_source` **except `scanned_ns`**, then of `symbol`, `partition`, `surface`, formatting each row as `"table|col|col|..."`, sorted.

- [ ] **Step 7: Add the sources to `atx-server/CMakeLists.txt`**

```cmake
add_library(atx-server-lib ${ATX_LIB_TYPE}
    src/version.cpp
    src/config.cpp
    src/realm.cpp
    src/auth.cpp
    src/catalog.cpp
)
```

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cmake --build --preset server --target atx-server-tests`
Then: `ctest --preset server -R "^TokenDigest\.|^ConstantTimeEquals\.|^Catalog\." --output-on-failure`
Expected: 15 tests, all PASS.

- [ ] **Step 9: Commit**

```bash
git add atx-server/include/atx/server/catalog.hpp atx-server/include/atx/server/auth.hpp \
        atx-server/src/catalog.cpp atx-server/src/auth.cpp \
        atx-server/tests/catalog_test.cpp atx-server/tests/auth_test.cpp \
        atx-server/CMakeLists.txt
git commit -m "feat(server): add the two-tier catalog, tokens, and entitlements

One connection, two tiers joined by ATTACH: an in-memory derived index that a
rescan can rebuild, and a WAL file holding realm, tokens, and entitlements that
nothing can reconstruct. The split decides what a crash costs.

Threading deviates from the sqlite.hpp house rule deliberately, and the header
says why: a :memory: database is private to its connection, so per-thread
connections would produce N separate indexes rather than one shared one. A
single Database under a mutex is correct here because catalog queries are
microsecond-scale in-memory lookups and every expensive operation -- decode,
curve evaluation, blob encode -- runs outside the lock on identifiers the
catalog already returned.

CHECK constraints close the value domains on the authoritative tier. An invalid
realm kind or entitlement mode is impossible to write, not merely discouraged,
because there is no source of truth to repair it from.

Only sha256(token) is stored, and a test greps the raw database file for a
planted plaintext canary. Comparison is constant-time so timing does not leak
how much of a guessed token was right. Unknown and disabled tokens return the
same error text, so probing cannot distinguish a wrong token from a revoked one.

replace_db_index deletes and re-inserts one database's rows inside a single
transaction. That is what makes a rescan drop stale rows and never expose a
half-updated view; a test shrinks an index and asserts the old rows are gone.

main_table_digest excludes db_source.scanned_ns and nothing else. That column
records when a row was indexed and so differs between a warm start and a cold
scan by design; every other column must match, and the determinism tests
compare exactly this.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 11: `atx-server` catalog scan, refresh, and warm start

**Files:**
- Create: `atx-server/include/atx/server/catalog_scan.hpp`, `atx-server/src/catalog_scan.cpp`
- Create: `atx-server/include/atx/server/catalog_refresh.hpp`, `atx-server/src/catalog_refresh.cpp`
- Create: `atx-server/tests/catalog_scan_test.cpp`
- Create: `atx-server/tests/support/fixture_db.hpp`, `atx-server/tests/support/fixture_db.cpp`
- Modify: `atx-server/CMakeLists.txt`

**Interfaces:**
- Consumes: `Catalog`, `DbIndex` (Task 10); `atx::vol::SurfaceDb` (`open`, `generation`, `symbols`, `symbol_config`, `partitions`, `refresh`, `map_surface`).
- Produces:
  - `atx::core::Result<DbIndex> atx::server::scan_surface_db(atx::vol::SurfaceDb& db)`
  - `atx::core::Status atx::server::index_realm(Catalog&, const Realm&, SurfaceRegistryLike&)` — declared here but implemented in Task 12 once `SurfaceRegistry` exists; this task's version takes `atx::vol::SurfaceDb&` directly.
  - `class atx::server::CatalogRefresher` with `CatalogRefresher(Catalog&, SurfaceRegistry&, std::chrono::milliseconds)`, `void start()`, `void stop() noexcept`, `std::uint64_t rescans() const`. Declared in this task; its `SurfaceRegistry` dependency is satisfied in Task 12, so the refresher's **tests** live in Task 12 and this task only lands `scan_surface_db` plus the warm-start round trip.
  - Test support: `atx::server::test::FixtureDb` — builds a temporary `SurfaceDb` with two symbols across two partition keys, one symbol deliberately absent from the second partition so coverage has a real gap.

- [ ] **Step 1: Write the fixture support header**

`atx-server/tests/support/fixture_db.hpp`:

```cpp
#pragma once

// A real on-disk SurfaceDb for tests. Every catalog, encode, and blob test
// needs one, and building it once here keeps the assertions about behaviour
// rather than about setup.

#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/surface_db.hpp"

namespace atx::server::test {

struct FixtureDb {
  std::string root;
  // "2024-03-14" holds SPY and AAPL; "2024-03-15" holds SPY only, so AAPL is
  // configured-but-not-fitted there and coverage has a genuine gap to report.
  std::vector<std::string> partition_keys{"2024-03-14", "2024-03-15"};
  std::vector<std::string> symbols{"AAPL", "SPY"};
};

// Creates the database under a scratch directory and returns its description.
[[nodiscard]] atx::core::Result<FixtureDb> make_fixture_db(std::string_view name);

} // namespace atx::server::test
```

`atx-server/tests/support/fixture_db.cpp` builds it: create the scratch directory with `atx::test::scratch_dir`, `atx::vol::SurfaceDb::create(root)`, `upsert_symbols` for both symbols with a default `SymbolFitConfig`, then `write_partition` twice with the symbol sets above. Reuse the surface-construction helper already used by `atx-vol/tests/surface_db_test.cpp`; if that helper is file-local there, copy the minimal surface builder into this file rather than exporting it from `atx-vol`.

- [ ] **Step 2: Write the failing test**

`atx-server/tests/catalog_scan_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include "atx/core/error.hpp"
#include "atx/server/catalog.hpp"
#include "atx/server/catalog_scan.hpp"
#include "atx/test/scratch.hpp"
#include "atx/vol/surface_db.hpp"
#include "support/fixture_db.hpp"

namespace {

using atx::server::Catalog;
using atx::server::DbIndex;
using atx::server::RealmEntry;
using atx::server::scan_surface_db;
using atx::server::test::make_fixture_db;

TEST(CatalogScan, IndexesSymbolsPartitionsAndSurfaces) {
  auto fixture = make_fixture_db("scan-basic");
  ASSERT_TRUE(fixture) << fixture.error().to_string();
  auto db = atx::vol::SurfaceDb::open(fixture->root);
  ASSERT_TRUE(db) << db.error().to_string();

  auto index = scan_surface_db(*db);
  ASSERT_TRUE(index) << index.error().to_string();
  EXPECT_EQ(index->symbols.size(), 2u);
  EXPECT_EQ(index->partitions.size(), 2u);
  // SPY in both partitions, AAPL in one.
  EXPECT_EQ(index->surfaces.size(), 3u);
  EXPECT_EQ(index->generation, db->generation());
}

TEST(CatalogScan, ScanningTwiceProducesIdenticalRows) {
  auto fixture = make_fixture_db("scan-idempotent");
  ASSERT_TRUE(fixture);
  auto db = atx::vol::SurfaceDb::open(fixture->root);
  ASSERT_TRUE(db);

  auto first = scan_surface_db(*db);
  auto second = scan_surface_db(*db);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_EQ(first->surfaces.size(), second->surfaces.size());
  for (std::size_t i = 0; i < first->surfaces.size(); ++i) {
    EXPECT_EQ(first->surfaces[i].symbol, second->surfaces[i].symbol);
    EXPECT_EQ(first->surfaces[i].partition_key, second->surfaces[i].partition_key);
    EXPECT_EQ(first->surfaces[i].expiry_count, second->surfaces[i].expiry_count);
    EXPECT_DOUBLE_EQ(first->surfaces[i].spot, second->surfaces[i].spot);
  }
}

TEST(CatalogScan, ScanOutputIsSortedForStableDigests) {
  auto fixture = make_fixture_db("scan-sorted");
  ASSERT_TRUE(fixture);
  auto db = atx::vol::SurfaceDb::open(fixture->root);
  ASSERT_TRUE(db);
  auto index = scan_surface_db(*db);
  ASSERT_TRUE(index);

  EXPECT_TRUE(std::is_sorted(index->symbols.begin(), index->symbols.end(),
                             [](const auto &a, const auto &b) { return a.symbol < b.symbol; }));
  EXPECT_TRUE(std::is_sorted(index->partitions.begin(), index->partitions.end(),
                             [](const auto &a, const auto &b) { return a.key < b.key; }));
}

// The acceptance criterion for warm start: restoring the snapshot must produce
// the same catalog a cold full scan would, on every column except scanned_ns.
TEST(CatalogWarmStart, RestoredSnapshotEqualsAColdScan) {
  auto fixture = make_fixture_db("warm-start");
  ASSERT_TRUE(fixture);
  auto db = atx::vol::SurfaceDb::open(fixture->root);
  ASSERT_TRUE(db);
  auto index = scan_surface_db(*db);
  ASSERT_TRUE(index);

  const std::filesystem::path dir = atx::test::scratch_dir("atx-server-warm");
  const std::string state_a = (dir / "a-state.db").string();
  const std::string state_b = (dir / "b-state.db").string();
  const std::string snapshot = (dir / "snapshot.db").string();

  std::vector<std::string> cold_digest;
  {
    auto catalog = Catalog::open(state_a);
    ASSERT_TRUE(catalog);
    RealmEntry entry;
    entry.db_id = "fixture";
    entry.kind = "surface_db";
    entry.root = fixture->root;
    ASSERT_TRUE((*catalog)->upsert_realm_entry(entry));
    ASSERT_TRUE((*catalog)->replace_db_index("fixture", *index));
    auto digest = (*catalog)->main_table_digest();
    ASSERT_TRUE(digest);
    cold_digest = *digest;
    ASSERT_TRUE((*catalog)->snapshot_to(snapshot));
  }

  auto warm = Catalog::open(state_b);
  ASSERT_TRUE(warm);
  ASSERT_TRUE((*warm)->restore_from(snapshot));
  auto warm_digest = (*warm)->main_table_digest();
  ASSERT_TRUE(warm_digest);

  EXPECT_EQ(*warm_digest, cold_digest);
  EXPECT_FALSE(cold_digest.empty()) << "an empty digest would make this test vacuous";
}

TEST(CatalogWarmStart, RestoreFromAMissingSnapshotFails) {
  const std::filesystem::path dir = atx::test::scratch_dir("atx-server-warm-missing");
  auto catalog = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(catalog);
  EXPECT_FALSE((*catalog)->restore_from((dir / "does-not-exist.db").string()));
}

} // namespace
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --build --preset server --target atx-server-tests`
Expected: FAIL at compile — `atx/server/catalog_scan.hpp: No such file or directory`.

- [ ] **Step 4: Write `atx-server/include/atx/server/catalog_scan.hpp`**

```cpp
#pragma once

// Turning a SurfaceDb on disk into the catalog's derived index.
//
// Output is sorted so two scans of an unchanged database produce byte-identical
// rows, which is what makes the warm-start equality assertion meaningful rather
// than accidentally true.

#include "atx/core/error.hpp"
#include "atx/server/catalog.hpp"
#include "atx/vol/surface_db.hpp"

namespace atx::server {

// Reads the manifest symbol table, the partition index, and each partition's
// per-symbol directory. Does NOT decode surfaces: the expiry count and spot
// come from the archive directory, so indexing a large realm does not pay a
// full decode per surface.
[[nodiscard]] atx::core::Result<DbIndex> scan_surface_db(atx::vol::SurfaceDb &db);

} // namespace atx::server
```

- [ ] **Step 5: Write `atx-server/src/catalog_scan.cpp`**

```cpp
#include "atx/server/catalog_scan.hpp"

#include <algorithm>

#include "atx/vol/surface_archive.hpp"

namespace atx::server {
namespace {

using atx::core::Ok;

} // namespace

atx::core::Result<DbIndex> scan_surface_db(atx::vol::SurfaceDb &db) {
  DbIndex index;
  index.generation = db.generation();

  // The manifest symbol table: how each symbol should be fit. Orthogonal to
  // what has actually been written, which is exactly why coverage can report
  // CONFIGURED_NOT_FITTED.
  for (const std::string &symbol : db.symbols()) {
    ATX_TRY(const atx::vol::SymbolFitConfig config, db.symbol_config(symbol));
    SymbolRow row;
    row.symbol = symbol;
    row.enabled = (config.flags & atx::vol::kDbSymEnabled) != 0;
    row.preset = static_cast<std::uint32_t>(config.preset);
    row.curve_kind = static_cast<std::uint32_t>(config.curve_kind);
    row.flags = config.flags;
    index.symbols.push_back(std::move(row));
  }

  for (const atx::vol::DbPartitionInfo &info : db.partitions()) {
    PartitionRow row;
    row.key = info.key;
    row.surface_count = static_cast<std::uint32_t>(info.surface_count);
    row.file_size = static_cast<std::uint64_t>(info.file_size);
    row.created_ns = info.created_ns;
    index.partitions.push_back(row);

    // Per-partition surface rows. map_surface returns a zero-copy view over the
    // mmapped archive, so this reads the directory rather than decoding every
    // surface -- indexing a large realm must not cost a full decode per entry.
    for (const SymbolRow &symbol : index.symbols) {
      auto loaded = db.map_surface(info.key, symbol.symbol);
      if (!loaded) {
        continue; // absent from this partition: coverage reports the gap
      }
      SurfaceRow surface;
      surface.partition_key = info.key;
      surface.symbol = symbol.symbol;
      surface.expiry_count = static_cast<std::uint32_t>((*loaded)->expiry_count());
      surface.spot = (*loaded)->spot();
      surface.model_kind = static_cast<std::uint32_t>((*loaded)->model_kind());
      surface.risk_state = static_cast<std::uint32_t>((*loaded)->risk_state());
      index.surfaces.push_back(std::move(surface));
    }
  }

  // Sorted so an unchanged database scans to identical rows every time. The
  // warm-start equality assertion depends on this.
  std::sort(index.symbols.begin(), index.symbols.end(),
            [](const SymbolRow &a, const SymbolRow &b) { return a.symbol < b.symbol; });
  std::sort(index.partitions.begin(), index.partitions.end(),
            [](const PartitionRow &a, const PartitionRow &b) { return a.key < b.key; });
  std::sort(index.surfaces.begin(), index.surfaces.end(),
            [](const SurfaceRow &a, const SurfaceRow &b) {
              return std::tie(a.partition_key, a.symbol) < std::tie(b.partition_key, b.symbol);
            });
  return Ok(std::move(index));
}

} // namespace atx::server
```

If `PricedSurfaceView` does not expose `expiry_count()`, `spot()`, `model_kind()`, or `risk_state()` under those exact names, read `atx-vol/include/atx/vol/priced_surface_view.hpp` and substitute the actual accessors. Do not invent fields; if a value is genuinely unavailable from the view, populate 0 and note it in the commit body.

- [ ] **Step 6: Write `atx-server/include/atx/server/catalog_refresh.hpp`**

Declaration only in this task; its implementation and tests land in Task 12, once `SurfaceRegistry` exists to poll.

```cpp
#pragma once

// CatalogRefresher — the generation-drift poller.
//
// Reading SurfaceDb::generation() is one manifest-header read, so polling every
// registered database is cheap. On drift it calls refresh(), rescans ONLY that
// db_id inside one transaction, and bumps db_source.generation. That value is
// exactly what every ResponseMeta.db_generation reports, so a client that
// revalidates on generation is correct by construction rather than by
// convention.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace atx::server {

class Catalog;
class SurfaceRegistry;

class CatalogRefresher {
public:
  CatalogRefresher(Catalog &catalog, SurfaceRegistry &registry,
                   std::chrono::milliseconds interval);
  ~CatalogRefresher();

  CatalogRefresher(const CatalogRefresher &) = delete;
  CatalogRefresher &operator=(const CatalogRefresher &) = delete;
  CatalogRefresher(CatalogRefresher &&) = delete;
  CatalogRefresher &operator=(CatalogRefresher &&) = delete;

  void start();
  void stop() noexcept; // idempotent
  [[nodiscard]] std::uint64_t rescans() const noexcept { return rescans_.load(); }

private:
  void loop();

  Catalog &catalog_;
  SurfaceRegistry &registry_;
  std::chrono::milliseconds interval_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<std::uint64_t> rescans_{0};
};

} // namespace atx::server
```

- [ ] **Step 7: Add the sources and test support to CMake**

```cmake
add_library(atx-server-lib ${ATX_LIB_TYPE}
    src/version.cpp
    src/config.cpp
    src/realm.cpp
    src/auth.cpp
    src/catalog.cpp
    src/catalog_scan.cpp
)
```

In `atx-server/tests/CMakeLists.txt`, add the support TU and its include path:

```cmake
add_executable(atx-server-tests ${ATX_SERVER_TEST_SOURCES} support/fixture_db.cpp)
target_include_directories(atx-server-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
```

The `file(GLOB ... "*_test.cpp")` pattern does not match `support/fixture_db.cpp`, so listing it explicitly is required.

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cmake --build --preset server --target atx-server-tests`
Then: `ctest --preset server -R "^CatalogScan\.|^CatalogWarmStart\." --output-on-failure`
Expected: 5 tests, all PASS.

- [ ] **Step 9: Commit**

```bash
git add atx-server/include/atx/server/catalog_scan.hpp \
        atx-server/include/atx/server/catalog_refresh.hpp \
        atx-server/src/catalog_scan.cpp \
        atx-server/tests/catalog_scan_test.cpp atx-server/tests/support \
        atx-server/CMakeLists.txt atx-server/tests/CMakeLists.txt
git commit -m "feat(server): scan a SurfaceDb into the catalog, and prove warm start

scan_surface_db reads the manifest symbol table, the partition index, and each
partition's per-symbol directory. It does not decode surfaces: expiry count and
spot come from the archive directory via the zero-copy view, so indexing a large
realm does not pay a full decode per entry.

The symbol table and the partition contents are scanned separately because they
are orthogonal -- one says how a symbol should be fit, the other says where a
fitted surface actually landed. That separation is the only reason coverage can
distinguish CONFIGURED_NOT_FITTED from ABSENT.

Scan output is sorted. Two scans of an unchanged database therefore produce
identical rows, which is what makes the warm-start equality assertion
meaningful rather than accidentally true. The test asserts the digest is
non-empty for the same reason: an empty-vs-empty comparison would pass while
proving nothing.

A shared fixture database is added under tests/support: two symbols across two
partition keys, with one symbol deliberately missing from the second partition
so coverage has a real gap to report instead of a synthetic one.

CatalogRefresher is declared here and implemented in the next task, once there
is a SurfaceRegistry for it to poll.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 12: `atx-server` surface registry, encoders, and the refresher

**Files:**
- Create: `atx-server/include/atx/server/surface_registry.hpp`, `atx-server/src/surface_registry.cpp`
- Create: `atx-server/include/atx/server/encode.hpp`, `atx-server/src/encode.cpp`
- Create: `atx-server/src/catalog_refresh.cpp`
- Create: `atx-server/tests/surface_registry_test.cpp`, `atx-server/tests/encode_test.cpp`, `atx-server/tests/catalog_refresh_test.cpp`
- Modify: `atx-server/CMakeLists.txt`

**Interfaces:**
- Consumes: `Realm` (Task 9), `Catalog`/`DbIndex` (Task 10), `scan_surface_db` (Task 11), surface protos (Task 8).
- Produces:
  - `class atx::server::SurfaceRegistry`: `SurfaceRegistry(const Realm&, std::size_t partition_cache_capacity)`, `Result<std::shared_ptr<atx::vol::SurfaceDb>> get(std::string_view db_id)`, `std::vector<std::string> open_db_ids() const`, `Result<std::uint64_t> generation(std::string_view db_id)`, `Status refresh(std::string_view db_id)`, `Result<atx::vol::SurfaceDbCacheStats> cache_stats(std::string_view db_id)`.
  - Free encoders in `atx::server`, all pure, none touching a socket or the registry:
    - `v1::SymbolFitConfig encode_symbol_config(std::string_view symbol, const atx::vol::SymbolFitConfig&)`
    - `v1::ExpirySummary encode_expiry(const atx::vol::PricedSurfaceView&, std::size_t expiry_index)`
    - `v1::SurfaceDiagnostics encode_diagnostics(const atx::vol::PricedSurfaceView&)`
    - `Result<v1::VolCurveSlice> encode_curve(const atx::vol::PricedSurfaceView&, std::string_view expiry_iso, double z_window, std::uint32_t curve_points, bool include_quotes)`
    - `Result<v1::SurfaceMeta> encode_surface_meta(const atx::vol::PricedSurfaceView&)`
    - `void stamp_meta(v1::ResponseMeta&, std::uint64_t db_generation, std::uint64_t content_hash)`
  - `atx::server::CatalogRefresher` implementation.

- [ ] **Step 1: Write the failing registry test**

`atx-server/tests/surface_registry_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/server/realm.hpp"
#include "atx/server/surface_registry.hpp"
#include "support/fixture_db.hpp"

namespace {

using atx::core::ErrorCode;
using atx::server::Realm;
using atx::server::RealmEntry;
using atx::server::SurfaceRegistry;
using atx::server::test::make_fixture_db;

Realm realm_with(const std::string &db_id, const std::string &root) {
  Realm realm;
  RealmEntry entry;
  entry.db_id = db_id;
  entry.kind = "surface_db";
  entry.root = root;
  EXPECT_TRUE(realm.add(entry));
  return realm;
}

TEST(SurfaceRegistry, OpensLazilyAndCachesTheHandle) {
  auto fixture = make_fixture_db("registry-lazy");
  ASSERT_TRUE(fixture);
  const Realm realm = realm_with("fixture", fixture->root);
  SurfaceRegistry registry{realm, 4};

  EXPECT_TRUE(registry.open_db_ids().empty()) << "nothing should open before first use";
  auto first = registry.get("fixture");
  ASSERT_TRUE(first) << first.error().to_string();
  auto second = registry.get("fixture");
  ASSERT_TRUE(second);
  EXPECT_EQ(first->get(), second->get()) << "the same SurfaceDb must be reused";
  EXPECT_EQ(registry.open_db_ids().size(), 1u);
}

TEST(SurfaceRegistry, UnregisteredDbIdIsNotFound) {
  auto fixture = make_fixture_db("registry-missing");
  ASSERT_TRUE(fixture);
  const Realm realm = realm_with("fixture", fixture->root);
  SurfaceRegistry registry{realm, 4};
  auto missing = registry.get("nope");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

TEST(SurfaceRegistry, ReportsGenerationAndCacheStats) {
  auto fixture = make_fixture_db("registry-stats");
  ASSERT_TRUE(fixture);
  const Realm realm = realm_with("fixture", fixture->root);
  SurfaceRegistry registry{realm, 4};

  auto generation = registry.generation("fixture");
  ASSERT_TRUE(generation);
  EXPECT_GT(*generation, 0u);

  auto stats = registry.cache_stats("fixture");
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->capacity, 4u);
}

// Many workers hit the registry at once. Opening must happen exactly once and
// no thread may observe a half-constructed SurfaceDb.
TEST(SurfaceRegistry, ConcurrentFirstAccessOpensExactlyOnce) {
  auto fixture = make_fixture_db("registry-concurrent");
  ASSERT_TRUE(fixture);
  const Realm realm = realm_with("fixture", fixture->root);
  SurfaceRegistry registry{realm, 4};

  constexpr int kThreads = 8;
  std::vector<std::thread> threads;
  std::vector<const void *> handles(kThreads, nullptr);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&registry, &handles, i]() {
      auto db = registry.get("fixture");
      if (db) {
        handles[static_cast<std::size_t>(i)] = db->get();
      }
    });
  }
  for (std::thread &t : threads) {
    t.join();
  }
  for (int i = 0; i < kThreads; ++i) {
    EXPECT_NE(handles[static_cast<std::size_t>(i)], nullptr) << "thread " << i;
    EXPECT_EQ(handles[static_cast<std::size_t>(i)], handles[0]) << "thread " << i;
  }
  EXPECT_EQ(registry.open_db_ids().size(), 1u);
}

} // namespace
```

- [ ] **Step 2: Write the failing encode test**

`atx-server/tests/encode_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "atx/core/error.hpp"
#include "atx/server/encode.hpp"
#include "atx/vol/surface_db.hpp"
#include "support/fixture_db.hpp"

namespace {

using atx::server::encode_curve;
using atx::server::encode_surface_meta;
using atx::server::stamp_meta;
using atx::server::test::make_fixture_db;
namespace v1 = atx::rpc::v1;

TEST(Encode, SurfaceMetaCarriesEveryExpiry) {
  auto fixture = make_fixture_db("encode-meta");
  ASSERT_TRUE(fixture);
  auto db = atx::vol::SurfaceDb::open(fixture->root);
  ASSERT_TRUE(db);
  auto loaded = db->map_surface("2024-03-14", "SPY");
  ASSERT_TRUE(loaded) << loaded.error().to_string();

  auto meta = encode_surface_meta(**loaded);
  ASSERT_TRUE(meta) << meta.error().to_string();
  EXPECT_GT(meta->expiries_size(), 0);
  EXPECT_GT(meta->spot(), 0.0);
  // Expiries must arrive in ascending time order; the term-structure panel
  // plots them in array order and would otherwise draw a zig-zag.
  for (int i = 1; i < meta->expiries_size(); ++i) {
    EXPECT_LT(meta->expiries(i - 1).years(), meta->expiries(i).years());
  }
}

TEST(Encode, CurveHasPointsAndRespectsTheWindow) {
  auto fixture = make_fixture_db("encode-curve");
  ASSERT_TRUE(fixture);
  auto db = atx::vol::SurfaceDb::open(fixture->root);
  ASSERT_TRUE(db);
  auto loaded = db->map_surface("2024-03-14", "SPY");
  ASSERT_TRUE(loaded);

  auto meta = encode_surface_meta(**loaded);
  ASSERT_TRUE(meta);
  ASSERT_GT(meta->expiries_size(), 0);
  const std::string expiry = meta->expiries(0).expiry_iso();

  auto slice = encode_curve(**loaded, expiry, 2.0, 41, true);
  ASSERT_TRUE(slice) << slice.error().to_string();
  EXPECT_EQ(slice->expiry_iso(), expiry);
  EXPECT_EQ(slice->curve_size(), 41);
  for (const v1::VolCurvePoint &point : slice->curve()) {
    EXPECT_GE(point.z(), -2.0);
    EXPECT_LE(point.z(), 2.0);
    EXPECT_GT(point.model_iv(), 0.0);
  }
}

TEST(Encode, CurveForAnUnknownExpiryIsNotFound) {
  auto fixture = make_fixture_db("encode-bad-expiry");
  ASSERT_TRUE(fixture);
  auto db = atx::vol::SurfaceDb::open(fixture->root);
  ASSERT_TRUE(db);
  auto loaded = db->map_surface("2024-03-14", "SPY");
  ASSERT_TRUE(loaded);

  auto slice = encode_curve(**loaded, "1999-01-01", 2.0, 41, false);
  ASSERT_FALSE(slice);
  EXPECT_EQ(slice.error().code(), atx::core::ErrorCode::NotFound);
}

// Unbounded curve_points or z_window would let one client ask for an
// arbitrarily large response. Both are clamped server-side.
TEST(Encode, ClampsAbsurdCurveRequests) {
  auto fixture = make_fixture_db("encode-clamp");
  ASSERT_TRUE(fixture);
  auto db = atx::vol::SurfaceDb::open(fixture->root);
  ASSERT_TRUE(db);
  auto loaded = db->map_surface("2024-03-14", "SPY");
  ASSERT_TRUE(loaded);
  auto meta = encode_surface_meta(**loaded);
  ASSERT_TRUE(meta);
  const std::string expiry = meta->expiries(0).expiry_iso();

  auto huge = encode_curve(**loaded, expiry, 1e9, 1'000'000, false);
  ASSERT_TRUE(huge);
  EXPECT_LE(huge->curve_size(), 1025);
  for (const v1::VolCurvePoint &point : huge->curve()) {
    EXPECT_LE(std::abs(point.z()), 8.0);
  }

  auto zero = encode_curve(**loaded, expiry, 0.0, 0, false);
  ASSERT_TRUE(zero) << "zero must mean 'server default', not 'empty curve'";
  EXPECT_GT(zero->curve_size(), 0);
}

TEST(Encode, QuotesAreOmittedWhenNotRequested) {
  auto fixture = make_fixture_db("encode-quotes");
  ASSERT_TRUE(fixture);
  auto db = atx::vol::SurfaceDb::open(fixture->root);
  ASSERT_TRUE(db);
  auto loaded = db->map_surface("2024-03-14", "SPY");
  ASSERT_TRUE(loaded);
  auto meta = encode_surface_meta(**loaded);
  ASSERT_TRUE(meta);
  const std::string expiry = meta->expiries(0).expiry_iso();

  auto without = encode_curve(**loaded, expiry, 2.0, 21, false);
  ASSERT_TRUE(without);
  EXPECT_EQ(without->quotes_size(), 0);
}

TEST(Encode, StampMetaSetsGenerationAndHash) {
  v1::ResponseMeta meta;
  stamp_meta(meta, 12, 0xABCDULL);
  EXPECT_EQ(meta.db_generation(), 12u);
  EXPECT_EQ(meta.content_hash(), 0xABCDULL);
  EXPECT_GT(meta.server_ns(), 0);
}

} // namespace
```

- [ ] **Step 3: Write the failing refresher test**

`atx-server/tests/catalog_refresh_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "atx/server/catalog.hpp"
#include "atx/server/catalog_refresh.hpp"
#include "atx/server/catalog_scan.hpp"
#include "atx/server/surface_registry.hpp"
#include "atx/test/scratch.hpp"
#include "atx/vol/surface_db.hpp"
#include "support/fixture_db.hpp"

namespace {

using atx::server::Catalog;
using atx::server::CatalogRefresher;
using atx::server::Realm;
using atx::server::RealmEntry;
using atx::server::SurfaceRegistry;
using atx::server::test::make_fixture_db;

TEST(CatalogRefresher, PicksUpAnExternalGenerationBump) {
  auto fixture = make_fixture_db("refresh-drift");
  ASSERT_TRUE(fixture);

  Realm realm;
  RealmEntry entry;
  entry.db_id = "fixture";
  entry.kind = "surface_db";
  entry.root = fixture->root;
  ASSERT_TRUE(realm.add(entry));

  const std::filesystem::path dir = atx::test::scratch_dir("atx-server-refresh");
  auto catalog = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(catalog);
  ASSERT_TRUE((*catalog)->upsert_realm_entry(entry));

  SurfaceRegistry registry{realm, 4};
  auto db = registry.get("fixture");
  ASSERT_TRUE(db);
  auto index = atx::server::scan_surface_db(**db);
  ASSERT_TRUE(index);
  ASSERT_TRUE((*catalog)->replace_db_index("fixture", *index));
  const std::uint64_t before = *(*catalog)->generation_of("fixture");

  CatalogRefresher refresher{**catalog, registry, std::chrono::milliseconds{50}};
  refresher.start();

  // An external writer bumps the manifest generation.
  {
    auto writer = atx::vol::SurfaceDb::open(fixture->root);
    ASSERT_TRUE(writer);
    atx::vol::SymbolFitConfig config;
    ASSERT_TRUE(writer->upsert_symbol("MSFT", config));
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  bool observed = false;
  while (std::chrono::steady_clock::now() < deadline) {
    auto now = (*catalog)->generation_of("fixture");
    if (now && *now > before) {
      observed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  refresher.stop();
  EXPECT_TRUE(observed) << "the refresher never noticed the generation bump";
  EXPECT_GT(refresher.rescans(), 0u);
}

TEST(CatalogRefresher, DoesNotRescanAStaticDatabase) {
  auto fixture = make_fixture_db("refresh-static");
  ASSERT_TRUE(fixture);
  Realm realm;
  RealmEntry entry;
  entry.db_id = "fixture";
  entry.kind = "surface_db";
  entry.root = fixture->root;
  ASSERT_TRUE(realm.add(entry));

  const std::filesystem::path dir = atx::test::scratch_dir("atx-server-refresh-static");
  auto catalog = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(catalog);
  ASSERT_TRUE((*catalog)->upsert_realm_entry(entry));

  SurfaceRegistry registry{realm, 4};
  auto db = registry.get("fixture");
  ASSERT_TRUE(db);
  ASSERT_TRUE((*catalog)->replace_db_index("fixture", *atx::server::scan_surface_db(**db)));

  CatalogRefresher refresher{**catalog, registry, std::chrono::milliseconds{20}};
  refresher.start();
  std::this_thread::sleep_for(std::chrono::milliseconds{300});
  refresher.stop();
  // Polling a generation is one header read; rescanning is not. A static
  // database must cost the former and never the latter.
  EXPECT_EQ(refresher.rescans(), 0u);
}

TEST(CatalogRefresher, StopIsIdempotentAndSafeWithoutStart) {
  auto fixture = make_fixture_db("refresh-stop");
  ASSERT_TRUE(fixture);
  Realm realm;
  RealmEntry entry;
  entry.db_id = "fixture";
  entry.kind = "surface_db";
  entry.root = fixture->root;
  ASSERT_TRUE(realm.add(entry));

  const std::filesystem::path dir = atx::test::scratch_dir("atx-server-refresh-stop");
  auto catalog = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(catalog);
  SurfaceRegistry registry{realm, 4};

  CatalogRefresher refresher{**catalog, registry, std::chrono::milliseconds{20}};
  refresher.stop(); // never started
  refresher.start();
  refresher.stop();
  refresher.stop();
  SUCCEED();
}

} // namespace
```

- [ ] **Step 4: Run all three tests to verify they fail**

Run: `cmake --build --preset server --target atx-server-tests`
Expected: FAIL at compile — `atx/server/surface_registry.hpp: No such file or directory`.

- [ ] **Step 5: Write `atx-server/include/atx/server/surface_registry.hpp`**

```cpp
#pragma once

// SurfaceRegistry — one lazily-opened SurfaceDb per db_id.
//
// Databases open on first use rather than at startup, so a realm with fifty
// registered databases does not pay fifty manifest reads and fifty mmap caches
// to answer a question about one of them.
//
// The returned shared_ptr keeps the SurfaceDb alive for as long as a caller
// holds it, so a handle taken before a refresh stays valid. SurfaceDb's own
// const queries are thread-safe over an immutable manifest snapshot, so several
// workers may read one database concurrently with no additional locking; the
// mutex here guards only the map.

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/server/realm.hpp"
#include "atx/vol/surface_db.hpp"

namespace atx::server {

class SurfaceRegistry {
public:
  SurfaceRegistry(const Realm &realm, std::size_t partition_cache_capacity);

  SurfaceRegistry(const SurfaceRegistry &) = delete;
  SurfaceRegistry &operator=(const SurfaceRegistry &) = delete;
  SurfaceRegistry(SurfaceRegistry &&) = delete;
  SurfaceRegistry &operator=(SurfaceRegistry &&) = delete;

  // NotFound if db_id is not in the realm. IoError/ParseError propagate from
  // SurfaceDb::open.
  [[nodiscard]] atx::core::Result<std::shared_ptr<atx::vol::SurfaceDb>>
  get(std::string_view db_id);

  [[nodiscard]] std::vector<std::string> open_db_ids() const;
  [[nodiscard]] atx::core::Result<std::uint64_t> generation(std::string_view db_id);
  [[nodiscard]] atx::core::Status refresh(std::string_view db_id);
  [[nodiscard]] atx::core::Result<atx::vol::SurfaceDbCacheStats>
  cache_stats(std::string_view db_id);

private:
  const Realm &realm_;
  std::size_t partition_cache_capacity_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<atx::vol::SurfaceDb>> open_;
};

} // namespace atx::server
```

- [ ] **Step 6: Write `atx-server/src/surface_registry.cpp`**

```cpp
#include "atx/server/surface_registry.hpp"

#include <algorithm>

namespace atx::server {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

} // namespace

SurfaceRegistry::SurfaceRegistry(const Realm &realm, std::size_t partition_cache_capacity)
    : realm_{realm}, partition_cache_capacity_{partition_cache_capacity == 0
                                                   ? std::size_t{1}
                                                   : partition_cache_capacity} {}

atx::core::Result<std::shared_ptr<atx::vol::SurfaceDb>>
SurfaceRegistry::get(std::string_view db_id) {
  const std::string key{db_id};
  {
    const std::lock_guard<std::mutex> lock{mutex_};
    const auto it = open_.find(key);
    if (it != open_.end()) {
      return Ok(it->second);
    }
  }

  ATX_TRY(const RealmEntry entry, realm_.find(db_id));
  atx::vol::SurfaceDbOpenOpts opts;
  opts.partition_cache_capacity = partition_cache_capacity_;
  ATX_TRY(atx::vol::SurfaceDb db, atx::vol::SurfaceDb::open(entry.root, opts));
  auto shared = std::make_shared<atx::vol::SurfaceDb>(std::move(db));

  const std::lock_guard<std::mutex> lock{mutex_};
  // Two threads may both have missed the first lookup and both opened the
  // database. Whichever inserts first wins, and every caller ends up with the
  // same handle; the loser's SurfaceDb is destroyed here.
  const auto [it, inserted] = open_.emplace(key, std::move(shared));
  (void)inserted;
  return Ok(it->second);
}

std::vector<std::string> SurfaceRegistry::open_db_ids() const {
  std::vector<std::string> ids;
  const std::lock_guard<std::mutex> lock{mutex_};
  ids.reserve(open_.size());
  for (const auto &[id, db] : open_) {
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

atx::core::Result<std::uint64_t> SurfaceRegistry::generation(std::string_view db_id) {
  ATX_TRY(auto db, get(db_id));
  return Ok(db->generation());
}

atx::core::Status SurfaceRegistry::refresh(std::string_view db_id) {
  ATX_TRY(auto db, get(db_id));
  return db->refresh();
}

atx::core::Result<atx::vol::SurfaceDbCacheStats>
SurfaceRegistry::cache_stats(std::string_view db_id) {
  ATX_TRY(auto db, get(db_id));
  return Ok(db->partition_cache_stats());
}

} // namespace atx::server
```

- [ ] **Step 7: Write `atx-server/include/atx/server/encode.hpp` and `src/encode.cpp`**

```cpp
// include/atx/server/encode.hpp
#pragma once

// Domain -> wire. Pure free functions: no socket, no registry, no catalog.
//
// This is the load-bearing boundary of the server. Every translation is
// testable against a fixture SurfaceDb with no port bound and no service
// running, which is why the encode tests are the fast ones and the service
// tests only have to prove routing.
//
// This logic currently also exists in atx-ui/src/vol/spy_opra_surface.cpp. The
// UI spec deletes that copy when the UI switches to the remote source. The
// duplication is scoped and temporary and is called out so it is not mistaken
// for an oversight.

#include <cstdint>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/rpc/v1/surface.pb.h"
#include "atx/vol/priced_surface_view.hpp"
#include "atx/vol/surface_db.hpp"

namespace atx::server {

// Server-side bounds. A client cannot ask for an unbounded strike expansion.
inline constexpr std::uint32_t kDefaultCurvePoints = 81;
inline constexpr std::uint32_t kMaxCurvePoints = 1025;
inline constexpr double kDefaultZWindow = 2.0;
inline constexpr double kMaxZWindow = 8.0;

[[nodiscard]] atx::rpc::v1::SymbolFitConfig
encode_symbol_config(std::string_view symbol, const atx::vol::SymbolFitConfig &config);

[[nodiscard]] atx::rpc::v1::ExpirySummary
encode_expiry(const atx::vol::PricedSurfaceView &view, std::size_t expiry_index);

[[nodiscard]] atx::rpc::v1::SurfaceDiagnostics
encode_diagnostics(const atx::vol::PricedSurfaceView &view);

// Expiries in ascending time order: the term-structure panel plots them in
// array order and would otherwise draw a zig-zag.
[[nodiscard]] atx::core::Result<atx::rpc::v1::SurfaceMeta>
encode_surface_meta(const atx::vol::PricedSurfaceView &view);

// z_window == 0 and curve_points == 0 mean "server default", not "empty".
// Both are clamped to the constants above.
[[nodiscard]] atx::core::Result<atx::rpc::v1::VolCurveSlice>
encode_curve(const atx::vol::PricedSurfaceView &view, std::string_view expiry_iso,
             double z_window, std::uint32_t curve_points, bool include_quotes);

void stamp_meta(atx::rpc::v1::ResponseMeta &meta, std::uint64_t db_generation,
                std::uint64_t content_hash);

} // namespace atx::server
```

Implement `src/encode.cpp` against the real `PricedSurfaceView` accessors. Read `atx-vol/include/atx/vol/priced_surface_view.hpp` first and use the names it actually defines; do not invent accessors. Required behaviour:

- `encode_surface_meta` iterates every expiry, calls `encode_expiry` per index, sorts by `years` ascending, and fills `spot`, `model_kind`, `risk_state`, and `diagnostics`.
- `encode_curve` locates the expiry by ISO string, returning `Err(ErrorCode::NotFound, ...)` when absent. It clamps `z_window` to `(0, kMaxZWindow]` (0 → `kDefaultZWindow`) and `curve_points` to `[3, kMaxCurvePoints]` (0 → `kDefaultCurvePoints`), then evaluates the model on an even grid across `[-z_window, +z_window]`. Quotes are appended only when `include_quotes` is true.
- `stamp_meta` sets `db_generation`, `content_hash`, and `server_ns` from the system clock.

- [ ] **Step 8: Write `atx-server/src/catalog_refresh.cpp`**

```cpp
#include "atx/server/catalog_refresh.hpp"

#include <iostream>

#include "atx/server/catalog.hpp"
#include "atx/server/catalog_scan.hpp"
#include "atx/server/surface_registry.hpp"

namespace atx::server {

CatalogRefresher::CatalogRefresher(Catalog &catalog, SurfaceRegistry &registry,
                                   std::chrono::milliseconds interval)
    : catalog_{catalog}, registry_{registry}, interval_{interval} {}

CatalogRefresher::~CatalogRefresher() { stop(); }

void CatalogRefresher::start() {
  if (thread_.joinable()) {
    return;
  }
  stopping_.store(false);
  thread_ = std::thread{[this] { loop(); }};
}

void CatalogRefresher::stop() noexcept {
  stopping_.store(true);
  if (thread_.joinable()) {
    thread_.join();
  }
}

void CatalogRefresher::loop() {
  while (!stopping_.load()) {
    for (const std::string &db_id : registry_.open_db_ids()) {
      if (stopping_.load()) {
        return;
      }
      // Reading a generation is one manifest-header read. Rescanning is not, so
      // the poll is what runs every interval and the rescan is what runs only
      // on drift.
      auto on_disk = registry_.generation(db_id);
      auto indexed = catalog_.generation_of(db_id);
      if (!on_disk || !indexed || *on_disk == *indexed) {
        continue;
      }
      if (!registry_.refresh(db_id)) {
        continue;
      }
      auto db = registry_.get(db_id);
      if (!db) {
        continue;
      }
      auto index = scan_surface_db(**db);
      if (!index) {
        std::cerr << "[atx-server] rescan of '" << db_id
                  << "' failed: " << index.error().to_string() << '\n';
        continue;
      }
      // Re-indexes ONLY this db_id, in one transaction. db_source.generation is
      // exactly what every ResponseMeta.db_generation reports, so a client that
      // revalidates on generation is correct by construction.
      if (!catalog_.replace_db_index(db_id, *index)) {
        continue;
      }
      rescans_.fetch_add(1);
    }

    // Sleep in slices so stop() is responsive even with a long interval.
    const auto deadline = std::chrono::steady_clock::now() + interval_;
    while (!stopping_.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
  }
}

} // namespace atx::server
```

Add `#include <chrono>` to `catalog_refresh.cpp`.

- [ ] **Step 9: Add the sources to `atx-server/CMakeLists.txt`**

```cmake
add_library(atx-server-lib ${ATX_LIB_TYPE}
    src/version.cpp
    src/config.cpp
    src/realm.cpp
    src/auth.cpp
    src/catalog.cpp
    src/catalog_scan.cpp
    src/catalog_refresh.cpp
    src/surface_registry.cpp
    src/encode.cpp
)
```

- [ ] **Step 10: Run the tests to verify they pass**

Run: `cmake --build --preset server --target atx-server-tests`
Then: `ctest --preset server -R "^SurfaceRegistry\.|^Encode\.|^CatalogRefresher\." --output-on-failure`
Expected: 13 tests, all PASS.

- [ ] **Step 11: Commit**

```bash
git add atx-server/include/atx/server/surface_registry.hpp \
        atx-server/include/atx/server/encode.hpp \
        atx-server/src/surface_registry.cpp atx-server/src/encode.cpp \
        atx-server/src/catalog_refresh.cpp \
        atx-server/tests/surface_registry_test.cpp atx-server/tests/encode_test.cpp \
        atx-server/tests/catalog_refresh_test.cpp atx-server/CMakeLists.txt
git commit -m "feat(server): add the surface registry, the encoders, and the refresher

SurfaceRegistry opens a database on first use, not at startup, so a realm with
fifty registered databases does not pay fifty manifest reads and fifty mmap
caches to answer a question about one. Two threads that both miss the first
lookup may both open it; whichever inserts first wins and every caller gets the
same handle, which the concurrency test asserts by comparing pointers across
eight threads.

The returned shared_ptr keeps a SurfaceDb alive for as long as a caller holds
it, so a handle taken before a refresh stays valid. SurfaceDb's const queries
are already thread-safe over an immutable manifest snapshot, so the registry
mutex guards only the map.

encode.hpp is the load-bearing boundary: pure free functions with no socket, no
registry, and no catalog, so every domain-to-wire translation is testable
against a fixture database with nothing bound. The service tests then only have
to prove routing.

Curve requests are clamped server-side. z_window and curve_points of zero mean
'server default' rather than 'empty', and absurd values are bounded, so one
client cannot ask for an unbounded strike expansion. Expiries are sorted
ascending because the term-structure panel plots them in array order.

The refresher polls generations and rescans only on drift. Reading a generation
is one header read; rescanning is not. A test asserts a static database
accumulates zero rescans over three hundred milliseconds of polling, and
another asserts an external writer's bump is picked up.

This encode logic currently also exists in atx-ui/src/vol/spy_opra_surface.cpp.
The UI spec deletes that copy when the UI switches to the remote source; the
duplication is scoped, temporary, and documented in the header so it is not
mistaken for an oversight.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 13: `atx-server` error mapping and `AdminService`

**Files:**
- Create: `atx-server/include/atx/server/service_error.hpp`, `atx-server/src/service_error.cpp`
- Create: `atx-server/include/atx/server/methods.hpp`
- Create: `atx-server/include/atx/server/service_admin.hpp`, `atx-server/src/service_admin.cpp`
- Create: `atx-server/tests/service_error_test.cpp`, `atx-server/tests/service_admin_test.cpp`
- Modify: `atx-server/CMakeLists.txt`

**Interfaces:**
- Consumes: `MethodTable`, `Service`, `CallContext` (Task 5); `Catalog`, `Realm`, `SurfaceRegistry` (Tasks 9–12).
- Produces:
  - `v1::RpcCode atx::server::rpc_code_for(atx::core::ErrorCode)`
  - `v1::RpcStatus atx::server::status_from_error(const atx::core::Error&)`
  - `v1::RpcStatus atx::server::permission_denied(std::string_view db_id)`
  - Namespace `atx::server::methods` with `inline constexpr std::string_view kAdminHealth = "atx.rpc.v1.AdminService/Health"` and the rest; plus `inline constexpr std::array<std::string_view, 14> kAll{...}` listing every method exactly once.
  - `class atx::server::AdminServiceImpl : public atx::rpc::Service` with `AdminServiceImpl(Catalog&, Realm&, SurfaceRegistry&, ServerStatsSource)` where `using ServerStatsSource = std::function<atx::rpc::RpcServerStats()>`.

- [ ] **Step 1: Write the failing error-mapping test**

`atx-server/tests/service_error_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/server/service_error.hpp"

namespace {

using atx::core::Error;
using atx::core::ErrorCode;
using atx::server::permission_denied;
using atx::server::rpc_code_for;
using atx::server::status_from_error;
namespace v1 = atx::rpc::v1;

TEST(ServiceError, MapsEveryDomainCode) {
  EXPECT_EQ(rpc_code_for(ErrorCode::NotFound), v1::RPC_CODE_NOT_FOUND);
  EXPECT_EQ(rpc_code_for(ErrorCode::InvalidArgument), v1::RPC_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(rpc_code_for(ErrorCode::OutOfRange), v1::RPC_CODE_RESOURCE_EXHAUSTED);
  EXPECT_EQ(rpc_code_for(ErrorCode::PermissionDenied), v1::RPC_CODE_PERMISSION_DENIED);
  EXPECT_EQ(rpc_code_for(ErrorCode::AlreadyExists), v1::RPC_CODE_FAILED_PRECONDITION);
  EXPECT_EQ(rpc_code_for(ErrorCode::Unavailable), v1::RPC_CODE_UNAVAILABLE);
  EXPECT_EQ(rpc_code_for(ErrorCode::NotImplemented), v1::RPC_CODE_UNIMPLEMENTED);
  EXPECT_EQ(rpc_code_for(ErrorCode::Internal), v1::RPC_CODE_INTERNAL);
  EXPECT_EQ(rpc_code_for(ErrorCode::Unknown), v1::RPC_CODE_INTERNAL);
}

// A corrupt archive is DATA_LOSS, not a generic internal error: the client
// should stop retrying and the operator should look at the file.
TEST(ServiceError, ParseErrorIsDataLoss) {
  EXPECT_EQ(rpc_code_for(ErrorCode::ParseError), v1::RPC_CODE_DATA_LOSS);
}

// IoError is UNAVAILABLE rather than INTERNAL so a client backs off and retries
// instead of treating a transient disk problem as a permanent server fault.
TEST(ServiceError, IoErrorIsUnavailable) {
  EXPECT_EQ(rpc_code_for(ErrorCode::IoError), v1::RPC_CODE_UNAVAILABLE);
}

TEST(ServiceError, StatusCarriesTheDomainMessage) {
  const v1::RpcStatus status =
      status_from_error(Error{ErrorCode::NotFound, "no surface for SPY on 2024-03-15"});
  EXPECT_EQ(status.code(), v1::RPC_CODE_NOT_FOUND);
  EXPECT_NE(status.message().find("no surface for SPY"), std::string::npos);
  EXPECT_TRUE(status.incident_id().empty()) << "incident ids belong to INTERNAL only";
}

// Not NOT_FOUND. Realm ids are not secrets, and masking an entitlement gap as
// absence makes a misconfiguration undebuggable from the client side.
TEST(ServiceError, UnentitledDbIsPermissionDeniedAndNamesTheId) {
  const v1::RpcStatus status = permission_denied("spy_prod");
  EXPECT_EQ(status.code(), v1::RPC_CODE_PERMISSION_DENIED);
  EXPECT_NE(status.message().find("spy_prod"), std::string::npos);
}

} // namespace
```

- [ ] **Step 2: Write the failing admin-service test**

`atx-server/tests/service_admin_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "atx/rpc/dispatcher.hpp"
#include "atx/rpc/method_table.hpp"
#include "atx/server/catalog.hpp"
#include "atx/server/methods.hpp"
#include "atx/server/service_admin.hpp"
#include "atx/server/surface_registry.hpp"
#include "atx/test/scratch.hpp"
#include "support/fixture_db.hpp"

namespace {

using atx::rpc::CallContext;
using atx::rpc::Entitlements;
using atx::rpc::MethodTable;
using atx::server::AdminServiceImpl;
using atx::server::Catalog;
using atx::server::Realm;
using atx::server::RealmEntry;
using atx::server::SurfaceRegistry;
using atx::server::test::make_fixture_db;
namespace v1 = atx::rpc::v1;
namespace methods = atx::server::methods;

struct Harness {
  std::unique_ptr<Catalog> catalog;
  Realm realm;
  std::unique_ptr<SurfaceRegistry> registry;
  std::unique_ptr<AdminServiceImpl> service;
  MethodTable table;
  std::string fixture_root;
};

Harness make_harness(const std::string &name) {
  Harness harness;
  auto fixture = make_fixture_db(name);
  EXPECT_TRUE(fixture);
  harness.fixture_root = fixture->root;

  RealmEntry entry;
  entry.db_id = "fixture";
  entry.kind = "surface_db";
  entry.root = fixture->root;
  EXPECT_TRUE(harness.realm.add(entry));

  const std::filesystem::path dir = atx::test::scratch_dir("atx-server-admin-" + name);
  auto catalog = Catalog::open((dir / "state.db").string());
  EXPECT_TRUE(catalog);
  harness.catalog = std::move(*catalog);
  EXPECT_TRUE(harness.catalog->upsert_realm_entry(entry));

  harness.registry = std::make_unique<SurfaceRegistry>(harness.realm, 4);
  harness.service = std::make_unique<AdminServiceImpl>(
      *harness.catalog, harness.realm, *harness.registry,
      [] { return atx::rpc::RpcServerStats{}; });
  harness.service->register_methods(harness.table);
  return harness;
}

CallContext entitled_context() {
  CallContext ctx;
  ctx.entitlements.authenticated = true;
  ctx.entitlements.token_label = "test";
  ctx.entitlements.readable_db_ids = {"fixture"};
  return ctx;
}

template <class Req, class Resp>
v1::RpcStatus call(const MethodTable &table, std::string_view method, const CallContext &ctx,
                   const Req &request, Resp &response) {
  std::string out;
  const v1::RpcStatus status = table.invoke(method, ctx, request.SerializeAsString(), out);
  if (status.code() == v1::RPC_CODE_OK) {
    EXPECT_TRUE(response.ParseFromString(out));
  }
  return status;
}

TEST(AdminService, RegistersEveryDeclaredMethod) {
  Harness harness = make_harness("register");
  for (const std::string_view method :
       {methods::kAdminHealth, methods::kAdminGetServerInfo, methods::kAdminGetStats,
        methods::kAdminRegisterDatabase, methods::kAdminExportRealm}) {
    EXPECT_TRUE(harness.table.contains(method)) << method;
  }
}

TEST(AdminService, HealthReportsServing) {
  Harness harness = make_harness("health");
  v1::HealthRequest request;
  v1::HealthResponse response;
  const v1::RpcStatus status =
      call(harness.table, methods::kAdminHealth, entitled_context(), request, response);
  EXPECT_EQ(status.code(), v1::RPC_CODE_OK);
  EXPECT_TRUE(response.serving());
}

TEST(AdminService, ServerInfoCarriesVersionAndSchemaHashes) {
  Harness harness = make_harness("info");
  v1::GetServerInfoRequest request;
  v1::ServerInfo response;
  const v1::RpcStatus status =
      call(harness.table, methods::kAdminGetServerInfo, entitled_context(), request, response);
  ASSERT_EQ(status.code(), v1::RPC_CODE_OK);
  EXPECT_FALSE(response.version().empty());
  EXPECT_FALSE(response.server_uuid().empty());
  EXPECT_NE(response.atxvsa_schema_hash(), 0u);
}

TEST(AdminService, ExportRealmReturnsRegisteredDatabases) {
  Harness harness = make_harness("export");
  v1::ExportRealmRequest request;
  v1::RealmConfig response;
  const v1::RpcStatus status =
      call(harness.table, methods::kAdminExportRealm, entitled_context(), request, response);
  ASSERT_EQ(status.code(), v1::RPC_CODE_OK);
  ASSERT_EQ(response.entries_size(), 1);
  EXPECT_EQ(response.entries(0).db_id(), "fixture");
}

TEST(AdminService, RegisterDatabaseAddsToTheRealmAndTheCatalog) {
  Harness harness = make_harness("register-db");
  auto second = make_fixture_db("register-db-second");
  ASSERT_TRUE(second);

  v1::RegisterDatabaseRequest request;
  request.set_db_id("second");
  request.set_kind("surface_db");
  request.set_root(second->root);
  v1::RegisterDatabaseResponse response;
  const v1::RpcStatus status = call(harness.table, methods::kAdminRegisterDatabase,
                                    entitled_context(), request, response);
  ASSERT_EQ(status.code(), v1::RPC_CODE_OK) << status.message();
  EXPECT_TRUE(response.registered());

  auto realm = harness.catalog->load_realm();
  ASSERT_TRUE(realm);
  EXPECT_TRUE(realm->contains("second"));
}

TEST(AdminService, RegisterDatabaseRejectsADuplicateId) {
  Harness harness = make_harness("register-dup");
  v1::RegisterDatabaseRequest request;
  request.set_db_id("fixture");
  request.set_kind("surface_db");
  request.set_root(harness.fixture_root);
  v1::RegisterDatabaseResponse response;
  const v1::RpcStatus status = call(harness.table, methods::kAdminRegisterDatabase,
                                    entitled_context(), request, response);
  EXPECT_EQ(status.code(), v1::RPC_CODE_FAILED_PRECONDITION);
}

TEST(AdminService, RegisterDatabaseRejectsAnUnopenableRoot) {
  Harness harness = make_harness("register-bad-root");
  v1::RegisterDatabaseRequest request;
  request.set_db_id("broken");
  request.set_kind("surface_db");
  request.set_root("C:/definitely/not/a/surface/db");
  v1::RegisterDatabaseResponse response;
  const v1::RpcStatus status = call(harness.table, methods::kAdminRegisterDatabase,
                                    entitled_context(), request, response);
  // Registering a root the server cannot open would leave a db_id every client
  // sees and no client can use.
  EXPECT_NE(status.code(), v1::RPC_CODE_OK);
  auto realm = harness.catalog->load_realm();
  ASSERT_TRUE(realm);
  EXPECT_FALSE(realm->contains("broken"));
}

TEST(Methods, EveryNameIsUniqueAndFullyQualified) {
  std::set<std::string_view> seen;
  for (const std::string_view name : methods::kAll) {
    EXPECT_TRUE(seen.insert(name).second) << "duplicate method name " << name;
    EXPECT_EQ(name.rfind("atx.rpc.v1.", 0), 0u) << name;
    EXPECT_NE(name.find('/'), std::string_view::npos) << name;
  }
  EXPECT_EQ(seen.size(), methods::kAll.size());
}

} // namespace
```

Add `#include <set>` at the top.

- [ ] **Step 3: Run both tests to verify they fail**

Run: `cmake --build --preset server --target atx-server-tests`
Expected: FAIL at compile — `atx/server/service_error.hpp: No such file or directory`.

- [ ] **Step 4: Write `atx-server/include/atx/server/methods.hpp`**

```cpp
#pragma once

// The single source of truth for method strings. Registration code and the
// CLI both reference these constants, and the agreement test checks kAll
// against what the services actually register -- a name that exists in one
// place and not the other is a silent UNIMPLEMENTED in production.

#include <array>
#include <string_view>

namespace atx::server::methods {

inline constexpr std::string_view kAdminHealth = "atx.rpc.v1.AdminService/Health";
inline constexpr std::string_view kAdminGetServerInfo = "atx.rpc.v1.AdminService/GetServerInfo";
inline constexpr std::string_view kAdminGetStats = "atx.rpc.v1.AdminService/GetStats";
inline constexpr std::string_view kAdminRegisterDatabase =
    "atx.rpc.v1.AdminService/RegisterDatabase";
inline constexpr std::string_view kAdminExportRealm = "atx.rpc.v1.AdminService/ExportRealm";

inline constexpr std::string_view kSurfaceListDatabases =
    "atx.rpc.v1.SurfaceService/ListDatabases";
inline constexpr std::string_view kSurfaceListSymbols = "atx.rpc.v1.SurfaceService/ListSymbols";
inline constexpr std::string_view kSurfaceListPartitions =
    "atx.rpc.v1.SurfaceService/ListPartitions";
inline constexpr std::string_view kSurfaceListSurfaces =
    "atx.rpc.v1.SurfaceService/ListSurfaces";
inline constexpr std::string_view kSurfaceGetCoverage = "atx.rpc.v1.SurfaceService/GetCoverage";
inline constexpr std::string_view kSurfaceGetSymbolConfig =
    "atx.rpc.v1.SurfaceService/GetSymbolConfig";
inline constexpr std::string_view kSurfaceGetSurfaceMeta =
    "atx.rpc.v1.SurfaceService/GetSurfaceMeta";
inline constexpr std::string_view kSurfaceGetCurve = "atx.rpc.v1.SurfaceService/GetCurve";
inline constexpr std::string_view kSurfaceGetSurfaceBlob =
    "atx.rpc.v1.SurfaceService/GetSurfaceBlob";

inline constexpr std::array<std::string_view, 14> kAll{
    kAdminHealth,            kAdminGetServerInfo,     kAdminGetStats,
    kAdminRegisterDatabase,  kAdminExportRealm,       kSurfaceListDatabases,
    kSurfaceListSymbols,     kSurfaceListPartitions,  kSurfaceListSurfaces,
    kSurfaceGetCoverage,     kSurfaceGetSymbolConfig, kSurfaceGetSurfaceMeta,
    kSurfaceGetCurve,        kSurfaceGetSurfaceBlob};

} // namespace atx::server::methods
```

- [ ] **Step 5: Write `atx-server/include/atx/server/service_error.hpp` and `src/service_error.cpp`**

```cpp
// include/atx/server/service_error.hpp
#pragma once

#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/rpc/v1/envelope.pb.h"

namespace atx::server {

// One table, so every handler reports the same domain failure the same way.
[[nodiscard]] atx::rpc::v1::RpcCode rpc_code_for(atx::core::ErrorCode code) noexcept;

[[nodiscard]] atx::rpc::v1::RpcStatus status_from_error(const atx::core::Error &error);

// PERMISSION_DENIED and it names the id. NOT_FOUND would be worse: realm ids
// are not secrets, and masking an entitlement gap as absence makes a
// misconfiguration undebuggable from the client side.
[[nodiscard]] atx::rpc::v1::RpcStatus permission_denied(std::string_view db_id);

} // namespace atx::server
```

```cpp
// src/service_error.cpp
#include "atx/server/service_error.hpp"

namespace atx::server {

atx::rpc::v1::RpcCode rpc_code_for(atx::core::ErrorCode code) noexcept {
  using atx::core::ErrorCode;
  namespace v1 = atx::rpc::v1;
  switch (code) {
  case ErrorCode::NotFound:
    return v1::RPC_CODE_NOT_FOUND;
  case ErrorCode::InvalidArgument:
    return v1::RPC_CODE_INVALID_ARGUMENT;
  case ErrorCode::OutOfRange:
    // A limit was exceeded. The message carries the actual size so a client can
    // decide whether to narrow the request or give up.
    return v1::RPC_CODE_RESOURCE_EXHAUSTED;
  case ErrorCode::PermissionDenied:
    return v1::RPC_CODE_PERMISSION_DENIED;
  case ErrorCode::AlreadyExists:
    return v1::RPC_CODE_FAILED_PRECONDITION;
  case ErrorCode::Unavailable:
  case ErrorCode::IoError:
    // Retryable. A transient disk problem is not a permanent server fault, and
    // reporting it as one would stop a client that should back off and retry.
    return v1::RPC_CODE_UNAVAILABLE;
  case ErrorCode::ParseError:
    // A CRC or schema-hash failure on an archive or manifest. DATA_LOSS tells
    // the client to stop retrying and the operator to look at the file.
    return v1::RPC_CODE_DATA_LOSS;
  case ErrorCode::NotImplemented:
    return v1::RPC_CODE_UNIMPLEMENTED;
  case ErrorCode::Internal:
  case ErrorCode::Unknown:
    return v1::RPC_CODE_INTERNAL;
  }
  return v1::RPC_CODE_INTERNAL;
}

atx::rpc::v1::RpcStatus status_from_error(const atx::core::Error &error) {
  atx::rpc::v1::RpcStatus status;
  status.set_code(rpc_code_for(error.code()));
  status.set_message(error.to_string());
  return status;
}

atx::rpc::v1::RpcStatus permission_denied(std::string_view db_id) {
  atx::rpc::v1::RpcStatus status;
  status.set_code(atx::rpc::v1::RPC_CODE_PERMISSION_DENIED);
  status.set_message("this token is not entitled to database '" + std::string{db_id} + "'");
  return status;
}

} // namespace atx::server
```

- [ ] **Step 6: Write `atx-server/include/atx/server/service_admin.hpp` and `src/service_admin.cpp`**

The header declares `AdminServiceImpl` deriving from `atx::rpc::Service`, holding references to `Catalog`, `Realm`, `SurfaceRegistry`, a `ServerStatsSource` callable, and a start timestamp. `register_methods` registers the five admin methods from `methods.hpp`. Handler behaviour:

- **Health** — `serving = true`, `uptime_ns` from the start timestamp.
- **GetServerInfo** — `version_string()`, realm id, uptime, `atx::vol::kSurfaceDbMajor`/`kSurfaceDbMinor` folded into `manifest_schema_hash`, the ATXVSA schema hash from `atx-vol`, and `catalog.server_uuid()`.
- **GetStats** — the `RpcServerStats` from the callable, plus one `DatabaseCacheStats` per open `db_id` from `SurfaceRegistry::cache_stats` and `generation`.
- **RegisterDatabase** — validate kind, then **open the database before registering it**: `SurfaceDb::open(root)` must succeed, otherwise return `status_from_error` and register nothing. On success, `realm.add`, `catalog.upsert_realm_entry`, `scan_surface_db`, `catalog.replace_db_index`, and report the generation. A duplicate id surfaces as `FAILED_PRECONDITION` via `AlreadyExists`.
- **ExportRealm** — `realm.to_config(realm_id)`.

`Realm` must gain a `realm_id` the server sets at construction; add `void set_realm_id(std::string)` and `std::string_view realm_id() const noexcept` to `realm.hpp`, defaulting to `"default"`.

- [ ] **Step 7: Add the sources to `atx-server/CMakeLists.txt`**

```cmake
    src/service_error.cpp
    src/service_admin.cpp
```

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cmake --build --preset server --target atx-server-tests`
Then: `ctest --preset server -R "^ServiceError\.|^AdminService\.|^Methods\." --output-on-failure`
Expected: 13 tests, all PASS.

- [ ] **Step 9: Commit**

```bash
git add atx-server/include/atx/server/service_error.hpp \
        atx-server/include/atx/server/methods.hpp \
        atx-server/include/atx/server/service_admin.hpp \
        atx-server/include/atx/server/realm.hpp \
        atx-server/src/service_error.cpp atx-server/src/service_admin.cpp \
        atx-server/src/realm.cpp \
        atx-server/tests/service_error_test.cpp atx-server/tests/service_admin_test.cpp \
        atx-server/CMakeLists.txt
git commit -m "feat(server): add the error mapping table and AdminService

One mapping table so every handler reports the same domain failure the same way.
The choices that are not obvious are the ones worth stating: IoError is
UNAVAILABLE rather than INTERNAL so a client backs off instead of treating a
transient disk problem as permanent, and ParseError is DATA_LOSS because a CRC
or schema-hash failure means stop retrying and go look at the file.

An unentitled db_id is PERMISSION_DENIED and the message names the id. NOT_FOUND
would be worse: realm ids are not secrets, and masking an entitlement gap as
absence makes a misconfiguration undebuggable from the client side.

RegisterDatabase opens the database before registering it. Registering a root
the server cannot open would leave a db_id that every client can see and no
client can use; the test asserts a bad root leaves the realm untouched.

Method strings live in one header. Registration code and the CLI both reference
those constants, and the agreement test checks the declared list against what
the services actually register, because a name that exists in one place and not
the other is a silent UNIMPLEMENTED in production.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 14: `atx-server` SurfaceService read RPCs

Eight of the nine methods. `GetSurfaceBlob` lands separately in Task 15 because its fidelity test is the crown jewel of this spec and deserves its own review gate.

**Files:**
- Create: `atx-server/include/atx/server/service_surface.hpp`, `atx-server/src/service_surface.cpp`
- Create: `atx-server/tests/service_surface_test.cpp`
- Modify: `atx-server/CMakeLists.txt`

**Interfaces:**
- Consumes: `Catalog`, `SurfaceRegistry`, `encode_*`, `status_from_error`, `permission_denied`, `methods::*`.
- Produces: `class atx::server::SurfaceServiceImpl : public atx::rpc::Service` with `SurfaceServiceImpl(Catalog&, SurfaceRegistry&, const ServerConfig&)`. Registers all nine method names; `GetSurfaceBlob`'s handler returns `UNIMPLEMENTED` until Task 15 fills it in.

- [ ] **Step 1: Write the failing test**

`atx-server/tests/service_surface_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "atx/rpc/method_table.hpp"
#include "atx/server/catalog.hpp"
#include "atx/server/catalog_scan.hpp"
#include "atx/server/config.hpp"
#include "atx/server/methods.hpp"
#include "atx/server/service_surface.hpp"
#include "atx/server/surface_registry.hpp"
#include "atx/test/scratch.hpp"
#include "support/fixture_db.hpp"

namespace {

using atx::rpc::CallContext;
using atx::rpc::MethodTable;
using atx::server::Catalog;
using atx::server::Realm;
using atx::server::RealmEntry;
using atx::server::ServerConfig;
using atx::server::SurfaceRegistry;
using atx::server::SurfaceServiceImpl;
using atx::server::test::make_fixture_db;
namespace v1 = atx::rpc::v1;
namespace methods = atx::server::methods;

struct Harness {
  std::unique_ptr<Catalog> catalog;
  Realm realm;
  std::unique_ptr<SurfaceRegistry> registry;
  ServerConfig config;
  std::unique_ptr<SurfaceServiceImpl> service;
  MethodTable table;
};

std::unique_ptr<Harness> make_harness(const std::string &name) {
  auto harness = std::make_unique<Harness>();
  auto fixture = make_fixture_db(name);
  EXPECT_TRUE(fixture);

  RealmEntry entry;
  entry.db_id = "fixture";
  entry.kind = "surface_db";
  entry.root = fixture->root;
  EXPECT_TRUE(harness->realm.add(entry));

  const std::filesystem::path dir = atx::test::scratch_dir("atx-server-surface-" + name);
  auto catalog = Catalog::open((dir / "state.db").string());
  EXPECT_TRUE(catalog);
  harness->catalog = std::move(*catalog);
  EXPECT_TRUE(harness->catalog->upsert_realm_entry(entry));

  harness->registry = std::make_unique<SurfaceRegistry>(harness->realm, 4);
  auto db = harness->registry->get("fixture");
  EXPECT_TRUE(db);
  auto index = atx::server::scan_surface_db(**db);
  EXPECT_TRUE(index);
  EXPECT_TRUE(harness->catalog->replace_db_index("fixture", *index));

  harness->service = std::make_unique<SurfaceServiceImpl>(
      *harness->catalog, *harness->registry, harness->config);
  harness->service->register_methods(harness->table);
  return harness;
}

CallContext context_for(std::vector<std::string> db_ids) {
  CallContext ctx;
  ctx.entitlements.authenticated = true;
  ctx.entitlements.token_label = "test";
  ctx.entitlements.readable_db_ids = std::move(db_ids);
  return ctx;
}

template <class Req, class Resp>
v1::RpcStatus call(const MethodTable &table, std::string_view method, const CallContext &ctx,
                   const Req &request, Resp &response) {
  std::string out;
  const v1::RpcStatus status = table.invoke(method, ctx, request.SerializeAsString(), out);
  if (status.code() == v1::RPC_CODE_OK) {
    EXPECT_TRUE(response.ParseFromString(out));
  }
  return status;
}

TEST(SurfaceService, RegistersEveryDeclaredMethod) {
  auto harness = make_harness("register");
  for (const std::string_view method :
       {methods::kSurfaceListDatabases, methods::kSurfaceListSymbols,
        methods::kSurfaceListPartitions, methods::kSurfaceListSurfaces,
        methods::kSurfaceGetCoverage, methods::kSurfaceGetSymbolConfig,
        methods::kSurfaceGetSurfaceMeta, methods::kSurfaceGetCurve,
        methods::kSurfaceGetSurfaceBlob}) {
    EXPECT_TRUE(harness->table.contains(method)) << method;
  }
}

TEST(SurfaceService, ListDatabasesReturnsOnlyEntitledIds) {
  auto harness = make_harness("list-db");
  v1::ListDatabasesRequest request;

  v1::ListDatabasesResponse entitled;
  ASSERT_EQ(call(harness->table, methods::kSurfaceListDatabases, context_for({"fixture"}),
                 request, entitled)
                .code(),
            v1::RPC_CODE_OK);
  ASSERT_EQ(entitled.databases_size(), 1);
  EXPECT_EQ(entitled.databases(0).db_id(), "fixture");

  v1::ListDatabasesResponse unentitled;
  ASSERT_EQ(call(harness->table, methods::kSurfaceListDatabases, context_for({}), request,
                 unentitled)
                .code(),
            v1::RPC_CODE_OK);
  EXPECT_EQ(unentitled.databases_size(), 0)
      << "an unentitled token must not learn that the database exists from this call";
}

// The entitlement gate on every data RPC, not just the listing one.
TEST(SurfaceService, UnentitledDbIsPermissionDeniedOnEveryDataRpc) {
  auto harness = make_harness("unentitled");
  const CallContext ctx = context_for({"something-else"});

  v1::ListSymbolsRequest symbols;
  symbols.set_db_id("fixture");
  v1::ListSymbolsResponse symbols_out;
  EXPECT_EQ(call(harness->table, methods::kSurfaceListSymbols, ctx, symbols, symbols_out).code(),
            v1::RPC_CODE_PERMISSION_DENIED);

  v1::GetSurfaceMetaRequest meta;
  meta.mutable_key()->mutable_partition()->set_db_id("fixture");
  meta.mutable_key()->mutable_partition()->set_key("2024-03-14");
  meta.mutable_key()->mutable_symbol()->set_symbol("SPY");
  v1::SurfaceMeta meta_out;
  EXPECT_EQ(call(harness->table, methods::kSurfaceGetSurfaceMeta, ctx, meta, meta_out).code(),
            v1::RPC_CODE_PERMISSION_DENIED);

  v1::GetCurveRequest curve;
  *curve.mutable_key() = meta.key();
  curve.set_expiry_iso("2024-06-21");
  v1::VolCurveSlice curve_out;
  EXPECT_EQ(call(harness->table, methods::kSurfaceGetCurve, ctx, curve, curve_out).code(),
            v1::RPC_CODE_PERMISSION_DENIED);
}

TEST(SurfaceService, ListSymbolsAndPartitionsComeFromTheCatalog) {
  auto harness = make_harness("list-symbols");
  const CallContext ctx = context_for({"fixture"});

  v1::ListSymbolsRequest symbols;
  symbols.set_db_id("fixture");
  v1::ListSymbolsResponse symbols_out;
  ASSERT_EQ(call(harness->table, methods::kSurfaceListSymbols, ctx, symbols, symbols_out).code(),
            v1::RPC_CODE_OK);
  EXPECT_EQ(symbols_out.symbols_size(), 2);

  v1::ListPartitionsRequest partitions;
  partitions.set_db_id("fixture");
  v1::ListPartitionsResponse partitions_out;
  ASSERT_EQ(
      call(harness->table, methods::kSurfaceListPartitions, ctx, partitions, partitions_out)
          .code(),
      v1::RPC_CODE_OK);
  EXPECT_EQ(partitions_out.partitions_size(), 2);
}

TEST(SurfaceService, ListSurfacesFiltersBySymbolAndKeyRange) {
  auto harness = make_harness("list-surfaces");
  const CallContext ctx = context_for({"fixture"});

  v1::ListSurfacesRequest all;
  all.set_db_id("fixture");
  v1::ListSurfacesResponse all_out;
  ASSERT_EQ(call(harness->table, methods::kSurfaceListSurfaces, ctx, all, all_out).code(),
            v1::RPC_CODE_OK);
  EXPECT_EQ(all_out.surfaces_size(), 3);

  v1::ListSurfacesRequest spy;
  spy.set_db_id("fixture");
  spy.set_symbol("SPY");
  v1::ListSurfacesResponse spy_out;
  ASSERT_EQ(call(harness->table, methods::kSurfaceListSurfaces, ctx, spy, spy_out).code(),
            v1::RPC_CODE_OK);
  EXPECT_EQ(spy_out.surfaces_size(), 2);

  v1::ListSurfacesRequest ranged;
  ranged.set_db_id("fixture");
  ranged.set_key_begin("2024-03-15");
  v1::ListSurfacesResponse ranged_out;
  ASSERT_EQ(call(harness->table, methods::kSurfaceListSurfaces, ctx, ranged, ranged_out).code(),
            v1::RPC_CODE_OK);
  EXPECT_EQ(ranged_out.surfaces_size(), 1);
}

// The reason the catalog exists. AAPL is configured but absent from the second
// partition, and the response must say so rather than silently omitting it.
TEST(SurfaceService, CoverageDistinguishesFittedFromConfiguredNotFitted) {
  auto harness = make_harness("coverage");
  v1::GetCoverageRequest request;
  request.set_db_id("fixture");
  v1::GetCoverageResponse response;
  ASSERT_EQ(call(harness->table, methods::kSurfaceGetCoverage, context_for({"fixture"}),
                 request, response)
                .code(),
            v1::RPC_CODE_OK);

  EXPECT_EQ(response.symbols_size(), 2);
  EXPECT_EQ(response.partition_keys_size(), 2);

  bool saw_fitted = false;
  bool saw_gap = false;
  for (const v1::CoverageCell &cell : response.cells()) {
    if (cell.symbol() == "SPY") {
      EXPECT_EQ(cell.state(), v1::COVERAGE_STATE_FITTED);
      saw_fitted = true;
    }
    if (cell.symbol() == "AAPL" && cell.partition_key() == "2024-03-15") {
      EXPECT_EQ(cell.state(), v1::COVERAGE_STATE_CONFIGURED_NOT_FITTED);
      saw_gap = true;
    }
  }
  EXPECT_TRUE(saw_fitted);
  EXPECT_TRUE(saw_gap);
}

TEST(SurfaceService, GetSurfaceMetaMatchesALocalRead) {
  auto harness = make_harness("meta");
  v1::GetSurfaceMetaRequest request;
  request.mutable_key()->mutable_partition()->set_db_id("fixture");
  request.mutable_key()->mutable_partition()->set_key("2024-03-14");
  request.mutable_key()->mutable_symbol()->set_symbol("SPY");
  v1::SurfaceMeta response;
  ASSERT_EQ(call(harness->table, methods::kSurfaceGetSurfaceMeta, context_for({"fixture"}),
                 request, response)
                .code(),
            v1::RPC_CODE_OK);

  auto db = harness->registry->get("fixture");
  ASSERT_TRUE(db);
  auto loaded = (*db)->map_surface("2024-03-14", "SPY");
  ASSERT_TRUE(loaded);
  auto local = atx::server::encode_surface_meta(**loaded);
  ASSERT_TRUE(local);

  EXPECT_EQ(response.expiries_size(), local->expiries_size());
  EXPECT_DOUBLE_EQ(response.spot(), local->spot());
  EXPECT_EQ(response.meta().db_generation(), (*db)->generation());
}

TEST(SurfaceService, GetCurveMatchesALocallyComputedCurve) {
  auto harness = make_harness("curve");
  auto db = harness->registry->get("fixture");
  ASSERT_TRUE(db);
  auto loaded = (*db)->map_surface("2024-03-14", "SPY");
  ASSERT_TRUE(loaded);
  auto local_meta = atx::server::encode_surface_meta(**loaded);
  ASSERT_TRUE(local_meta);
  ASSERT_GT(local_meta->expiries_size(), 0);
  const std::string expiry = local_meta->expiries(0).expiry_iso();

  v1::GetCurveRequest request;
  request.mutable_key()->mutable_partition()->set_db_id("fixture");
  request.mutable_key()->mutable_partition()->set_key("2024-03-14");
  request.mutable_key()->mutable_symbol()->set_symbol("SPY");
  request.set_expiry_iso(expiry);
  request.set_z_window(2.0);
  request.set_curve_points(41);
  v1::VolCurveSlice response;
  ASSERT_EQ(call(harness->table, methods::kSurfaceGetCurve, context_for({"fixture"}), request,
                 response)
                .code(),
            v1::RPC_CODE_OK);

  auto local = atx::server::encode_curve(**loaded, expiry, 2.0, 41, false);
  ASSERT_TRUE(local);
  ASSERT_EQ(response.curve_size(), local->curve_size());
  for (int i = 0; i < response.curve_size(); ++i) {
    EXPECT_DOUBLE_EQ(response.curve(i).z(), local->curve(i).z()) << "point " << i;
    EXPECT_DOUBLE_EQ(response.curve(i).model_iv(), local->curve(i).model_iv()) << "point " << i;
  }
}

TEST(SurfaceService, MissingSurfaceIsNotFound) {
  auto harness = make_harness("missing");
  v1::GetSurfaceMetaRequest request;
  request.mutable_key()->mutable_partition()->set_db_id("fixture");
  request.mutable_key()->mutable_partition()->set_key("2024-03-15");
  request.mutable_key()->mutable_symbol()->set_symbol("AAPL"); // configured, not fitted
  v1::SurfaceMeta response;
  EXPECT_EQ(call(harness->table, methods::kSurfaceGetSurfaceMeta, context_for({"fixture"}),
                 request, response)
                .code(),
            v1::RPC_CODE_NOT_FOUND);
}

TEST(SurfaceService, UnknownDatabaseIsNotFoundWhenEntitled) {
  auto harness = make_harness("unknown-db");
  v1::ListSymbolsRequest request;
  request.set_db_id("ghost");
  v1::ListSymbolsResponse response;
  // Entitled to "ghost" but it is not in the realm: NOT_FOUND, distinct from
  // the PERMISSION_DENIED an unentitled id would get.
  EXPECT_EQ(
      call(harness->table, methods::kSurfaceListSymbols, context_for({"ghost"}), request,
           response)
          .code(),
      v1::RPC_CODE_NOT_FOUND);
}

TEST(SurfaceService, EveryResponseStampsTheGeneration) {
  auto harness = make_harness("stamp");
  const CallContext ctx = context_for({"fixture"});
  auto db = harness->registry->get("fixture");
  ASSERT_TRUE(db);
  const std::uint64_t generation = (*db)->generation();

  v1::ListSymbolsRequest symbols;
  symbols.set_db_id("fixture");
  v1::ListSymbolsResponse symbols_out;
  ASSERT_EQ(call(harness->table, methods::kSurfaceListSymbols, ctx, symbols, symbols_out).code(),
            v1::RPC_CODE_OK);
  EXPECT_EQ(symbols_out.meta().db_generation(), generation);
  EXPECT_GT(symbols_out.meta().server_ns(), 0);
}

} // namespace
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset server --target atx-server-tests`
Expected: FAIL at compile — `atx/server/service_surface.hpp: No such file or directory`.

- [ ] **Step 3: Write `atx-server/include/atx/server/service_surface.hpp`**

```cpp
#pragma once

// SurfaceServiceImpl — the read path.
//
// Every handler follows the same three steps, in this order:
//   1. entitlement check on db_id  -> PERMISSION_DENIED (never NOT_FOUND)
//   2. catalog or registry lookup  -> NOT_FOUND / DATA_LOSS / UNAVAILABLE
//   3. encode + stamp ResponseMeta
//
// The order matters: checking entitlement before existence means an unentitled
// caller cannot probe which db_ids are real by comparing error codes.

#include "atx/core/error.hpp"
#include "atx/rpc/service.hpp"
#include "atx/server/catalog.hpp"
#include "atx/server/config.hpp"
#include "atx/server/encode.hpp"
#include "atx/server/surface_registry.hpp"

namespace atx::server {

class SurfaceServiceImpl final : public atx::rpc::Service {
public:
  SurfaceServiceImpl(Catalog &catalog, SurfaceRegistry &registry, const ServerConfig &config);

  [[nodiscard]] std::string_view name() const noexcept override {
    return "atx.rpc.v1.SurfaceService";
  }

  void register_methods(atx::rpc::MethodTable &table) override;

private:
  Catalog &catalog_;
  SurfaceRegistry &registry_;
  const ServerConfig &config_;
};

} // namespace atx::server
```

- [ ] **Step 4: Write `atx-server/src/service_surface.cpp`**

Register all nine methods. Each handler begins with the shared gate:

```cpp
// Shared preamble for every db_id-scoped handler. Entitlement first, existence
// second: reversing them would let an unentitled caller enumerate real db_ids
// by comparing NOT_FOUND against PERMISSION_DENIED.
#define ATX_REQUIRE_DB(ctx, db_id)                                                             \
  if (!(ctx).entitlements.can_read(db_id)) {                                                   \
    return permission_denied(db_id);                                                           \
  }
```

Prefer a small helper function over a macro if it reads better in this codebase; the requirement is that every handler performs the check in that order.

Handler behaviour:

- **ListDatabases** — `catalog_.list_databases()`, filtered to `ctx.entitlements.readable_db_ids`. An unentitled caller gets an empty list and `OK`, not an error: this is a discovery call and an error would tell them the database exists.
- **ListSymbols / ListPartitions / ListSurfaces** — gate, then the matching `Catalog` accessor with `Page` offset/limit clamped to `[1, 5000]` (0 → 500). Stamp `ResponseMeta` with `registry_.generation(db_id)`.
- **GetCoverage** — gate, then `catalog_.coverage(...)`. Build `symbols` and `partition_keys` as the sorted row/column orders and emit one `CoverageCell` per non-absent cell.
- **GetSymbolConfig** — gate, then `registry_.get(db_id)` → `symbol_config(symbol)` → `encode_symbol_config`.
- **GetSurfaceMeta** — gate, `registry_.get`, `map_surface(partition_key, symbol)`, `encode_surface_meta`, stamp.
- **GetCurve** — gate, `map_surface`, `encode_curve(view, expiry_iso, z_window, curve_points, include_quotes)`, stamp.
- **GetSurfaceBlob** — register the name now with a body returning `make_status(v1::RPC_CODE_UNIMPLEMENTED, "GetSurfaceBlob lands in the next change")`. Registering the name here keeps the agreement test honest across both tasks.

Every non-OK path returns `status_from_error(error)` so the mapping table is the single place codes are chosen.

- [ ] **Step 5: Add the source to `atx-server/CMakeLists.txt`**

```cmake
    src/service_surface.cpp
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset server --target atx-server-tests`
Then: `ctest --preset server -R "^SurfaceService\." --output-on-failure`
Expected: 11 tests, all PASS.

- [ ] **Step 7: Commit**

```bash
git add atx-server/include/atx/server/service_surface.hpp \
        atx-server/src/service_surface.cpp \
        atx-server/tests/service_surface_test.cpp atx-server/CMakeLists.txt
git commit -m "feat(server): add the SurfaceService read path

Eight of nine methods; GetSurfaceBlob is registered but stubbed so the method
agreement test stays honest while its fidelity proof lands separately.

Every handler checks entitlement before existence. Reversing that order would
let an unentitled caller enumerate real db_ids by comparing NOT_FOUND against
PERMISSION_DENIED, so the sequence is fixed and tested on three different RPCs.

ListDatabases is the one exception and returns an empty list rather than an
error for an unentitled token. It is a discovery call, and an error there would
itself disclose that the database exists.

GetCoverage is the reason the catalog was built. The fixture has a symbol
configured in the manifest but absent from one partition, and the test asserts
that cell comes back CONFIGURED_NOT_FITTED rather than being silently omitted --
a silent omission is indistinguishable from a symbol nobody asked for.

GetCurve and GetSurfaceMeta are checked against locally computed results from
the same SurfaceDb rather than against hand-written expectations, so the test
proves the wire path preserves the values instead of restating them.

Paging limits are clamped server-side. Every response stamps db_generation from
the live SurfaceDb so a client revalidating on generation is correct rather than
hopeful.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 15: `GetSurfaceBlob` and the fidelity proof

The deliverable the whole spec exists for: a client pulls one symbol's serialized surface, decodes it locally, and gets **bit-identical** results to a local query.

**Files:**
- Modify: `atx-server/src/service_surface.cpp` (replace the stub)
- Modify: `atx-server/include/atx/server/encode.hpp`, `atx-server/src/encode.cpp` (add the blob builder)
- Create: `atx-server/tests/blob_fidelity_test.cpp`

**Interfaces:**
- Consumes: `atx::vol::SurfaceDb::load_surface`, `atx::vol::write_surface_archive_v2`, `atx::vol::SurfaceArchiveV2::open`/`map_symbol`.
- Produces:
  - `atx::core::Result<std::vector<std::byte>> atx::server::build_single_symbol_archive(atx::vol::SurfaceDb&, std::string_view partition_key, std::string_view symbol)`
  - `std::uint64_t atx::server::atxvsa_schema_hash() noexcept`
  - `std::uint64_t atx::server::content_hash(std::span<const std::byte>) noexcept`

- [ ] **Step 1: Write the failing test**

`atx-server/tests/blob_fidelity_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "atx/rpc/method_table.hpp"
#include "atx/server/catalog.hpp"
#include "atx/server/catalog_scan.hpp"
#include "atx/server/config.hpp"
#include "atx/server/encode.hpp"
#include "atx/server/methods.hpp"
#include "atx/server/service_surface.hpp"
#include "atx/server/surface_registry.hpp"
#include "atx/test/scratch.hpp"
#include "atx/vol/surface_archive.hpp"
#include "support/fixture_db.hpp"

namespace {

using atx::rpc::CallContext;
using atx::rpc::MethodTable;
using atx::server::atxvsa_schema_hash;
using atx::server::build_single_symbol_archive;
using atx::server::Catalog;
using atx::server::Realm;
using atx::server::RealmEntry;
using atx::server::ServerConfig;
using atx::server::SurfaceRegistry;
using atx::server::SurfaceServiceImpl;
using atx::server::test::make_fixture_db;
namespace v1 = atx::rpc::v1;
namespace methods = atx::server::methods;

struct Harness {
  std::unique_ptr<Catalog> catalog;
  Realm realm;
  std::unique_ptr<SurfaceRegistry> registry;
  ServerConfig config;
  std::unique_ptr<SurfaceServiceImpl> service;
  MethodTable table;
};

std::unique_ptr<Harness> make_harness(const std::string &name, std::size_t max_blob_bytes = 0) {
  auto harness = std::make_unique<Harness>();
  auto fixture = make_fixture_db(name);
  EXPECT_TRUE(fixture);

  RealmEntry entry;
  entry.db_id = "fixture";
  entry.kind = "surface_db";
  entry.root = fixture->root;
  EXPECT_TRUE(harness->realm.add(entry));

  const std::filesystem::path dir = atx::test::scratch_dir("atx-server-blob-" + name);
  auto catalog = Catalog::open((dir / "state.db").string());
  EXPECT_TRUE(catalog);
  harness->catalog = std::move(*catalog);
  EXPECT_TRUE(harness->catalog->upsert_realm_entry(entry));

  harness->registry = std::make_unique<SurfaceRegistry>(harness->realm, 4);
  auto db = harness->registry->get("fixture");
  EXPECT_TRUE(db);
  EXPECT_TRUE(harness->catalog->replace_db_index("fixture", *atx::server::scan_surface_db(**db)));

  if (max_blob_bytes != 0) {
    harness->config.max_blob_bytes = max_blob_bytes;
  }
  harness->service = std::make_unique<SurfaceServiceImpl>(
      *harness->catalog, *harness->registry, harness->config);
  harness->service->register_methods(harness->table);
  return harness;
}

CallContext entitled() {
  CallContext ctx;
  ctx.entitlements.authenticated = true;
  ctx.entitlements.readable_db_ids = {"fixture"};
  return ctx;
}

v1::GetSurfaceBlobRequest blob_request(std::uint64_t client_hash) {
  v1::GetSurfaceBlobRequest request;
  request.mutable_key()->mutable_partition()->set_db_id("fixture");
  request.mutable_key()->mutable_partition()->set_key("2024-03-14");
  request.mutable_key()->mutable_symbol()->set_symbol("SPY");
  request.set_client_atxvsa_schema_hash(client_hash);
  return request;
}

v1::RpcStatus fetch_blob(const MethodTable &table, const v1::GetSurfaceBlobRequest &request,
                         v1::SurfaceBlob &blob) {
  std::string out;
  const v1::RpcStatus status =
      table.invoke(methods::kSurfaceGetSurfaceBlob, entitled(), request.SerializeAsString(), out);
  if (status.code() == v1::RPC_CODE_OK) {
    EXPECT_TRUE(blob.ParseFromString(out));
  }
  return status;
}

// THE test. Bytes off the wire, decoded with the public archive API, must
// price identically to the same surface read directly from the database. Not
// "close" -- identical. Anything less means a client evaluating locally is
// silently disagreeing with the server.
TEST(BlobFidelity, DecodedBlobPricesBitIdenticallyToALocalRead) {
  auto harness = make_harness("fidelity");
  v1::SurfaceBlob blob;
  ASSERT_EQ(fetch_blob(harness->table, blob_request(atxvsa_schema_hash()), blob).code(),
            v1::RPC_CODE_OK);
  ASSERT_FALSE(blob.bytes().empty());

  std::vector<std::byte> bytes(blob.bytes().size());
  std::memcpy(bytes.data(), blob.bytes().data(), blob.bytes().size());
  auto archive = atx::vol::SurfaceArchiveV2::open(std::move(bytes));
  ASSERT_TRUE(archive) << archive.error().to_string();
  auto remote_view = archive->map_symbol("SPY");
  ASSERT_TRUE(remote_view) << remote_view.error().to_string();

  auto db = harness->registry->get("fixture");
  ASSERT_TRUE(db);
  auto local = (*db)->map_surface("2024-03-14", "SPY");
  ASSERT_TRUE(local);

  ASSERT_EQ(remote_view->expiry_count(), (*local)->expiry_count());
  ASSERT_GT(remote_view->expiry_count(), 0u);

  // Compare fair values across a grid that spans every expiry and a wide strike
  // range, so a discrepancy confined to the wings cannot hide.
  const double spot = (*local)->spot();
  std::size_t compared = 0;
  for (std::size_t e = 0; e < remote_view->expiry_count(); ++e) {
    for (int step = -8; step <= 8; ++step) {
      const double strike = spot * (1.0 + 0.05 * static_cast<double>(step));
      for (const bool is_call : {true, false}) {
        const double remote = remote_view->fair_value(e, strike, is_call);
        const double local_value = (*local)->fair_value(e, strike, is_call);
        // Bit-identical. memcmp on the doubles, not a tolerance: both sides ran
        // the same code over the same bytes, so any difference is a bug in the
        // round trip rather than floating-point noise.
        EXPECT_EQ(std::memcmp(&remote, &local_value, sizeof(double)), 0)
            << "expiry " << e << " strike " << strike << (is_call ? " call" : " put")
            << " remote=" << remote << " local=" << local_value;
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 100u) << "the comparison grid was too small to prove anything";
}

TEST(BlobFidelity, BlobContainsOnlyTheRequestedSymbol) {
  auto harness = make_harness("single-symbol");
  v1::SurfaceBlob blob;
  ASSERT_EQ(fetch_blob(harness->table, blob_request(atxvsa_schema_hash()), blob).code(),
            v1::RPC_CODE_OK);

  std::vector<std::byte> bytes(blob.bytes().size());
  std::memcpy(bytes.data(), blob.bytes().data(), blob.bytes().size());
  auto archive = atx::vol::SurfaceArchiveV2::open(std::move(bytes));
  ASSERT_TRUE(archive);
  auto all = archive->map_all();
  ASSERT_TRUE(all);
  // Never a whole partition. Shipping AAPL to a client that asked for SPY would
  // leak data across an entitlement boundary that is per-database today and may
  // become per-symbol later.
  EXPECT_EQ(all->size(), 1u);
  EXPECT_FALSE(archive->map_symbol("AAPL"));
}

TEST(BlobFidelity, ResponseCarriesTheSchemaHashAndGeneration) {
  auto harness = make_harness("hashes");
  v1::SurfaceBlob blob;
  ASSERT_EQ(fetch_blob(harness->table, blob_request(atxvsa_schema_hash()), blob).code(),
            v1::RPC_CODE_OK);
  EXPECT_EQ(blob.atxvsa_schema_hash(), atxvsa_schema_hash());
  EXPECT_EQ(blob.db_id(), "fixture");
  EXPECT_EQ(blob.partition_key(), "2024-03-14");
  EXPECT_EQ(blob.symbol(), "SPY");

  auto db = harness->registry->get("fixture");
  ASSERT_TRUE(db);
  EXPECT_EQ(blob.meta().db_generation(), (*db)->generation());
  EXPECT_NE(blob.meta().content_hash(), 0u);
}

// A client built against a different archive layout must be refused, not fed
// bytes it will mis-read into plausible-looking garbage.
TEST(BlobFidelity, MismatchedClientSchemaHashIsFailedPrecondition) {
  auto harness = make_harness("skew");
  v1::SurfaceBlob blob;
  const v1::RpcStatus status =
      fetch_blob(harness->table, blob_request(atxvsa_schema_hash() ^ 0xFFULL), blob);
  EXPECT_EQ(status.code(), v1::RPC_CODE_FAILED_PRECONDITION);
  // Both hashes must appear so the operator can tell which side is stale.
  EXPECT_NE(status.message().find(std::to_string(atxvsa_schema_hash())), std::string::npos);
  EXPECT_TRUE(blob.bytes().empty());
}

// A client that sends 0 has not been built against a known layout yet; treat it
// as "no assertion" rather than as a mismatch, so a first-contact client works.
TEST(BlobFidelity, ZeroClientSchemaHashSkipsTheCheck) {
  auto harness = make_harness("zero-hash");
  v1::SurfaceBlob blob;
  EXPECT_EQ(fetch_blob(harness->table, blob_request(0), blob).code(), v1::RPC_CODE_OK);
}

// Over the limit must report the real size, not truncate. A truncated archive
// would fail to open with an opaque parse error instead of an actionable one.
TEST(BlobFidelity, OversizedBlobIsResourceExhaustedAndNamesTheSize) {
  auto harness = make_harness("oversize", /*max_blob_bytes=*/64);
  v1::SurfaceBlob blob;
  const v1::RpcStatus status = fetch_blob(harness->table, blob_request(atxvsa_schema_hash()), blob);
  EXPECT_EQ(status.code(), v1::RPC_CODE_RESOURCE_EXHAUSTED);
  EXPECT_NE(status.message().find("64"), std::string::npos);
  EXPECT_TRUE(blob.bytes().empty()) << "a truncated archive is worse than no archive";
}

TEST(BlobFidelity, MissingSurfaceIsNotFound) {
  auto harness = make_harness("blob-missing");
  v1::GetSurfaceBlobRequest request = blob_request(atxvsa_schema_hash());
  request.mutable_key()->mutable_partition()->set_key("2024-03-15");
  request.mutable_key()->mutable_symbol()->set_symbol("AAPL");
  v1::SurfaceBlob blob;
  EXPECT_EQ(fetch_blob(harness->table, request, blob).code(), v1::RPC_CODE_NOT_FOUND);
}

TEST(BlobFidelity, RepeatedRequestsProduceIdenticalBytes) {
  auto harness = make_harness("stable");
  v1::SurfaceBlob first;
  v1::SurfaceBlob second;
  ASSERT_EQ(fetch_blob(harness->table, blob_request(atxvsa_schema_hash()), first).code(),
            v1::RPC_CODE_OK);
  ASSERT_EQ(fetch_blob(harness->table, blob_request(atxvsa_schema_hash()), second).code(),
            v1::RPC_CODE_OK);
  // Determinism is what makes content_hash usable for client-side caching.
  EXPECT_EQ(first.bytes(), second.bytes());
  EXPECT_EQ(first.meta().content_hash(), second.meta().content_hash());
}

} // namespace
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset server --target atx-server-tests`
Then: `ctest --preset server -R "^BlobFidelity\." --output-on-failure`
Expected: FAIL — either a compile error on `build_single_symbol_archive`, or `RPC_CODE_UNIMPLEMENTED` from the Task 14 stub.

- [ ] **Step 3: Add the blob builder to `encode.hpp`**

```cpp
// Appended to atx-server/include/atx/server/encode.hpp

#include <span>
#include <vector>

namespace atx::server {

// The ATXVSA layout this build reads and writes. A client compares its own
// value against this; a mismatch is refused rather than mis-read.
[[nodiscard]] std::uint64_t atxvsa_schema_hash() noexcept;

// Stable over identical bytes, so a client can cache on it.
[[nodiscard]] std::uint64_t content_hash(std::span<const std::byte> bytes) noexcept;

// Builds a SINGLE-SYMBOL ATXVSA v2 archive: load the owned surface, then
// re-encode just that one entry.
//
//   load_surface -> PricedSurface -> SurfaceArchiveItem
//                -> write_surface_archive_v2 -> bytes
//
// Never a whole partition. Shipping other symbols to a client that asked for
// one would leak data across an entitlement boundary that is per-database today
// and may become per-symbol later.
//
// This costs one decode plus one encode per request, which v1 accepts. Slicing
// the partition's directory entry zero-copy is a later optimisation, not a
// correctness requirement, and it would be much harder to prove bit-identical.
[[nodiscard]] atx::core::Result<std::vector<std::byte>>
build_single_symbol_archive(atx::vol::SurfaceDb &db, std::string_view partition_key,
                            std::string_view symbol);

} // namespace atx::server
```

- [ ] **Step 4: Implement the blob builder in `encode.cpp`**

```cpp
#include "atx/vol/surface_archive.hpp"

namespace atx::server {

std::uint64_t atxvsa_schema_hash() noexcept {
  // Sourced from atx-vol so it moves when the archive layout moves. If
  // surface_archive.hpp exposes a schema-hash constant, return it directly
  // rather than recomputing one here -- two definitions of the same hash is
  // exactly the drift this field exists to catch.
  return atx::vol::kSurfaceArchiveV2SchemaHash;
}

std::uint64_t content_hash(std::span<const std::byte> bytes) noexcept {
  // FNV-1a. Not cryptographic and not meant to be: this is a cache key and a
  // change detector, and the archive already carries its own CRC for integrity.
  std::uint64_t hash = 1469598103934665603ULL;
  for (const std::byte b : bytes) {
    hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(b));
    hash *= 1099511628211ULL;
  }
  return hash;
}

atx::core::Result<std::vector<std::byte>>
build_single_symbol_archive(atx::vol::SurfaceDb &db, std::string_view partition_key,
                            std::string_view symbol) {
  ATX_TRY(atx::vol::PricedSurface surface, db.load_surface(partition_key, symbol));

  atx::vol::SurfaceArchiveItem item;
  item.symbol = std::string{symbol};
  item.surface = std::move(surface);

  const std::array<atx::vol::SurfaceArchiveItem, 1> items{std::move(item)};
  atx::vol::ArchiveV2WriteOpts opts;
  return atx::vol::write_surface_archive_v2(items, opts);
}

} // namespace atx::server
```

Read `atx-vol/include/atx/vol/surface_archive.hpp` around lines 610-660 for the actual `SurfaceArchiveItem` field names and `ArchiveV2WriteOpts` members before writing this, and use what is there. If no `kSurfaceArchiveV2SchemaHash` constant exists, use the archive magic plus version fields the header does define, and note the substitution in the commit body.

- [ ] **Step 5: Replace the `GetSurfaceBlob` stub in `service_surface.cpp`**

```cpp
table.add<v1::GetSurfaceBlobRequest, v1::SurfaceBlob>(
    std::string{methods::kSurfaceGetSurfaceBlob},
    [this](const CallContext &ctx, const v1::GetSurfaceBlobRequest &request,
           v1::SurfaceBlob &response) -> v1::RpcStatus {
      const std::string &db_id = request.key().partition().db_id();
      if (!ctx.entitlements.can_read(db_id)) {
        return permission_denied(db_id);
      }

      // Schema check BEFORE the work. A client that cannot read the result
      // should not cost a decode and an encode to find that out. Zero means the
      // client is asserting nothing, which keeps first-contact clients working.
      const std::uint64_t server_hash = atxvsa_schema_hash();
      if (request.client_atxvsa_schema_hash() != 0 &&
          request.client_atxvsa_schema_hash() != server_hash) {
        return make_status(v1::RPC_CODE_FAILED_PRECONDITION,
                           "ATXVSA schema mismatch: server " + std::to_string(server_hash) +
                               ", client " +
                               std::to_string(request.client_atxvsa_schema_hash()) +
                               ". Rebuild the client against this server's atx-vol.");
      }

      auto db = registry_.get(db_id);
      if (!db) {
        return status_from_error(db.error());
      }
      auto bytes = build_single_symbol_archive(**db, request.key().partition().key(),
                                               request.key().symbol().symbol());
      if (!bytes) {
        return status_from_error(bytes.error());
      }

      // Report the real size and send nothing. A truncated archive fails to
      // open with an opaque parse error; a size the client can act on is worth
      // far more.
      if (bytes->size() > config_.max_blob_bytes) {
        return make_status(v1::RPC_CODE_RESOURCE_EXHAUSTED,
                           "surface archive is " + std::to_string(bytes->size()) +
                               " bytes, over the " + std::to_string(config_.max_blob_bytes) +
                               " byte limit");
      }

      response.set_db_id(db_id);
      response.set_partition_key(request.key().partition().key());
      response.set_symbol(request.key().symbol().symbol());
      response.set_atxvsa_schema_hash(server_hash);
      response.set_bytes(reinterpret_cast<const char *>(bytes->data()), bytes->size());
      stamp_meta(*response.mutable_meta(), (*db)->generation(), content_hash(*bytes));
      return ok_status();
    });
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset server --target atx-server-tests`
Then: `ctest --preset server -R "^BlobFidelity\." --output-on-failure`
Expected: 8 tests, all PASS.

- [ ] **Step 7: Run the whole suite**

Run: `ctest --preset server -L atx_server --output-on-failure`
Expected: all PASS.

- [ ] **Step 8: Commit**

```bash
git add atx-server/include/atx/server/encode.hpp atx-server/src/encode.cpp \
        atx-server/src/service_surface.cpp atx-server/tests/blob_fidelity_test.cpp
git commit -m "feat(server): serve single-symbol surface archives, proven bit-identical

GetSurfaceBlob loads one symbol's surface and re-encodes it as its own ATXVSA v2
archive. Never a whole partition: shipping other symbols to a client that asked
for one would leak data across an entitlement boundary that is per-database
today and may become per-symbol later. A test decodes the blob and asserts the
archive contains exactly one entry.

The fidelity test is the point of the whole spec. It decodes the wire bytes with
the public archive API and compares fair_value against the same surface read
directly from the database, over a grid spanning every expiry and sixteen strike
steps either side of spot, for calls and puts. The comparison is memcmp on the
doubles rather than a tolerance: both sides run the same code over the same
bytes, so any difference is a round-trip bug, not floating-point noise. The test
also asserts the grid was large enough to mean something.

The schema hash is checked before the work, not after, so a client that could
not read the result does not cost a decode and an encode to find out. A client
hash of zero asserts nothing and is allowed through, which keeps a first-contact
client working. On mismatch the message carries both values so an operator can
see which side is stale.

Over the limit returns the real byte size and no bytes. A truncated archive
fails to open with an opaque parse error; a size the client can act on is worth
far more than a partial payload.

Repeated requests produce identical bytes and an identical content hash, which
is what makes that field usable as a client cache key.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 16: the `atx-server` daemon

**Files:**
- Create: `atx-server/include/atx/server/server.hpp`, `atx-server/src/server.cpp`
- Create: `atx-server/tools/main.cpp`
- Create: `atx-server/tests/server_lifecycle_test.cpp`
- Modify: `atx-server/CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Tasks 9–15, plus `atx::rpc::RpcServer`, `Dispatcher`, `MethodTable`.
- Produces:
  - `class atx::server::Server` with `static Result<std::unique_ptr<Server>> start(ServerConfig)`, `std::uint16_t port() const noexcept`, `Catalog& catalog() noexcept`, `void wait()`, `void shutdown() noexcept`. Destructor calls `shutdown()`.
  - Executable target `atx-server` (`OUTPUT_NAME atx-server`).

- [ ] **Step 1: Write the failing test**

`atx-server/tests/server_lifecycle_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "atx/core/error.hpp"
#include "atx/rpc/client.hpp"
#include "atx/rpc/socket.hpp"
#include "atx/server/methods.hpp"
#include "atx/server/server.hpp"
#include "atx/test/scratch.hpp"
#include "support/fixture_db.hpp"

namespace {

using atx::core::ErrorCode;
using atx::rpc::RpcClient;
using atx::rpc::RpcClientConfig;
using atx::rpc::WsaScope;
using atx::server::Server;
using atx::server::ServerConfig;
using atx::server::test::make_fixture_db;
namespace v1 = atx::rpc::v1;
namespace methods = atx::server::methods;

ServerConfig config_for(const std::string &name) {
  const std::filesystem::path dir = atx::test::scratch_dir("atx-server-lifecycle-" + name);
  ServerConfig config;
  config.listen_host = "127.0.0.1";
  config.listen_port = 0; // ephemeral
  config.state_path = (dir / "state.db").string();
  config.worker_count = 2;
  return config;
}

RpcClientConfig client_for(std::uint16_t port, std::string token) {
  RpcClientConfig config;
  config.host = "127.0.0.1";
  config.port = port;
  config.auth_token = std::move(token);
  config.client_build = "lifecycle-test";
  config.call_timeout = std::chrono::milliseconds{5000};
  return config;
}

TEST(Server, StartsAndAnswersHealth) {
  const WsaScope wsa;
  auto server = Server::start(config_for("health"));
  ASSERT_TRUE(server) << server.error().to_string();
  ASSERT_TRUE((*server)->catalog().put_token("t", "test", {}));

  auto client = RpcClient::connect(client_for((*server)->port(), "t"));
  ASSERT_TRUE(client) << client.error().to_string();

  v1::HealthRequest request;
  auto response = client->call<v1::HealthRequest, v1::HealthResponse>(methods::kAdminHealth,
                                                                     request);
  ASSERT_TRUE(response) << response.error().to_string();
  EXPECT_TRUE(response->serving());
}

TEST(Server, RefusesToStartOnANonLoopbackAddress) {
  const WsaScope wsa;
  ServerConfig config = config_for("nonloopback");
  config.listen_host = "0.0.0.0";
  auto server = Server::start(config);
  ASSERT_FALSE(server);
  EXPECT_EQ(server.error().code(), ErrorCode::PermissionDenied);
}

TEST(Server, ServesSurfaceDataEndToEnd) {
  const WsaScope wsa;
  auto fixture = make_fixture_db("lifecycle-surface");
  ASSERT_TRUE(fixture);

  auto server = Server::start(config_for("surface"));
  ASSERT_TRUE(server);
  ASSERT_TRUE((*server)->catalog().put_token("t", "test", {"fixture"}));

  auto client = RpcClient::connect(client_for((*server)->port(), "t"));
  ASSERT_TRUE(client);

  v1::RegisterDatabaseRequest reg;
  reg.set_db_id("fixture");
  reg.set_kind("surface_db");
  reg.set_root(fixture->root);
  auto registered =
      client->call<v1::RegisterDatabaseRequest, v1::RegisterDatabaseResponse>(
          methods::kAdminRegisterDatabase, reg);
  ASSERT_TRUE(registered) << registered.error().to_string();

  v1::ListSurfacesRequest list;
  list.set_db_id("fixture");
  auto surfaces =
      client->call<v1::ListSurfacesRequest, v1::ListSurfacesResponse>(
          methods::kSurfaceListSurfaces, list);
  ASSERT_TRUE(surfaces) << surfaces.error().to_string();
  EXPECT_EQ(surfaces->surfaces_size(), 3);
}

TEST(Server, UnknownTokenIsRejected) {
  const WsaScope wsa;
  auto server = Server::start(config_for("badtoken"));
  ASSERT_TRUE(server);

  auto client = RpcClient::connect(client_for((*server)->port(), "never-issued"));
  ASSERT_TRUE(client);
  v1::HealthRequest request;
  auto response =
      client->call<v1::HealthRequest, v1::HealthResponse>(methods::kAdminHealth, request);
  ASSERT_FALSE(response);
  EXPECT_EQ(response.error().code(), ErrorCode::PermissionDenied);
}

// Warm start: the second process must not rescan a database whose generation
// has not moved, and must still answer correctly.
TEST(Server, RestartRestoresTheCatalogAndStillServes) {
  const WsaScope wsa;
  auto fixture = make_fixture_db("lifecycle-restart");
  ASSERT_TRUE(fixture);
  const ServerConfig config = config_for("restart");

  {
    auto server = Server::start(config);
    ASSERT_TRUE(server);
    ASSERT_TRUE((*server)->catalog().put_token("t", "test", {"fixture"}));
    auto client = RpcClient::connect(client_for((*server)->port(), "t"));
    ASSERT_TRUE(client);
    v1::RegisterDatabaseRequest reg;
    reg.set_db_id("fixture");
    reg.set_kind("surface_db");
    reg.set_root(fixture->root);
    ASSERT_TRUE((client->call<v1::RegisterDatabaseRequest, v1::RegisterDatabaseResponse>(
        methods::kAdminRegisterDatabase, reg)));
    (*server)->shutdown();
  }

  auto restarted = Server::start(config);
  ASSERT_TRUE(restarted) << restarted.error().to_string();
  auto client = RpcClient::connect(client_for((*restarted)->port(), "t"));
  ASSERT_TRUE(client) << "the token must survive the restart";

  v1::ListSurfacesRequest list;
  list.set_db_id("fixture");
  auto surfaces = client->call<v1::ListSurfacesRequest, v1::ListSurfacesResponse>(
      methods::kSurfaceListSurfaces, list);
  ASSERT_TRUE(surfaces) << surfaces.error().to_string();
  EXPECT_EQ(surfaces->surfaces_size(), 3)
      << "the realm and its index did not survive the restart";
}

TEST(Server, ShutdownIsIdempotent) {
  const WsaScope wsa;
  auto server = Server::start(config_for("shutdown"));
  ASSERT_TRUE(server);
  (*server)->shutdown();
  (*server)->shutdown();
  SUCCEED();
}

} // namespace
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset server --target atx-server-tests`
Expected: FAIL at compile — `atx/server/server.hpp: No such file or directory`.

- [ ] **Step 3: Write `atx-server/include/atx/server/server.hpp`**

```cpp
#pragma once

// Server — assembles the whole daemon and owns every thread it starts.
//
// Startup order is not arbitrary:
//   1. Catalog (state tier is authoritative; nothing else can run without it)
//   2. realm import, if requested        -- before anything is served
//   3. Realm loaded from the catalog
//   4. SurfaceRegistry over that realm
//   5. warm-start restore, then rescan only the databases whose generation moved
//   6. services registered into one MethodTable
//   7. Dispatcher wired to the catalog's token resolver
//   8. RpcServer bound (refuses non-loopback a second time)
//   9. CatalogRefresher started LAST, so it never races the initial index

#include <cstdint>
#include <memory>

#include "atx/core/error.hpp"
#include "atx/server/catalog.hpp"
#include "atx/server/config.hpp"

namespace atx::server {

class Server {
public:
  [[nodiscard]] static atx::core::Result<std::unique_ptr<Server>> start(ServerConfig config);

  ~Server();
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  Server(Server &&) = delete;
  Server &operator=(Server &&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept;
  // Exposed so tools and tests can mint tokens without a second connection to
  // the state database.
  [[nodiscard]] Catalog &catalog() noexcept;

  // Blocks until shutdown() is called.
  void wait();
  // Idempotent. Snapshots the derived catalog on the way out so the next start
  // is warm.
  void shutdown() noexcept;

private:
  class Impl;
  explicit Server(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::server
```

- [ ] **Step 4: Write `atx-server/src/server.cpp`**

Implement `Impl` holding, in declaration order so destruction unwinds correctly: `ServerConfig`, `std::unique_ptr<Catalog>`, `Realm`, `std::unique_ptr<SurfaceRegistry>`, the two service objects, `std::unique_ptr<atx::rpc::RpcServer>`, `std::unique_ptr<CatalogRefresher>`, plus a `std::mutex`/`std::condition_variable`/`bool` for `wait()`.

`Server::start` performs the nine steps from the header comment:

```cpp
atx::core::Result<std::unique_ptr<Server>> Server::start(ServerConfig config) {
  // Checked here even though parse_args already did: a programmatic caller does
  // not go through argv, and this is the mistake that turns a local tool into
  // an unauthenticated public data service.
  if (!atx::rpc::is_loopback(config.listen_host)) {
    return atx::core::Err(atx::core::ErrorCode::PermissionDenied,
                          "refusing to listen on non-loopback address '" +
                              config.listen_host +
                              "': this build has no transport encryption");
  }
  ATX_TRY(auto catalog, Catalog::open(config.state_path));
  // ... realm import, realm load, registry, warm start, services, dispatcher,
  //     RpcServer, refresher
}
```

Specifics:

- **Realm import** — when `config.realm_import_path` is non-empty, read the file, `Realm::parse_json`, and `catalog->upsert_realm_entry` for each entry. A parse failure aborts startup: serving with a partially applied realm is worse than not starting.
- **Warm start** — the snapshot path is `config.state_path + ".catalog-snapshot"`. If it exists, `catalog->restore_from(path)`. Then for each realm entry compare `registry->generation(db_id)` against `catalog->generation_of(db_id)` and call `scan_surface_db` + `replace_db_index` only on drift or absence.
- **Dispatcher authenticator** — `[&catalog](std::string_view token) { return catalog->resolve_token(token); }`.
- **RpcServer config** — copy limits from `ServerConfig`, set `server_build = version_string()` and `atxvsa_schema_hash = atxvsa_schema_hash()`.
- **`shutdown()`** — stop the refresher first (so it cannot re-index into a closing catalog), then the `RpcServer`, then `catalog->snapshot_to(snapshot_path)`, then signal `wait()`. Guard with an `std::atomic<bool>` so it is idempotent.

- [ ] **Step 5: Write `atx-server/tools/main.cpp`**

```cpp
#include <csignal>
#include <cstdlib>
#include <iostream>

#include "atx/rpc/socket.hpp"
#include "atx/server/config.hpp"
#include "atx/server/server.hpp"
#include "atx/server/version.hpp"

namespace {

// Written by the signal handler, read by main. Only sig_atomic_t is safe here.
volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

} // namespace

int main(int argc, char **argv) {
  auto config = atx::server::parse_args(argc, argv);
  if (!config) {
    std::cerr << "atx-server: " << config.error().to_string() << "\n\n"
              << atx::server::usage_text();
    return EXIT_FAILURE;
  }
  if (config->print_help) {
    std::cout << atx::server::usage_text();
    return EXIT_SUCCESS;
  }
  if (config->print_version) {
    std::cout << atx::server::version_string() << '\n';
    return EXIT_SUCCESS;
  }

  const atx::rpc::WsaScope wsa;
  auto server = atx::server::Server::start(*config);
  if (!server) {
    std::cerr << "atx-server: " << server.error().to_string() << '\n';
    return EXIT_FAILURE;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::cout << atx::server::version_string() << " listening on " << config->listen_host << ':'
            << (*server)->port() << '\n';

  // Poll the flag rather than calling shutdown() from the handler: shutdown()
  // joins threads and takes locks, neither of which is async-signal-safe.
  while (g_stop == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  std::cout << "atx-server: shutting down\n";
  (*server)->shutdown();
  return EXIT_SUCCESS;
}
```

Add `#include <chrono>` and `#include <thread>`.

- [ ] **Step 6: Add the executable to `atx-server/CMakeLists.txt`**

```cmake
    src/server.cpp

# ... after the library block:
add_executable(atx-server-daemon tools/main.cpp)
set_target_properties(atx-server-daemon PROPERTIES OUTPUT_NAME atx-server)
target_link_libraries(atx-server-daemon PRIVATE atx::server atx_warnings)
target_compile_features(atx-server-daemon PRIVATE cxx_std_20)
```

The target is named `atx-server-daemon` because `atx-server-lib` already uses the natural name and CMake target names must be unique; `OUTPUT_NAME` gives the binary the expected name.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset server`
Then: `ctest --preset server -R "^Server\." --output-on-failure`
Expected: 6 tests, all PASS.

- [ ] **Step 8: Verify the binary refuses a non-loopback bind**

Run: `./build/dev/bin/atx-server --listen 0.0.0.0:50051; echo "exit=$?"`
Expected: a diagnostic naming the missing transport encryption, and `exit=1`.

Run: `./build/dev/bin/atx-server --help; echo "exit=$?"`
Expected: usage text mentioning the loopback restriction, and `exit=0`.

- [ ] **Step 9: Commit**

```bash
git add atx-server/include/atx/server/server.hpp atx-server/src/server.cpp \
        atx-server/tools/main.cpp atx-server/tests/server_lifecycle_test.cpp \
        atx-server/CMakeLists.txt
git commit -m "feat(server): assemble the daemon

Startup order is load-bearing and the header says so. The realm import runs
before anything is served, because a partially applied realm is worse than a
server that did not start. The refresher starts last so it cannot race the
initial index. Shutdown reverses it: stop the refresher first so it cannot
re-index into a closing catalog, then the socket, then snapshot the derived
catalog so the next start is warm.

The loopback check runs again here even though parse_args already made it. A
programmatic caller never goes through argv, and this is the single mistake
that turns a local tool into an unauthenticated public data service.

The signal handler sets a flag and main polls it. Calling shutdown() from the
handler would join threads and take locks inside a signal context, neither of
which is async-signal-safe.

The restart test registers a database, stops the process, starts it again, and
asserts both the token and the surface index survived -- warm start is only
worth having if the authoritative tier really is authoritative.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 17: `atx-server-cli`, method agreement, and the final gate

**Files:**
- Create: `atx-server/tools/cli.cpp`
- Create: `atx-server/tests/method_agreement_test.cpp`
- Create: `atx-server/README.md`
- Modify: `atx-server/CMakeLists.txt`
- Modify: `README.md` (root)

**Interfaces:**
- Consumes: `RpcClient`, `methods::kAll`, every service.
- Produces: executable `atx-server-cli`; ctest cases `AtxServerCli.*`.

- [ ] **Step 1: Write the failing agreement test**

`atx-server/tests/method_agreement_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "atx/rpc/method_table.hpp"
#include "atx/server/catalog.hpp"
#include "atx/server/config.hpp"
#include "atx/server/methods.hpp"
#include "atx/server/service_admin.hpp"
#include "atx/server/service_surface.hpp"
#include "atx/server/surface_registry.hpp"
#include "atx/test/scratch.hpp"

namespace {

using atx::rpc::MethodTable;
using atx::server::AdminServiceImpl;
using atx::server::Catalog;
using atx::server::Realm;
using atx::server::ServerConfig;
using atx::server::SurfaceRegistry;
using atx::server::SurfaceServiceImpl;
namespace methods = atx::server::methods;

// A method name declared in methods.hpp but never registered is a silent
// UNIMPLEMENTED in production; a method registered but not declared is
// unreachable from the CLI and any generated client. Both directions matter.
TEST(MethodAgreement, DeclaredNamesAndRegisteredNamesMatchExactly) {
  const std::filesystem::path dir = atx::test::scratch_dir("atx-server-agreement");
  auto catalog = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(catalog);
  Realm realm;
  SurfaceRegistry registry{realm, 4};
  const ServerConfig config;

  MethodTable table;
  AdminServiceImpl admin{**catalog, realm, registry,
                         [] { return atx::rpc::RpcServerStats{}; }};
  SurfaceServiceImpl surface{**catalog, registry, config};
  admin.register_methods(table);
  surface.register_methods(table);

  std::set<std::string> declared;
  for (const std::string_view name : methods::kAll) {
    declared.emplace(name);
  }
  const std::vector<std::string> registered_vec = table.method_names();
  const std::set<std::string> registered{registered_vec.begin(), registered_vec.end()};

  std::vector<std::string> declared_not_registered;
  std::set_difference(declared.begin(), declared.end(), registered.begin(), registered.end(),
                      std::back_inserter(declared_not_registered));
  std::vector<std::string> registered_not_declared;
  std::set_difference(registered.begin(), registered.end(), declared.begin(), declared.end(),
                      std::back_inserter(registered_not_declared));

  EXPECT_TRUE(declared_not_registered.empty())
      << "declared but never registered: " << declared_not_registered.size() << " method(s), "
      << "first: " << (declared_not_registered.empty() ? "" : declared_not_registered.front());
  EXPECT_TRUE(registered_not_declared.empty())
      << "registered but not declared: " << registered_not_declared.size() << " method(s), "
      << "first: " << (registered_not_declared.empty() ? "" : registered_not_declared.front());
  EXPECT_EQ(declared.size(), methods::kAll.size()) << "kAll contains a duplicate";
}

// Every method name must be resolvable to a service prefix that exists.
TEST(MethodAgreement, EveryNameBelongsToAKnownService) {
  for (const std::string_view name : methods::kAll) {
    const bool admin = name.rfind("atx.rpc.v1.AdminService/", 0) == 0;
    const bool surface = name.rfind("atx.rpc.v1.SurfaceService/", 0) == 0;
    EXPECT_TRUE(admin || surface) << name;
  }
}

} // namespace
```

- [ ] **Step 2: Run the test to verify it fails or passes**

Run: `cmake --build --preset server --target atx-server-tests`
Then: `ctest --preset server -R "^MethodAgreement\." --output-on-failure`
Expected: PASS if Tasks 13–15 registered every name. If it FAILS, the failure message names the offending method — fix the registration or the declaration before continuing. Do not weaken the test.

- [ ] **Step 3: Write `atx-server/tools/cli.cpp`**

A subcommand client. Global flags `--server HOST:PORT` (default `127.0.0.1:50051`) and `--token TOKEN`. Subcommands:

| Subcommand | Call | Output |
|---|---|---|
| `health` | `kAdminHealth` | `serving=true uptime_ns=...` |
| `info` | `kAdminGetServerInfo` | version, realm, uuid, schema hashes |
| `stats` | `kAdminGetStats` | counters, one line per database |
| `register --db ID --root PATH` | `kAdminRegisterDatabase` | `registered ID generation=N` |
| `realm` | `kAdminExportRealm` | one line per entry |
| `databases` | `kSurfaceListDatabases` | one line per database |
| `symbols --db ID [--prefix P]` | `kSurfaceListSymbols` | one line per symbol |
| `partitions --db ID` | `kSurfaceListPartitions` | one line per partition |
| `surfaces --db ID [--symbol S]` | `kSurfaceListSurfaces` | one line per surface |
| `coverage --db ID` | `kSurfaceGetCoverage` | the matrix, `●`/`○`/`·` per cell |
| `meta --db ID --key K --symbol S` | `kSurfaceGetSurfaceMeta` | spot, expiry table |
| `curve --db ID --key K --symbol S --expiry E` | `kSurfaceGetCurve` | `z,strike,model_iv` per line |
| `blob --db ID --key K --symbol S --out FILE` | `kSurfaceGetSurfaceBlob` | writes the archive, prints size and hashes |

Rules the implementation must follow:

- Unknown subcommand or unknown flag → usage on stderr, exit 1.
- Any RPC error → `atx-server-cli: <error>` on stderr, exit 1. The exit code is what the ctest cases assert.
- `blob --out` writes with `std::ios::binary`. A text-mode write on Windows would corrupt the archive by translating `0x0A`, and the corruption would only surface later as a parse error.
- `curve` prints CSV with a header line, so its output can be diffed against a locally computed curve.

- [ ] **Step 4: Add the CLI target and its ctest cases**

In `atx-server/CMakeLists.txt`:

```cmake
add_executable(atx-server-cli tools/cli.cpp)
target_link_libraries(atx-server-cli PRIVATE atx::server atx::rpc atx_warnings)
target_compile_features(atx-server-cli PRIVATE cxx_std_20)
```

In `atx-server/tests/CMakeLists.txt`:

```cmake
# Argument-handling gates. These need no running server: they assert the CLI
# fails loudly rather than silently doing the wrong thing.
add_test(NAME AtxServerCli.RejectsUnknownSubcommand
    COMMAND $<TARGET_FILE:atx-server-cli> not-a-subcommand)
set_tests_properties(AtxServerCli.RejectsUnknownSubcommand
    PROPERTIES WILL_FAIL TRUE LABELS atx_server)

add_test(NAME AtxServerCli.RejectsMissingDbArgument
    COMMAND $<TARGET_FILE:atx-server-cli> symbols)
set_tests_properties(AtxServerCli.RejectsMissingDbArgument
    PROPERTIES WILL_FAIL TRUE LABELS atx_server)

add_test(NAME AtxServerCli.FailsWhenNoServerIsListening
    COMMAND $<TARGET_FILE:atx-server-cli> --server 127.0.0.1:1 --token t health)
set_tests_properties(AtxServerCli.FailsWhenNoServerIsListening
    PROPERTIES WILL_FAIL TRUE LABELS atx_server)

add_test(NAME AtxServerCli.HelpSucceeds
    COMMAND $<TARGET_FILE:atx-server-cli> --help)
set_tests_properties(AtxServerCli.HelpSucceeds PROPERTIES LABELS atx_server)
```

- [ ] **Step 5: Run the CLI smoke sequence by hand against a live server**

```bash
# Terminal 1
./build/dev/bin/atx-server --state ./tmp/smoke-state.db

# Terminal 2 -- mint a token first via the CLI's register path is not possible,
# so use a scratch state database seeded by the lifecycle test, or add a
# `--bootstrap-token` flag to the daemon if this proves awkward in practice.
./build/dev/bin/atx-server-cli --token t register --db spy --root <a populated SurfaceDb>
./build/dev/bin/atx-server-cli --token t databases
./build/dev/bin/atx-server-cli --token t surfaces --db spy --symbol SPY
./build/dev/bin/atx-server-cli --token t coverage --db spy
./build/dev/bin/atx-server-cli --token t curve --db spy --key <date> --symbol SPY --expiry <iso>
./build/dev/bin/atx-server-cli --token t blob --db spy --key <date> --symbol SPY --out spy.atxvsa
```

Expected: each command prints its table and exits 0; `spy.atxvsa` is non-empty and `SurfaceArchiveV2::open` accepts it (the blob-fidelity test already proves this programmatically).

If minting the first token turns out to require a daemon flag, add `--bootstrap-token TOKEN` to `ServerConfig`/`parse_args` and have `Server::start` call `catalog->put_token` with read entitlement on every realm database. Document it in `usage_text()` as loopback-only, and add a config test asserting it is rejected alongside a non-loopback `--listen`.

- [ ] **Step 6: Write `atx-server/README.md`**

Cover: what the daemon is, the loopback-only restriction and why, how to start it, the realm import format with a worked JSON example, every CLI subcommand, the wire format (framing + envelope + handshake), the error-code table, and a "deliberately deferred" list pointing at the spec. Link the spec at `docs/superpowers/specs/2026-08-02-atx-server-rpc-foundation-design.md`.

- [ ] **Step 7: Add `atx-server` to the root `README.md` project table**

Follow the existing format for `atx-vol` and `atx-ui`. One line: name, one-sentence purpose, and the `ATX_BUILD_SERVER=ON` gate.

- [ ] **Step 8: Run the entire gate**

Run: `cmake --build --preset server`
Expected: clean build, no warnings on hand-written targets.

Run: `ctest --preset server -L atx_server --output-on-failure`
Expected: every test PASS.

Run: `ctest --preset server -L atx_server --repeat until-fail:3 --output-on-failure`
Expected: PASS on all three repetitions. Socket tests that pass once are not passing.

Run: `cmake --preset dev && cmake --build --preset dev`
Then: `ctest --preset dev -L atx_vol_fast --output-on-failure`
Expected: unchanged from before this branch — the server work must not regress `atx-vol`.

- [ ] **Step 9: Verify every acceptance criterion in the spec**

Walk section 13 of `docs/superpowers/specs/2026-08-02-atx-server-rpc-foundation-design.md` and confirm each of the ten items. Record the actual command and its observed output for each. Any criterion that cannot be demonstrated is not done; fix it rather than reinterpreting the criterion.

- [ ] **Step 10: Commit**

```bash
git add atx-server/tools/cli.cpp atx-server/tests/method_agreement_test.cpp \
        atx-server/README.md atx-server/CMakeLists.txt atx-server/tests/CMakeLists.txt \
        README.md
git commit -m "feat(server): add atx-server-cli, the method agreement gate, and docs

The CLI is both an operator tool and the end-to-end proof: connect, handshake,
register, list, coverage, curve, blob, each as a separate process against a live
server.

The agreement test compares declared method names against registered ones in
both directions. A name declared and never registered is a silent UNIMPLEMENTED
in production; a name registered and never declared is unreachable from the CLI
and from any generated client. Neither direction is caught by any other test,
and the failure message names the offending method rather than just the count.

blob --out writes in binary mode. A text-mode write on Windows would translate
0x0A inside the archive and the corruption would surface much later as an opaque
parse error, far from its cause.

The CLI's argument-handling gates run as ctest cases with WILL_FAIL, so a
regression that makes the tool exit zero on a bad invocation is caught. Exiting
zero on failure is worse than crashing: a script would carry on.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Plan Self-Review

Run after the plan is written and before execution begins. Findings from the pass that produced this section are recorded inline.

**1. Spec coverage.** Every section of the design maps to a task:

| Spec section | Task |
|---|---|
| §2.1 transport decision, grpc removal | 1 |
| §2.2 concurrency | 6 |
| §2.3 loopback gate | 6, 9, 16 |
| §2.4 daemon lifecycle | 16 |
| §4.2 atx-rpc layout | 3–7 |
| §4.3 framing and envelope | 2, 3 |
| §4.4 service registration | 5 |
| §4.5 atx-proto | 2, 8 |
| §4.6 atx-server layout | 9–16 |
| §4.7 realm | 9 |
| §5 catalog | 10, 11 |
| §6.1 threading | 6 |
| §6.2 catalog locking | 10 |
| §7 service surface | 13, 14, 15 |
| §8 error model | 13 |
| §9 security posture | 6, 9, 10, 16 |
| §10 build wiring | 1 |
| §11 deferred | not implemented, by design |
| §12 tests 1–15 | 3, 6, 12, 10, 11, 15, 17, 5, 6, 12, 16, 14, 16, 9, 17 |
| §13 acceptance 1–10 | 17 step 9 |

No gaps.

**2. Placeholder scan.** Three items in this plan describe behaviour without inlining full code, and each is deliberate because the correct code depends on an API the plan must not guess at:

- Task 10 step 6 — the `Catalog` accessor bodies. The schema and the two load-bearing methods are given in full; the remaining accessors are specified query-by-query because they are mechanical prepared-statement loops.
- Task 12 step 7 — `encode.cpp`. The plan requires reading `priced_surface_view.hpp` and using its real accessors rather than inventing names.
- Task 15 steps 3–4 — the archive schema-hash constant. The plan requires reading `surface_archive.hpp` and states what to do if the expected constant is absent.

These are instructions to read a specific header, not "TBD". No step says "add error handling" or "write tests for the above".

**3. Type consistency.** Checked across tasks:

- `atx::rpc::Entitlements` is defined in Task 5 (`call_context.hpp`) and consumed by Task 10 (`Catalog::resolve_token`) and Task 14 — one definition, one spelling.
- `FrameLimits` is defined in Task 1 and used in Tasks 3, 4, 6, 7 with the same member name `max_frame_bytes`.
- `v1::RpcStatus` / `v1::RpcCode` are named consistently in every task, and the rename from the spec's `Status`/`Code` is recorded in Global Constraints.
- `DbIndex` is produced by `scan_surface_db` (Task 11) and consumed by `Catalog::replace_db_index` (Task 10) with matching field names.
- `methods::*` constants are defined in Task 13 and referenced by Tasks 13, 14, 15, 16, 17 by the same names.
- `atxvsa_schema_hash()` is declared in Task 15 and used by Tasks 15 and 16.
- `SurfaceRegistry` is forward-declared in Task 11's `catalog_refresh.hpp` and defined in Task 12 — the ordering is called out in Task 11's interface block.

One fix applied during review: `Realm` gained `set_realm_id`/`realm_id`, added in Task 13 step 6 because `AdminService.ExportRealm` needs a realm id that Task 9 did not define.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-08-02-atx-server-rpc-foundation.md`.
