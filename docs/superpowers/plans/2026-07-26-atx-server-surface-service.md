# atx-server Surface Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `atx-proto` (gRPC wire contract) and `atx-server` (the central multi-client data server over the atx-vol surface database), per `docs/superpowers/specs/2026-07-26-atx-server-surface-service-design.md`.

**Architecture:** One synchronous gRPC server binary. An embedded SQLite catalog (`:memory:` derived coverage index `ATTACH`ed to an on-disk WAL state db holding realm/tokens/entitlements) is the SpiderRock-SRSE analogue. `SurfaceService` serves proto view models plus opt-in raw ATXVSA record blobs; `AdminService` serves health/stats/realm admin. Pure encoder functions translate atx-vol domain types to proto with no gRPC dependency.

**Tech Stack:** C++20, clang-cl, CMake+Ninja, vcpkg (grpc, protobuf, openssl), vendored SQLite via `atx::core::db`, `atx::vol` SurfaceDb/SurfaceArchiveV2/PricedSurface, GoogleTest.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-26-atx-server-surface-service-design.md`. Read it before starting any task.
- vcpkg deps added: `grpc`, `protobuf`. Pinned by existing `builtin-baseline` `9e9398f90a6c386bbd6ed89714ddb036b2e969eb`. The chocolatey `protoc` on PATH must NOT be used — protoc and `grpc_cpp_plugin` come from the vcpkg toolchain via CMake imported targets.
- Everything gated behind `option(ATX_BUILD_SERVER ... OFF)`. A `dev`-preset build must be byte-for-byte unaffected.
- New configure preset `server` (inherits `dev`, `binaryDir` `build-server`, `ATX_BUILD_SERVER=ON`). All server work builds under `cmake --preset server`.
- **Do NOT use `atx::core::Sha256`** (`atx-core/include/atx/core/sha256.hpp` is untracked in-flight work from another session — user directive: skip it). Token digests use OpenSSL (`EVP_Digest` with `EVP_sha256()`, `CRYPTO_memcmp`), linked as `OpenSSL::Crypto` (already a vcpkg manifest dep).
- Error handling: `atx::core::Result<T>` / `Status` / `Ok` / `Err` / `ATX_TRY` (`atx-core/include/atx/core/error.hpp`). No exception escapes a gRPC handler.
- First-party server code links `atx_warnings` (/W4 /WX). The `atx-proto` target does NOT link `atx_warnings`; its generated headers are exposed as SYSTEM includes.
- No filesystem path crosses the wire except `AdminService.ExportRealm` (the sanctioned admin exception, spec §4.4).
- Tests: GoogleTest, discovered with `gtest_discover_tests(... PROPERTIES LABELS atx_server DISCOVERY_MODE PRE_TEST)`. One bare label only (see `atx-vol/tests/CMakeLists.txt:207-221` for why multi-label lists break).
- Auth metadata key: `x-atx-token` (lowercase — gRPC requires lowercase metadata keys).
- SQLite discipline: single `Catalog` connection guarded by `std::mutex`; every expensive operation (archive decode, curve eval, file IO) happens OUTSIDE the catalog lock. Spec §6.
- Commit after every task (commit messages in normal prose, Conventional Commits style, `feat(server):` / `test(server):` / `build:` scopes).
- Repo working tree is shared with other sessions: `git add` ONLY the files your task touches, never `git add -A`.

## File Structure

```
vcpkg.json                          MODIFY  + grpc, protobuf
CMakeLists.txt                      MODIFY  + ATX_BUILD_SERVER option + subdirs
CMakePresets.json                   MODIFY  + server configure/build presets
atx-core/include/atx/core/db/sqlite.hpp  MODIFY  threading-note amendment (Task 4)

atx-proto/
  CMakeLists.txt                    atx-proto static lib, protobuf_generate x2
  atx/rpc/v1/keys.proto             SymbolKey, ExpiryKey, OptionKey, PartitionKey, SurfaceKey
  atx/rpc/v1/common.proto           ResponseMeta, Page, PageInfo
  atx/rpc/v1/surface.proto          SymbolFitConfig, Provenance, ExpirySummary, SurfaceMeta,
                                    CurvePoint, VolCurveSlice, SurfaceBlob
  atx/rpc/v1/surface_service.proto  SurfaceService + request/response messages
  atx/rpc/v1/admin.proto            AdminService + RealmEntry/RealmConfig + messages

atx-server/
  CMakeLists.txt                    atx-server-lib, atx-server, atx-server-cli, tests/
  include/atx/server/config.hpp     ServerConfig + validate_listen
  include/atx/server/catalog.hpp    Catalog (SQLite, two-tier)
  include/atx/server/catalog_index.hpp  index_surface_db, bootstrap_catalog
  include/atx/server/auth.hpp       token_digest, constant_time_equal, Entitlements, authenticate
  include/atx/server/surface_registry.hpp  SurfaceRegistry
  include/atx/server/encode.hpp     pure domain->proto encoders
  include/atx/server/service_error.hpp  to_grpc_status, run_handler
  include/atx/server/service_admin.hpp  AdminServiceImpl
  include/atx/server/service_surface.hpp  SurfaceServiceImpl
  include/atx/server/server.hpp     Server build/start/shutdown
  include/atx/server/refresher.hpp  CatalogRefresher
  src/<one .cpp per header>
  tools/main.cpp                    atx-server binary
  tools/cli.cpp                     atx-server-cli binary
  tools/mkfixture.cpp               fixture-db generator (smoke test + demos)
  tests/CMakeLists.txt
  tests/test_support.hpp/.cpp       make_essvi PricedSurface + make_fixture_db
  tests/config_test.cpp
  tests/catalog_test.cpp
  tests/auth_test.cpp
  tests/registry_test.cpp
  tests/index_test.cpp
  tests/encode_test.cpp
  tests/service_error_test.cpp
  tests/service_test.cpp            in-process channel: admin + surface RPCs
  tests/blob_fidelity_test.cpp
  tests/lifecycle_test.cpp          refresher, warm start, concurrency
  tests/smoke.ps1                   process-level smoke (server + cli)
```

---

### Task 1: Build wiring (vcpkg, option, presets)

**Files:**
- Modify: `vcpkg.json`
- Modify: `CMakeLists.txt` (root)
- Modify: `CMakePresets.json`

**Interfaces:**
- Produces: CMake option `ATX_BUILD_SERVER` (OFF default); configure preset `server` + build preset `server`; `find_package(gRPC CONFIG)` resolvable under the `server` preset.

- [ ] **Step 1: Add grpc + protobuf to vcpkg.json**

`vcpkg.json` dependencies array becomes:

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
    "grpc",
    "protobuf"
  ],
  "builtin-baseline": "9e9398f90a6c386bbd6ed89714ddb036b2e969eb"
}
```

- [ ] **Step 2: Add the ATX_BUILD_SERVER option + subdirectories to root CMakeLists.txt**

Immediately before the final `if(ATX_BUILD_UI)` block in `CMakeLists.txt`, add:

```cmake
# atx-server: gRPC data server over the surface database. OFF by default so a
# normal atx-vol dev never builds grpc/protobuf (vcpkg cold build is ~30 min
# once, cached after). The `server` preset flips it on.
option(ATX_BUILD_SERVER "Build the ATX gRPC server (atx-proto + atx-server)" OFF)
if(ATX_BUILD_SERVER)
    add_subdirectory(atx-proto)
    add_subdirectory(atx-server)
endif()
```

- [ ] **Step 3: Add the server presets to CMakePresets.json**

In `configurePresets`, after the `"rel-avx2"` entry, add:

```json
{
  "name": "server",
  "inherits": "dev",
  "displayName": "server: dev + ATX_BUILD_SERVER (atx-proto + atx-server)",
  "description": "Everything in dev plus the gRPC server subprojects. Separate build dir so flipping the option never dirties the dev build cache. First configure triggers the one-time vcpkg grpc+protobuf build.",
  "binaryDir": "${sourceDir}/build-server",
  "cacheVariables": {
    "ATX_BUILD_SERVER": "ON"
  }
}
```

In `buildPresets`, add:

```json
{ "name": "server", "configurePreset": "server" }
```

- [ ] **Step 4: Verify the dev preset is unaffected**

Run: `cmake --preset dev` (from repo root)
Expected: configures exactly as before; output contains NO mention of gRPC, protobuf, atx-proto, or atx-server.

- [ ] **Step 5: Configure the server preset (triggers the one-time vcpkg build)**

Run: `cmake --preset server`
Expected: vcpkg builds/restores `grpc` and `protobuf` (first run may take 20-40 min; binary-cached afterwards), then configure FAILS at `add_subdirectory(atx-proto)` with "add_subdirectory given source ... which is not an existing directory" — that error is the success signal for THIS task (the option + toolchain resolve; the directories arrive in Tasks 2-3).

Note: if vcpkg fails on `grpc` itself, stop and report — do not work around with FetchContent or a system install.

- [ ] **Step 6: Commit**

```bash
git add vcpkg.json CMakeLists.txt CMakePresets.json
git commit -m "build: wire ATX_BUILD_SERVER option, server preset, grpc+protobuf manifest deps"
```

---

### Task 2: atx-proto contract package

**Files:**
- Create: `atx-proto/CMakeLists.txt`
- Create: `atx-proto/atx/rpc/v1/keys.proto`
- Create: `atx-proto/atx/rpc/v1/common.proto`
- Create: `atx-proto/atx/rpc/v1/surface.proto`
- Create: `atx-proto/atx/rpc/v1/surface_service.proto`
- Create: `atx-proto/atx/rpc/v1/admin.proto`

**Interfaces:**
- Produces: CMake target `atx-proto` (static). C++ namespace `atx::rpc::v1`. Generated headers included as `"atx/rpc/v1/surface_service.grpc.pb.h"` etc. Stubs `atx::rpc::v1::SurfaceService::NewStub(channel)`, `atx::rpc::v1::AdminService::NewStub(channel)`; service bases `SurfaceService::Service`, `AdminService::Service`.
- Every message/field name below is load-bearing — later tasks use them verbatim.

- [ ] **Step 1: Write keys.proto**

```protobuf
// atx-proto/atx/rpc/v1/keys.proto
syntax = "proto3";
package atx.rpc.v1;

// SpiderRock-style composable keys (TickerKey -> ExpiryKey -> OptionKey).
// No filesystem path ever appears in a key: data is addressed as
// (db_id, partition_key, symbol). Spec §3.
message SymbolKey { string symbol = 1; }
message ExpiryKey {
  SymbolKey symbol = 1;
  string expiry_iso = 2; // YYYY-MM-DD
}
message OptionKey {
  ExpiryKey expiry = 1;
  double strike = 2;
  bool is_call = 3;
}
message PartitionKey {
  string db_id = 1;
  string key = 2; // canonical partition key, e.g. a trading date
}
message SurfaceKey {
  string db_id = 1;
  string partition_key = 2;
  string symbol = 3;
}
```

- [ ] **Step 2: Write common.proto**

```protobuf
// atx-proto/atx/rpc/v1/common.proto
syntax = "proto3";
package atx.rpc.v1;

// Stamped on every response: the SpiderRock "live until replaced" analogue.
// A client revalidates by comparing db_generation instead of refetching.
message ResponseMeta {
  uint64 db_generation = 1; // catalog's db_source.generation for this db_id
  fixed64 content_hash = 2; // payload-scoped hash where defined (blob CRC); 0 otherwise
  sfixed64 server_ns = 3;   // server wall clock, ns since epoch
}

// Keyset pagination: `after` is the last key of the previous page ("" = start).
message Page {
  uint32 limit = 1; // 0 => server default (500), capped at 5000
  string after = 2;
}
message PageInfo {
  string next_after = 1; // pass as Page.after to continue; "" = exhausted
  bool truncated = 2;
}
```

- [ ] **Step 3: Write surface.proto**

Domain enums (FitPreset, VolCurveKind, CalendarRepair, SurfacePurpose, FitQualityMode, SurfaceState) travel as raw `uint32` wire values, NOT proto enums — atx-vol owns those enums and a proto mirror would drift. Clients that need semantics link atx-vol or use the blob path.

```protobuf
// atx-proto/atx/rpc/v1/surface.proto
syntax = "proto3";
package atx.rpc.v1;
import "atx/rpc/v1/keys.proto";
import "atx/rpc/v1/common.proto";

// Decoded summary of one symbol's stored fit configuration, plus the raw
// 256-byte DbSymbolRecord for full-fidelity consumers (hybrid model, spec §7).
message SymbolFitConfig {
  bool enabled = 1;
  uint32 preset = 2;          // atx::vol::FitPreset wire value
  bool pin_curve = 3;
  uint32 curve_kind = 4;      // atx::vol::VolCurveKind wire value
  double band_k = 5;
  uint32 calendar_repair = 6; // atx::vol::CalendarRepair wire value
  bool use_correction_cache = 7;
  bool score_parity = 8;
  bool enforce_calendar_floor = 9;
  bool use_deam_cache_for_fit = 10;
  bytes record_blob = 11;     // raw DbSymbolRecord (256 B), full fidelity
}

message Provenance {
  uint32 purpose = 1;       // atx::vol::SurfacePurpose
  uint32 quality_mode = 2;  // atx::vol::FitQualityMode
  uint32 state = 3;         // atx::vol::SurfaceState
  uint64 source_generation = 4;
  uint64 served_generation = 5;
  bool legacy_format = 6;
}

message ExpirySummary {
  uint32 index = 1;        // slice index, ascending T
  double years = 2;        // year fraction (the primary key; iso is derived)
  string expiry_iso = 3;   // approx: now_ns + years*365.25d, ACT/365.25, UTC
  double forward = 4;
  double atm_vol = 5;      // surface iv at (forward, years)
  double atm_total_variance = 6;
  uint64 strikes_used = 7;
  uint64 strikes_dropped = 8;
  uint32 curve_kind = 9;   // atx::vol::VolCurveKind of this slice
}

message SurfaceMeta {
  SurfaceKey key = 1;
  uint32 uid = 2;
  uint32 n_slices = 3;
  double spot = 4;
  double rate = 5;
  sfixed64 now_ns = 6;     // the surface's pricing timestamp
  Provenance provenance = 7;
  repeated ExpirySummary expiries = 8;
  ResponseMeta meta = 9;
}

message CurvePoint {
  double z = 1;            // normalized strike: K = F*exp(z*atm_vol*sqrt(T))
  double strike = 2;
  double iv = 3;
  // Populated only when VolCurveSlice.with_greeks. Side is OTM by convention:
  // strike >= forward => call, else put.
  double fair_value = 4;
  double delta = 5;
  double gamma = 6;
  double theta = 7;
  double vega = 8;
}

message VolCurveSlice {
  SurfaceKey key = 1;
  uint32 expiry_index = 2;
  double years = 3;
  string expiry_iso = 4;
  double forward = 5;
  double atm_vol = 6;
  bool with_greeks = 7;
  uint32 n_dropped_points = 8; // sampled points with non-finite iv, skipped
  repeated CurvePoint points = 9;
  ResponseMeta meta = 10;
}

// One symbol's raw ATXVSA2 surface record (spec §7 blob path). The client
// copies `record` into 64-byte-aligned storage that outlives the view, then
// calls atx::vol::PricedSurfaceView::create_over_record over it.
message SurfaceBlob {
  SurfaceKey key = 1;
  bytes record = 2;
  fixed64 atxvsa_schema_hash = 3; // ArchiveV2Header.schema_hash of the source
  fixed32 payload_crc32c = 4;     // record's directory-entry CRC
  uint32 uid = 5;
  uint32 n_slices = 6;
  ResponseMeta meta = 7;
}
```

- [ ] **Step 4: Write surface_service.proto**

```protobuf
// atx-proto/atx/rpc/v1/surface_service.proto
syntax = "proto3";
package atx.rpc.v1;
import "atx/rpc/v1/keys.proto";
import "atx/rpc/v1/common.proto";
import "atx/rpc/v1/surface.proto";

message ListDatabasesRequest {}
message DatabaseInfo {
  string db_id = 1;
  string kind = 2; // 'surface_db'
  uint64 generation = 3;
  uint32 symbol_count = 4;
  uint32 partition_count = 5;
  uint64 surface_count = 6;
}
message ListDatabasesResponse {
  repeated DatabaseInfo databases = 1; // entitled dbs only
  ResponseMeta meta = 2;
}

message ListSymbolsRequest {
  string db_id = 1;
  string query = 2; // "" = all; else FTS5 MATCH expression over symbol text
  Page page = 3;
}
message SymbolEntry {
  string symbol = 1;
  bool enabled = 2;
  uint32 preset = 3;
}
message ListSymbolsResponse {
  repeated SymbolEntry symbols = 1;
  PageInfo page = 2;
  ResponseMeta meta = 3;
}

message ListPartitionsRequest {
  string db_id = 1;
  Page page = 2;
}
message PartitionEntry {
  string key = 1;
  uint32 surface_count = 2;
  uint64 file_size = 3;
  sfixed64 created_ns = 4;
}
message ListPartitionsResponse {
  repeated PartitionEntry partitions = 1;
  PageInfo page = 2;
  ResponseMeta meta = 3;
}

// The coverage query (spec §5.2): which (partition, symbol) surfaces exist.
message ListSurfacesRequest {
  string db_id = 1;
  string symbol = 2;   // "" = all symbols
  string key_from = 3; // inclusive partition-key lower bound; "" = open
  string key_to = 4;   // inclusive upper bound; "" = open
  Page page = 5;
}
message SurfaceEntry {
  string partition_key = 1;
  string symbol = 2;
  uint32 expiry_count = 3;
  double spot = 4;
  uint32 model_kinds = 5; // OR of (1 << VolCurveKind) present in the surface
  uint32 risk_state = 6;  // atx::vol::SurfaceState
}
message ListSurfacesResponse {
  repeated SurfaceEntry surfaces = 1;
  PageInfo page = 2;
  ResponseMeta meta = 3;
}

message GetSymbolConfigRequest {
  string db_id = 1;
  string symbol = 2;
}
message GetSymbolConfigResponse {
  SymbolFitConfig config = 1;
  Provenance provenance = 2;
  bool has_provenance = 3;
  ResponseMeta meta = 4;
}

message GetSurfaceMetaRequest { SurfaceKey key = 1; }
message GetCurveRequest {
  SurfaceKey key = 1;
  uint32 expiry_index = 2;
  uint32 n_points = 3;  // 0 => 41; clamped to [5, 401]
  double z_window = 4;  // 0 => 2.0; clamped to (0, 6]
  bool with_greeks = 5;
}
message GetSurfaceBlobRequest {
  SurfaceKey key = 1;
  // If nonzero and != the archive's schema hash, the server returns
  // FAILED_PRECONDITION carrying both hashes instead of bytes the client
  // would mis-read. Spec §7.
  fixed64 client_atxvsa_schema_hash = 2;
}

service SurfaceService {
  rpc ListDatabases(ListDatabasesRequest) returns (ListDatabasesResponse);
  rpc ListSymbols(ListSymbolsRequest) returns (ListSymbolsResponse);
  rpc ListPartitions(ListPartitionsRequest) returns (ListPartitionsResponse);
  rpc ListSurfaces(ListSurfacesRequest) returns (ListSurfacesResponse);
  rpc GetSymbolConfig(GetSymbolConfigRequest) returns (GetSymbolConfigResponse);
  rpc GetSurfaceMeta(GetSurfaceMetaRequest) returns (SurfaceMeta);
  rpc GetCurve(GetCurveRequest) returns (VolCurveSlice);
  rpc GetSurfaceBlob(GetSurfaceBlobRequest) returns (SurfaceBlob);
}
```

- [ ] **Step 5: Write admin.proto**

```protobuf
// atx-proto/atx/rpc/v1/admin.proto
syntax = "proto3";
package atx.rpc.v1;
import "atx/rpc/v1/common.proto";

message HealthRequest {}
message HealthResponse { bool serving = 1; }

message GetServerInfoRequest {}
message ServerInfo {
  string version = 1;     // "0.1.0"
  string server_uuid = 2; // stable per state dir (state.kv 'server_uuid')
  sfixed64 start_ns = 3;
  sfixed64 uptime_ns = 4;
  uint32 database_count = 5;
}

message GetStatsRequest {}
message CacheStats {
  string db_id = 1;
  uint64 resident = 2; // SurfaceDb partition view cache occupancy
  uint64 capacity = 3;
}
message ServerStats {
  uint64 requests_total = 1;
  uint64 requests_failed = 2;
  repeated CacheStats caches = 3;
}

// Realm import/export format (spec §4.4). ExportRealm is the ONE sanctioned
// path-bearing response; RegisterDatabase is the one path-bearing request.
// Both require an authenticated token like every RPC.
message RealmEntry {
  string db_id = 1;
  string kind = 2; // 'surface_db'
  string root = 3; // absolute directory of the SurfaceDb
}
message RealmConfig { repeated RealmEntry databases = 1; }

message RegisterDatabaseRequest { RealmEntry database = 1; }
message RegisterDatabaseResponse {
  bool indexed = 1;
  uint64 generation = 2;
}

message ExportRealmRequest {}

service AdminService {
  rpc Health(HealthRequest) returns (HealthResponse);
  rpc GetServerInfo(GetServerInfoRequest) returns (ServerInfo);
  rpc GetStats(GetStatsRequest) returns (ServerStats);
  rpc RegisterDatabase(RegisterDatabaseRequest) returns (RegisterDatabaseResponse);
  rpc ExportRealm(ExportRealmRequest) returns (RealmConfig);
}
```

- [ ] **Step 6: Write atx-proto/CMakeLists.txt**

```cmake
# ---- atx-proto: generated gRPC/protobuf wire contract (atx.rpc.v1) ----------
# Its own target, deliberately WITHOUT atx_warnings: generated code does not
# survive /W4 /WX. Consumers get the generated headers as SYSTEM includes.
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(ATX_PROTO_FILES
    atx/rpc/v1/keys.proto
    atx/rpc/v1/common.proto
    atx/rpc/v1/surface.proto
    atx/rpc/v1/surface_service.proto
    atx/rpc/v1/admin.proto
)

add_library(atx-proto STATIC ${ATX_PROTO_FILES})
target_link_libraries(atx-proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
target_include_directories(atx-proto SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

protobuf_generate(
    TARGET atx-proto
    LANGUAGE cpp
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)
protobuf_generate(
    TARGET atx-proto
    LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=\$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)
```

- [ ] **Step 7: Build the target**

Run: `cmake --preset server` (re-configure now that `atx-proto/` exists; `add_subdirectory(atx-server)` still fails — comment it out? NO: guard order in root CMakeLists is `atx-proto` then `atx-server`; temporarily the `atx-server` line will error. To keep Task 1's wiring intact, create a stub `atx-server/CMakeLists.txt` containing only a comment line `# populated in Task 3`.)

Then: `cmake --build --preset server --target atx-proto`
Expected: protoc + grpc_cpp_plugin run; `atx-proto.lib` links. Generated files land under `build-server/atx-proto/atx/rpc/v1/*.pb.{h,cc}` and `*.grpc.pb.{h,cc}`.

- [ ] **Step 8: Commit**

```bash
git add atx-proto/ atx-server/CMakeLists.txt
git commit -m "feat(proto): add atx.rpc.v1 contract package (keys, surface, admin) with generated gRPC target"
```

---

### Task 3: atx-server skeleton + ServerConfig

**Files:**
- Modify: `atx-server/CMakeLists.txt` (replace Task 2's stub)
- Create: `atx-server/include/atx/server/config.hpp`
- Create: `atx-server/src/config.cpp`
- Create: `atx-server/tests/CMakeLists.txt`
- Create: `atx-server/tests/config_test.cpp`

**Interfaces:**
- Produces:
  - `struct atx::server::ServerConfig { std::string listen{"127.0.0.1:50051"}; std::string state_dir{"atx-server-state"}; std::string tls_cert_path{}; std::string tls_key_path{}; std::size_t max_blob_bytes{16ull << 20}; std::size_t partition_cache_capacity{16}; int refresh_interval_ms{2000}; }`
  - `[[nodiscard]] atx::core::Status atx::server::validate_listen(const ServerConfig &cfg);`
  - CMake targets `atx-server-lib` (static, links atx::vol + atx-proto + OpenSSL::Crypto + atx_warnings) and test exe `atx-server-tests`.

- [ ] **Step 1: Write atx-server/CMakeLists.txt**

```cmake
# ---- atx-server: the ATX gRPC data server -----------------------------------
find_package(OpenSSL REQUIRED)

add_library(atx-server-lib STATIC
    src/config.cpp
)
add_library(atx::server ALIAS atx-server-lib)
target_include_directories(atx-server-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(atx-server-lib PUBLIC cxx_std_20)
target_link_libraries(atx-server-lib
    PUBLIC atx-proto atx::vol atx::core
    PRIVATE OpenSSL::Crypto atx_warnings
)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
```

(`src/` gains one line per task; `tools/` executables arrive in Task 15.)

- [ ] **Step 2: Write config.hpp**

```cpp
#pragma once

// atx-server runtime configuration + the non-loopback-requires-TLS gate
// (spec §8): binding anything but loopback without TLS must refuse to start.

#include <cstddef>
#include <string>

#include "atx/core/error.hpp" // Result, Status

namespace atx::server {

struct ServerConfig {
  std::string listen{"127.0.0.1:50051"};
  std::string state_dir{"atx-server-state"}; // holds state.db + catalog_snapshot.db
  std::string tls_cert_path{};
  std::string tls_key_path{};
  std::size_t max_blob_bytes{16ull << 20};      // RESOURCE_EXHAUSTED above this
  std::size_t partition_cache_capacity{16};     // forwarded to SurfaceDbOpenOpts
  int refresh_interval_ms{2000};                // CatalogRefresher cadence
};

// True iff `listen`'s host part is loopback: "127.", "localhost", "::1",
// "[::1]". Pure string classification; no sockets.
[[nodiscard]] bool is_loopback_listen(const std::string &listen);

// Ok iff (loopback) OR (both TLS paths set). Err(InvalidArgument) otherwise,
// naming the missing TLS configuration in the message.
[[nodiscard]] atx::core::Status validate_listen(const ServerConfig &cfg);

} // namespace atx::server
```

- [ ] **Step 3: Write the failing tests**

```cpp
// atx-server/tests/config_test.cpp
#include <gtest/gtest.h>

#include "atx/server/config.hpp"

namespace atx::server {
namespace {

TEST(Config, LoopbackClassification) {
  EXPECT_TRUE(is_loopback_listen("127.0.0.1:50051"));
  EXPECT_TRUE(is_loopback_listen("localhost:50051"));
  EXPECT_TRUE(is_loopback_listen("[::1]:50051"));
  EXPECT_FALSE(is_loopback_listen("0.0.0.0:50051"));
  EXPECT_FALSE(is_loopback_listen("192.168.1.10:50051"));
  EXPECT_FALSE(is_loopback_listen("[::]:50051"));
}

TEST(Config, LoopbackWithoutTlsIsOk) {
  ServerConfig cfg;
  EXPECT_TRUE(validate_listen(cfg).has_value());
}

TEST(Config, NonLoopbackWithoutTlsRefused) {
  ServerConfig cfg;
  cfg.listen = "0.0.0.0:50051";
  const auto st = validate_listen(cfg);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), core::ErrorCode::InvalidArgument);
  EXPECT_NE(st.error().message().find("TLS"), std::string::npos);
}

TEST(Config, NonLoopbackWithTlsOk) {
  ServerConfig cfg;
  cfg.listen = "0.0.0.0:50051";
  cfg.tls_cert_path = "cert.pem";
  cfg.tls_key_path = "key.pem";
  EXPECT_TRUE(validate_listen(cfg).has_value());
}

TEST(Config, HalfTlsRefused) {
  ServerConfig cfg;
  cfg.listen = "10.0.0.5:50051";
  cfg.tls_cert_path = "cert.pem"; // key missing
  EXPECT_FALSE(validate_listen(cfg).has_value());
}

} // namespace
} // namespace atx::server
```

And `atx-server/tests/CMakeLists.txt`:

```cmake
add_executable(atx-server-tests
    config_test.cpp
)
target_link_libraries(atx-server-tests PRIVATE
    atx::server GTest::gtest GTest::gtest_main atx_warnings
)
include(GoogleTest)
gtest_discover_tests(atx-server-tests
    PROPERTIES LABELS atx_server
    DISCOVERY_MODE PRE_TEST)
```

- [ ] **Step 4: Run tests to verify they fail**

Run: `cmake --build --preset server --target atx-server-tests`
Expected: link failure — `is_loopback_listen` / `validate_listen` undefined.

- [ ] **Step 5: Implement config.cpp**

```cpp
#include "atx/server/config.hpp"

#include <string_view>

namespace atx::server {

using core::Err;
using core::ErrorCode;
using core::Ok;
using core::Status;

bool is_loopback_listen(const std::string &listen) {
  std::string_view host{listen};
  if (!host.empty() && host.front() == '[') { // bracketed IPv6: [::1]:port
    const auto close = host.find(']');
    host = (close == std::string_view::npos) ? host.substr(1) : host.substr(1, close - 1);
  } else {
    const auto colon = host.rfind(':');
    if (colon != std::string_view::npos && host.find(':') == colon) {
      host = host.substr(0, colon); // exactly one ':' => host:port
    }
    // >1 ':' and no bracket => bare IPv6 literal; keep whole string as host.
  }
  return host == "localhost" || host == "::1" || host.starts_with("127.");
}

Status validate_listen(const ServerConfig &cfg) {
  if (is_loopback_listen(cfg.listen)) {
    return Ok();
  }
  if (cfg.tls_cert_path.empty() || cfg.tls_key_path.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "listen address '" + cfg.listen +
                   "' is not loopback: refusing to start without TLS "
                   "(--tls-cert and --tls-key are both required)");
  }
  return Ok();
}

} // namespace atx::server
```

Note: check `error.hpp` for the exact `Ok`/`Err` helper spellings before writing (they are free functions in `atx::core`; `Ok()` for `Status`). If the codebase spells them differently, follow the codebase.

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build --preset server --target atx-server-tests && ctest --test-dir build-server -L atx_server --output-on-failure`
Expected: all `Config.*` PASS.

- [ ] **Step 7: Commit**

```bash
git add atx-server/
git commit -m "feat(server): atx-server skeleton with ServerConfig and non-loopback TLS gate"
```

---

### Task 4: Catalog — open, schema, snapshot plumbing

**Files:**
- Create: `atx-server/include/atx/server/catalog.hpp`
- Create: `atx-server/src/catalog.cpp`
- Create: `atx-server/tests/catalog_test.cpp`
- Modify: `atx-server/CMakeLists.txt` (add `src/catalog.cpp`)
- Modify: `atx-server/tests/CMakeLists.txt` (add `catalog_test.cpp`)
- Modify: `atx-core/include/atx/core/db/sqlite.hpp:22-27` (threading-note amendment)

**Interfaces:**
- Consumes: `atx::core::db::Database` (`open_memory`, `exec`, `prepare`, `prepare_cached`, `pragma`, `backup_to`), `atx::core::db::Transaction`.
- Produces:
  - `class atx::server::Catalog` — `static core::Result<Catalog> open(std::string_view state_db_path);` move-only.
  - `core::db::Database &Catalog::db()` and `std::mutex &Catalog::mu()` — the ONE connection + its lock. Callers (indexer, services) lock `mu()` around every use of `db()`.
  - `core::Status Catalog::snapshot_to(std::string_view path)` — online-backup `main` to a file (tmp+rename).
  - `core::Status Catalog::restore_main_from(std::string_view snapshot_path)` — backup a snapshot file INTO `main`.
- Schema: exactly spec §5.1 with the CHECK constraints of §5.1's "closed value domains".

- [ ] **Step 1: Amend the sqlite.hpp threading note (spec §6 decision)**

In `atx-core/include/atx/core/db/sqlite.hpp`, replace the lines

```
//  A single `Database` (connection) must NOT be shared across threads. Each
//  thread that touches SQLite owns its own `Database`. Multiple `Database`
//  objects on multiple threads are safe concurrently. See
//  third-party/sqlite/PROVENANCE.md.
```

with

```
//  SQLite's actual contract at THREADSAFE=2: a single connection must not be
//  used by two threads SIMULTANEOUSLY. The recommended pattern remains one
//  `Database` per thread (no locking to reason about). A single `Database`
//  MAY be shared across threads iff every use is serialized by an external
//  mutex — atx-server's Catalog does this deliberately because its ':memory:'
//  main db is private to one connection and cannot be per-thread (see
//  docs/superpowers/specs/2026-07-26-atx-server-surface-service-design.md §6).
//  Multiple `Database` objects on multiple threads remain safe concurrently.
//  See third-party/sqlite/PROVENANCE.md.
```

- [ ] **Step 2: Write catalog.hpp**

```cpp
#pragma once

// Catalog — the embedded-SQLite SRSE analogue (spec §5). Two tiers on ONE
// connection: `main` (:memory:) is the DERIVED coverage index, rebuilt from
// the realm's SurfaceDbs; `state` (ATTACHed file, WAL) is AUTHORITATIVE —
// realm, tokens, entitlements, kv. Thread model (spec §6): this single
// connection is guarded by `mu()`; callers hold the lock across every use of
// `db()` and do all expensive work (archive decode, file IO) OUTSIDE it.

#include <memory>
#include <mutex>
#include <string_view>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"

namespace atx::server {

inline constexpr int kCatalogSchemaVersion = 1;

class Catalog {
public:
  // Open ':memory:' main, ATTACH `state_db_path` as state (created if absent,
  // WAL, busy_timeout 5000 ms), create all tables, stamp schema_version.
  [[nodiscard]] static core::Result<Catalog> open(std::string_view state_db_path);

  Catalog(Catalog &&) noexcept = default;
  Catalog &operator=(Catalog &&) noexcept = default;
  Catalog(const Catalog &) = delete;
  Catalog &operator=(const Catalog &) = delete;

  // The shared connection + its lock. Lock mu() around EVERY db() use.
  [[nodiscard]] core::db::Database &db() noexcept { return *db_; }
  [[nodiscard]] std::mutex &mu() noexcept { return *mu_; }

  // Online-backup `main` to `path` (spec §5.4 warm start). Writes `path`+".tmp"
  // then renames. Takes the lock internally.
  [[nodiscard]] core::Status snapshot_to(std::string_view path);
  // Restore: backup the snapshot file INTO `main`, replacing it. Lock held.
  [[nodiscard]] core::Status restore_main_from(std::string_view snapshot_path);

private:
  Catalog() = default;
  std::unique_ptr<std::mutex> mu_{};
  std::unique_ptr<core::db::Database> db_{};
};

} // namespace atx::server
```

- [ ] **Step 3: Write the failing tests**

```cpp
// atx-server/tests/catalog_test.cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "atx/server/catalog.hpp"

namespace atx::server {
namespace {

namespace fs = std::filesystem;

struct TempDir {
  fs::path path;
  TempDir() : path(fs::temp_directory_path() /
                   ("atx_srv_cat_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                    "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
    fs::create_directories(path);
  }
  ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
  [[nodiscard]] std::string state_db() const { return (path / "state.db").string(); }
};

TEST(Catalog, OpenCreatesSchema) {
  TempDir tmp;
  auto cat = Catalog::open(tmp.state_db());
  ASSERT_TRUE(cat.has_value()) << cat.error().to_string();
  std::scoped_lock lk(cat->mu());
  // main tables exist and are empty
  for (const char *sql : {"SELECT COUNT(*) FROM db_source", "SELECT COUNT(*) FROM symbol",
                          "SELECT COUNT(*) FROM partition", "SELECT COUNT(*) FROM surface"}) {
    auto st = cat->db().prepare(sql);
    ASSERT_TRUE(st.has_value()) << sql;
    auto step = st->step();
    ASSERT_TRUE(step.has_value());
    EXPECT_EQ(st->column_int(0), 0);
  }
  // state schema_version stamped
  auto st = cat->db().prepare("SELECT version FROM state.schema_version");
  ASSERT_TRUE(st.has_value());
  ASSERT_TRUE(st->step().has_value());
  EXPECT_EQ(st->column_int(0), kCatalogSchemaVersion);
}

TEST(Catalog, ReopenIsIdempotentAndStatePersists) {
  TempDir tmp;
  {
    auto cat = Catalog::open(tmp.state_db());
    ASSERT_TRUE(cat.has_value());
    std::scoped_lock lk(cat->mu());
    ASSERT_TRUE(cat->db()
                    .exec("INSERT INTO state.kv(key, value) VALUES('probe', x'01')")
                    .has_value());
  }
  auto cat2 = Catalog::open(tmp.state_db());
  ASSERT_TRUE(cat2.has_value());
  std::scoped_lock lk(cat2->mu());
  auto st = cat2->db().prepare("SELECT COUNT(*) FROM state.kv WHERE key='probe'");
  ASSERT_TRUE(st.has_value());
  ASSERT_TRUE(st->step().has_value());
  EXPECT_EQ(st->column_int(0), 1);
}

TEST(Catalog, CheckConstraintRejectsBadKind) {
  TempDir tmp;
  auto cat = Catalog::open(tmp.state_db());
  ASSERT_TRUE(cat.has_value());
  std::scoped_lock lk(cat->mu());
  EXPECT_FALSE(cat->db()
                   .exec("INSERT INTO state.realm(db_id, kind, root, added_ns) "
                         "VALUES('x', 'mystery', 'r', 0)")
                   .has_value());
}

TEST(Catalog, EntitlementForeignKeyEnforced) {
  TempDir tmp;
  auto cat = Catalog::open(tmp.state_db());
  ASSERT_TRUE(cat.has_value());
  std::scoped_lock lk(cat->mu());
  // entitlement referencing an absent token must fail (FK on by default).
  EXPECT_FALSE(cat->db()
                   .exec("INSERT INTO state.entitlement(token_sha256, db_id, mode) "
                         "VALUES(zeroblob(32), 'db1', 'read')")
                   .has_value());
}

TEST(Catalog, SnapshotAndRestoreRoundTripsMain) {
  TempDir tmp;
  auto cat = Catalog::open(tmp.state_db());
  ASSERT_TRUE(cat.has_value());
  {
    std::scoped_lock lk(cat->mu());
    ASSERT_TRUE(cat->db()
                    .exec("INSERT INTO db_source(db_id, kind, generation, scanned_ns) "
                          "VALUES('d1', 'surface_db', 7, 123)")
                    .has_value());
  }
  const std::string snap = (tmp.path / "snap.db").string();
  ASSERT_TRUE(cat->snapshot_to(snap).has_value());

  auto cat2 = Catalog::open((tmp.path / "state2.db").string());
  ASSERT_TRUE(cat2.has_value());
  ASSERT_TRUE(cat2->restore_main_from(snap).has_value());
  std::scoped_lock lk(cat2->mu());
  auto st = cat2->db().prepare("SELECT generation FROM db_source WHERE db_id='d1'");
  ASSERT_TRUE(st.has_value());
  ASSERT_TRUE(st->step().has_value());
  EXPECT_EQ(st->column_int(0), 7);
}

} // namespace
} // namespace atx::server
```

- [ ] **Step 4: Run to verify failure**

Run: `cmake --build --preset server --target atx-server-tests`
Expected: compile/link failure (no catalog.hpp implementation).

- [ ] **Step 5: Implement catalog.cpp**

```cpp
#include "atx/server/catalog.hpp"

#include <filesystem>
#include <string>

#include "atx/core/error.hpp"

namespace atx::server {

using core::Err;
using core::ErrorCode;
using core::Ok;
using core::Result;
using core::Status;
namespace db = core::db;

namespace {

// state (authoritative) — IF NOT EXISTS: survives restart.
constexpr const char *kStateDdl = R"sql(
CREATE TABLE IF NOT EXISTS state.schema_version(version INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS state.realm(
  db_id TEXT PRIMARY KEY,
  kind TEXT NOT NULL CHECK(kind IN ('surface_db')),
  root TEXT NOT NULL,
  added_ns INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS state.token(
  token_sha256 BLOB PRIMARY KEY CHECK(length(token_sha256) = 32),
  label TEXT NOT NULL UNIQUE,
  created_ns INTEGER NOT NULL,
  disabled INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS state.entitlement(
  token_sha256 BLOB NOT NULL REFERENCES token(token_sha256),
  db_id TEXT NOT NULL,
  mode TEXT NOT NULL CHECK(mode IN ('read')),
  PRIMARY KEY(token_sha256, db_id));
CREATE TABLE IF NOT EXISTS state.kv(key TEXT PRIMARY KEY, value BLOB);
)sql";

// main (derived, :memory:) — plain CREATE: always fresh per process.
constexpr const char *kMainDdl = R"sql(
CREATE TABLE db_source(
  db_id TEXT PRIMARY KEY,
  kind TEXT NOT NULL CHECK(kind IN ('surface_db')),
  generation INTEGER NOT NULL,
  scanned_ns INTEGER NOT NULL);
CREATE TABLE symbol(
  db_id TEXT NOT NULL,
  symbol TEXT NOT NULL,
  enabled INTEGER NOT NULL,
  preset INTEGER NOT NULL,
  curve_kind INTEGER NOT NULL,
  config_blob BLOB NOT NULL,
  provenance_blob BLOB,
  PRIMARY KEY(db_id, symbol));
CREATE TABLE partition(
  db_id TEXT NOT NULL,
  key TEXT NOT NULL,
  surface_count INTEGER NOT NULL,
  file_size INTEGER NOT NULL,
  created_ns INTEGER NOT NULL,
  PRIMARY KEY(db_id, key));
CREATE TABLE surface(
  db_id TEXT NOT NULL,
  part_key TEXT NOT NULL,
  symbol TEXT NOT NULL,
  expiry_count INTEGER NOT NULL,
  spot REAL NOT NULL,
  model_kind INTEGER NOT NULL,
  risk_state INTEGER NOT NULL,
  PRIMARY KEY(db_id, part_key, symbol));
CREATE INDEX surface_by_symbol ON surface(symbol, part_key);
CREATE VIRTUAL TABLE symbol_fts USING fts5(symbol, tags, content='');
)sql";

// Escape a path for embedding in single-quoted SQL (ATTACH takes no binds
// through exec; double any single quote).
std::string sql_quote(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('\'');
  for (char c : s) {
    if (c == '\'') out.push_back('\'');
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

} // namespace

Result<Catalog> Catalog::open(std::string_view state_db_path) {
  ATX_TRY(auto mem, db::Database::open_memory());
  Catalog cat;
  cat.mu_ = std::make_unique<std::mutex>();
  cat.db_ = std::make_unique<db::Database>(std::move(mem));
  auto &d = *cat.db_;

  if (auto st = d.exec("ATTACH DATABASE " + sql_quote(state_db_path) + " AS state"); !st) {
    return Err(st.error().code(), "ATTACH state failed: " + st.error().message());
  }
  ATX_TRY_VOID(d.set_busy_timeout(5000));
  // WAL applies to the file-backed attached db; :memory: main has no journal.
  ATX_TRY_VOID(d.pragma("state.journal_mode", "WAL"));
  ATX_TRY_VOID(d.exec(kStateDdl));
  ATX_TRY_VOID(d.exec(kMainDdl));

  // Stamp schema_version exactly once.
  ATX_TRY(auto count_stmt, d.prepare("SELECT COUNT(*) FROM state.schema_version"));
  ATX_TRY(auto step, count_stmt.step());
  if (count_stmt.column_int(0) == 0) {
    ATX_TRY_VOID(d.exec("INSERT INTO state.schema_version(version) VALUES(1)"));
  } else {
    ATX_TRY(auto ver_stmt, d.prepare("SELECT version FROM state.schema_version"));
    ATX_TRY(auto vstep, ver_stmt.step());
    if (ver_stmt.column_int(0) != kCatalogSchemaVersion) {
      return Err(ErrorCode::ParseError,
                 "state schema version " + std::to_string(ver_stmt.column_int(0)) +
                     " != expected " + std::to_string(kCatalogSchemaVersion));
    }
  }
  return cat;
}

Status Catalog::snapshot_to(std::string_view path) {
  const std::string tmp = std::string(path) + ".tmp";
  {
    std::scoped_lock lk(*mu_);
    std::error_code ec;
    std::filesystem::remove(tmp, ec); // stale tmp from a crashed run
    ATX_TRY(auto dest, db::Database::open(tmp, db::OpenMode::ReadWriteCreate));
    ATX_TRY_VOID(db_->backup_to(dest));
  } // dest closed here
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    // rename over an existing file can fail on Windows: remove then retry.
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, ec);
    if (ec) return Err(ErrorCode::IoError, "snapshot rename failed: " + ec.message());
  }
  return Ok();
}

Status Catalog::restore_main_from(std::string_view snapshot_path) {
  std::scoped_lock lk(*mu_);
  ATX_TRY(auto src, db::Database::open(snapshot_path, db::OpenMode::ReadOnly));
  return src.backup_to(*db_);
}

} // namespace atx::server
```

Implementation notes for the engineer:
- `ATX_TRY_VOID` — if `atx/core/macro.hpp` does not define a void-result TRY, use `if (auto st = ...; !st) return Err(st.error().code(), st.error().message());` — check the macro header first and follow whatever the codebase uses for `Status` propagation.
- `Database` is move-only; `unique_ptr<Database>` keeps `Catalog` movable without relying on `Database`'s move semantics inside `std::optional`.
- If `exec` refuses multi-statement scripts, split the DDL constants into one `exec` per statement (check `sqlite.cpp`'s exec loop first — it likely handles multi-statement text).

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build --preset server --target atx-server-tests && ctest --test-dir build-server -L atx_server --output-on-failure`
Expected: `Config.*` and `Catalog.*` all PASS. If `CheckConstraintRejectsBadKind` or the FK test passes vacuously, verify `SQLITE_DEFAULT_FOREIGN_KEYS=1` is active by asserting `PRAGMA foreign_keys` returns 1.

- [ ] **Step 7: Commit**

```bash
git add atx-server/ atx-core/include/atx/core/db/sqlite.hpp
git commit -m "feat(server): two-tier SQLite catalog (memory main + attached WAL state) with snapshot/restore"
```

---

### Task 5: Catalog realm, token, and entitlement CRUD

**Files:**
- Modify: `atx-server/include/atx/server/catalog.hpp`
- Modify: `atx-server/src/catalog.cpp`
- Modify: `atx-server/tests/catalog_test.cpp`

**Interfaces:**
- Produces (all take the internal lock; all `[[nodiscard]]`):
  - `struct RealmRow { std::string db_id, kind, root; };`
  - `core::Status Catalog::register_database(std::string_view db_id, std::string_view kind, std::string_view root);` — upsert (`INSERT OR REPLACE`); `InvalidArgument` on empty id/root.
  - `core::Result<std::vector<RealmRow>> Catalog::realm() const;` — ordered by db_id.
  - `core::Status Catalog::add_token(std::string_view label, std::span<const std::byte, 32> digest);` — `AlreadyExists` on duplicate digest or label.
  - `core::Status Catalog::disable_token(std::span<const std::byte, 32> digest);` — `NotFound` if absent.
  - `core::Result<std::array<std::byte, 32>> Catalog::digest_by_label(std::string_view label) const;` — `NotFound` if absent (used by `--grant LABEL`).
  - `enum class TokenState { Unknown, Disabled, Active };`
  - `core::Result<TokenState> Catalog::token_state(std::span<const std::byte, 32> digest) const;`
  - `core::Status Catalog::grant(std::span<const std::byte, 32> digest, std::string_view db_id);` — mode fixed `'read'`; `NotFound` if token unknown.
  - `core::Result<std::vector<std::string>> Catalog::entitled_dbs(std::span<const std::byte, 32> digest) const;` — sorted.

- [ ] **Step 1: Write the failing tests (append to catalog_test.cpp)**

```cpp
std::array<std::byte, 32> digest_of(unsigned char fill) {
  std::array<std::byte, 32> d{};
  d.fill(std::byte{fill});
  return d;
}

TEST(Catalog, RealmRegisterAndList) {
  TempDir tmp;
  auto cat = Catalog::open(tmp.state_db());
  ASSERT_TRUE(cat.has_value());
  ASSERT_TRUE(cat->register_database("mag7", "surface_db", "C:/data/mag7").has_value());
  ASSERT_TRUE(cat->register_database("spy", "surface_db", "C:/data/spy").has_value());
  // upsert: re-register with a new root replaces
  ASSERT_TRUE(cat->register_database("spy", "surface_db", "D:/data/spy").has_value());
  EXPECT_FALSE(cat->register_database("", "surface_db", "r").has_value());
  auto rows = cat->realm();
  ASSERT_TRUE(rows.has_value());
  ASSERT_EQ(rows->size(), 2u);
  EXPECT_EQ((*rows)[0].db_id, "mag7");
  EXPECT_EQ((*rows)[1].root, "D:/data/spy");
}

TEST(Catalog, TokenLifecycleAndEntitlements) {
  TempDir tmp;
  auto cat = Catalog::open(tmp.state_db());
  ASSERT_TRUE(cat.has_value());
  const auto d1 = digest_of(0xA1);

  EXPECT_EQ(cat->token_state(d1).value(), TokenState::Unknown);
  ASSERT_TRUE(cat->add_token("ui-desktop", d1).has_value());
  EXPECT_EQ(cat->token_state(d1).value(), TokenState::Active);
  EXPECT_EQ(cat->add_token("ui-desktop", digest_of(0xB2)).error().code(),
            core::ErrorCode::AlreadyExists); // duplicate label

  ASSERT_TRUE(cat->register_database("mag7", "surface_db", "C:/data/mag7").has_value());
  ASSERT_TRUE(cat->grant(d1, "mag7").has_value());
  EXPECT_EQ(cat->grant(digest_of(0xCC), "mag7").error().code(), core::ErrorCode::NotFound);

  auto dbs = cat->entitled_dbs(d1);
  ASSERT_TRUE(dbs.has_value());
  ASSERT_EQ(dbs->size(), 1u);
  EXPECT_EQ((*dbs)[0], "mag7");

  auto found = cat->digest_by_label("ui-desktop");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, d1);

  ASSERT_TRUE(cat->disable_token(d1).has_value());
  EXPECT_EQ(cat->token_state(d1).value(), TokenState::Disabled);
}
```

(Add `#include <array>`, `#include <span>` to the test file.)

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build --preset server --target atx-server-tests`
Expected: compile failure — methods missing.

- [ ] **Step 3: Implement in catalog.cpp**

Representative implementations (all follow the same bind/step shape; SQLite bind indices are 1-based, columns 0-based):

```cpp
Status Catalog::register_database(std::string_view db_id, std::string_view kind,
                                  std::string_view root) {
  if (db_id.empty() || root.empty()) {
    return Err(ErrorCode::InvalidArgument, "db_id and root must be non-empty");
  }
  std::scoped_lock lk(*mu_);
  ATX_TRY(auto stmt, db_->prepare_cached(
      "INSERT OR REPLACE INTO state.realm(db_id, kind, root, added_ns) "
      "VALUES(?1, ?2, ?3, ?4)"));
  ATX_TRY_VOID(stmt->bind(1, db_id));
  ATX_TRY_VOID(stmt->bind(2, kind));
  ATX_TRY_VOID(stmt->bind(3, root));
  ATX_TRY_VOID(stmt->bind(4, static_cast<core::i64>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count())));
  ATX_TRY(auto step, stmt->step());
  return Ok();
}

Result<Catalog::TokenState> Catalog::token_state(std::span<const std::byte, 32> digest) const {
  std::scoped_lock lk(*mu_);
  ATX_TRY(auto stmt, db_->prepare_cached(
      "SELECT disabled FROM state.token WHERE token_sha256 = ?1"));
  ATX_TRY_VOID(stmt->bind(1, std::span<const std::byte>{digest}));
  ATX_TRY(auto step, stmt->step());
  if (step == db::Statement::Step::Done) return TokenState::Unknown;
  return stmt->column_int(0) != 0 ? TokenState::Disabled : TokenState::Active;
}
```

`add_token`: `INSERT INTO state.token(...) VALUES(...)` — map the constraint
failure to `AlreadyExists` (SQLite returns a constraint error; wrap:
`if (!step) return Err(ErrorCode::AlreadyExists, "token or label exists");`).
`grant`: first `token_state` == Unknown => `Err(NotFound)`; then
`INSERT OR REPLACE INTO state.entitlement(token_sha256, db_id, mode) VALUES(?1, ?2, 'read')`.
`disable_token`: `UPDATE state.token SET disabled=1 WHERE token_sha256=?1`, then
`db_->changes() == 0 => Err(NotFound)`.
`entitled_dbs`: `SELECT db_id FROM state.entitlement WHERE token_sha256=?1 ORDER BY db_id`, loop `step()` until `Done`.
`digest_by_label`: `SELECT token_sha256 FROM state.token WHERE label=?1`, copy the 32-byte blob out, `NotFound` when `Done` first.
`realm()`: `SELECT db_id, kind, root FROM state.realm ORDER BY db_id`.
`const` methods: `mu_`/`db_` are behind pointers, so locking in `const` methods is fine without `mutable`.

Note on `TokenState`: declare it INSIDE `class Catalog` (`Catalog::TokenState`) or at namespace scope — pick namespace scope (matches the Interfaces block: `atx::server::TokenState`), and keep the Interfaces block spelling everywhere later.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build --preset server --target atx-server-tests && ctest --test-dir build-server -L atx_server --output-on-failure`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add atx-server/
git commit -m "feat(server): catalog realm, token, and entitlement CRUD"
```

---

### Task 6: Auth — token digest + entitlement resolution

**Files:**
- Create: `atx-server/include/atx/server/auth.hpp`
- Create: `atx-server/src/auth.cpp`
- Create: `atx-server/tests/auth_test.cpp`
- Modify: `atx-server/CMakeLists.txt` (+ `src/auth.cpp`), `atx-server/tests/CMakeLists.txt` (+ `auth_test.cpp`)

**Interfaces:**
- Consumes: `Catalog::token_state`, `Catalog::entitled_dbs` (Task 5).
- Produces:
  - `core::Result<std::array<std::byte, 32>> atx::server::token_digest(std::string_view token);` — SHA-256 via OpenSSL `EVP_Digest`.
  - `bool atx::server::constant_time_equal(std::span<const std::byte, 32> a, std::span<const std::byte, 32> b);` — `CRYPTO_memcmp`.
  - `struct Entitlements { std::vector<std::string> db_ids; [[nodiscard]] bool entitled(std::string_view db_id) const; };`
  - `core::Result<Entitlements> atx::server::authorize_token(std::string_view token, Catalog &cat);` — Unknown/Disabled token => `Err(PermissionDenied, "unauthenticated: ...")`. Handlers surface this as gRPC `UNAUTHENTICATED` (Task 10 wires that; the message prefix is NOT sniffed — handlers call `authorize_token` before `run_handler` and map its failure to UNAUTHENTICATED directly).
  - Metadata key constant: `inline constexpr std::string_view kTokenMetadataKey = "x-atx-token";`

- [ ] **Step 1: Write the failing tests**

```cpp
// atx-server/tests/auth_test.cpp
#include <gtest/gtest.h>

#include <array>
#include <cstddef>

#include "atx/server/auth.hpp"
#include "atx/server/catalog.hpp"

#include <filesystem>

namespace atx::server {
namespace {

// FIPS-180 test vector: sha256("abc").
constexpr const char *kAbcHex =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

std::string to_hex(std::span<const std::byte> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (std::byte b : bytes) {
    out.push_back(kHex[std::to_integer<unsigned>(b) >> 4]);
    out.push_back(kHex[std::to_integer<unsigned>(b) & 0xF]);
  }
  return out;
}

TEST(Auth, DigestMatchesKnownVector) {
  auto d = token_digest("abc");
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(to_hex(*d), kAbcHex);
}

TEST(Auth, ConstantTimeEqual) {
  auto a = token_digest("abc").value();
  auto b = token_digest("abc").value();
  auto c = token_digest("abd").value();
  EXPECT_TRUE(constant_time_equal(a, b));
  EXPECT_FALSE(constant_time_equal(a, c));
}

TEST(Auth, AuthorizeTokenAgainstCatalog) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "atx_srv_auth";
  fs::create_directories(dir);
  auto cat = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(cat.has_value());

  // unknown token
  auto e = authorize_token("nope", *cat);
  ASSERT_FALSE(e.has_value());
  EXPECT_EQ(e.error().code(), core::ErrorCode::PermissionDenied);

  // active token, one grant
  const auto d = token_digest("secret-token").value();
  ASSERT_TRUE(cat->add_token("t1", d).has_value());
  ASSERT_TRUE(cat->register_database("mag7", "surface_db", "C:/x").has_value());
  ASSERT_TRUE(cat->grant(d, "mag7").has_value());
  auto ok = authorize_token("secret-token", *cat);
  ASSERT_TRUE(ok.has_value());
  EXPECT_TRUE(ok->entitled("mag7"));
  EXPECT_FALSE(ok->entitled("spy"));

  // disabled token
  ASSERT_TRUE(cat->disable_token(d).has_value());
  EXPECT_FALSE(authorize_token("secret-token", *cat).has_value());
  std::error_code ec;
  fs::remove_all(dir, ec);
}

} // namespace
} // namespace atx::server
```

- [ ] **Step 2: Run to verify failure** — compile failure, header missing.

- [ ] **Step 3: Implement auth.hpp / auth.cpp**

```cpp
// auth.hpp
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/server/catalog.hpp"

namespace atx::server {

inline constexpr std::string_view kTokenMetadataKey = "x-atx-token";

[[nodiscard]] core::Result<std::array<std::byte, 32>> token_digest(std::string_view token);
[[nodiscard]] bool constant_time_equal(std::span<const std::byte, 32> a,
                                       std::span<const std::byte, 32> b);

struct Entitlements {
  std::vector<std::string> db_ids; // sorted
  [[nodiscard]] bool entitled(std::string_view db_id) const;
};

// Digest the presented token, look it up, resolve entitlements. Unknown or
// disabled tokens fail with PermissionDenied; callers surface UNAUTHENTICATED.
[[nodiscard]] core::Result<Entitlements> authorize_token(std::string_view token, Catalog &cat);

} // namespace atx::server
```

```cpp
// auth.cpp
#include "atx/server/auth.hpp"

#include <algorithm>

#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace atx::server {

using core::Err;
using core::ErrorCode;
using core::Result;

Result<std::array<std::byte, 32>> token_digest(std::string_view token) {
  std::array<std::byte, 32> out{};
  unsigned int len = 0;
  if (EVP_Digest(token.data(), token.size(), reinterpret_cast<unsigned char *>(out.data()),
                 &len, EVP_sha256(), nullptr) != 1 ||
      len != out.size()) {
    return Err(ErrorCode::Internal, "EVP_Digest(sha256) failed");
  }
  return out;
}

bool constant_time_equal(std::span<const std::byte, 32> a, std::span<const std::byte, 32> b) {
  return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

bool Entitlements::entitled(std::string_view db_id) const {
  return std::binary_search(db_ids.begin(), db_ids.end(), db_id);
}

Result<Entitlements> authorize_token(std::string_view token, Catalog &cat) {
  ATX_TRY(auto digest, token_digest(token));
  ATX_TRY(auto state, cat.token_state(digest));
  if (state != TokenState::Active) {
    return Err(ErrorCode::PermissionDenied, "unauthenticated: unknown or disabled token");
  }
  ATX_TRY(auto dbs, cat.entitled_dbs(digest));
  return Entitlements{std::move(dbs)};
}

} // namespace atx::server
```

Note: the digest lookup is an exact-match SQL `WHERE token_sha256 = ?1` (an index probe on the primary key). `constant_time_equal` exists for any future in-memory comparisons; SQLite's B-tree probe on a 32-byte random digest does not leak usable timing about token VALUES (the attacker-controlled preimage is already hashed). Do not "improve" this by iterating all tokens with CRYPTO_memcmp.

- [ ] **Step 4: Run tests to verify they pass** — `ctest --test-dir build-server -L atx_server --output-on-failure`.

- [ ] **Step 5: Commit**

```bash
git add atx-server/
git commit -m "feat(server): token auth via OpenSSL sha256 digests with catalog-backed entitlements"
```

---

### Task 7: Test fixture SurfaceDb + SurfaceRegistry

**Files:**
- Create: `atx-server/tests/test_support.hpp`
- Create: `atx-server/tests/test_support.cpp`
- Create: `atx-server/include/atx/server/surface_registry.hpp`
- Create: `atx-server/src/surface_registry.cpp`
- Create: `atx-server/tests/registry_test.cpp`
- Modify: `atx-server/CMakeLists.txt`, `atx-server/tests/CMakeLists.txt` (add `test_support.cpp` to the test target sources)

**Interfaces:**
- Consumes: `atx::vol::SurfaceDb::{create, open, write_partition, upsert_symbol}`, `atx::vol::PricedSurface::create`, `EssviCurve`, `CurveSurface`, `SliceContext`, `PricingContext`, `SurfaceArchiveItem`, `SymbolFitConfig`, `SurfaceProvenance` (patterns from `atx-vol/tests/surface_db_test.cpp:196-233`).
- Produces:
  - test_support: `atx::vol::PricedSurface atx::server::testing::make_essvi(std::uint32_t uid, int n_slices);` (verbatim port of `surface_db_test.cpp`'s `make_essvi` + its `make_pricing` helper, S=100.0, r=0.045) and
    `struct FixtureDb { std::string root; std::vector<std::string> keys; std::vector<std::string> symbols; };`
    `FixtureDb atx::server::testing::make_fixture_db(const std::filesystem::path &root, int n_days);` — creates a SurfaceDb at `root`; for day `i` in `[0, n_days)`: partition key `"2026-07-2" + std::to_string(i)`, containing symbols `{"SPY", "AAPL"}` (uids `100+i`, `200+i`, 3 and 4 slices respectively), each written with explicit `SurfaceProvenance{}` defaults; `upsert_symbol("SPY", symbol_config_from_preset(FitPreset::Populate))` once.
  - registry: `class SurfaceRegistry { public: explicit SurfaceRegistry(Catalog &cat, std::size_t partition_cache_capacity); [[nodiscard]] core::Result<std::shared_ptr<atx::vol::SurfaceDb>> get(std::string_view db_id); [[nodiscard]] std::vector<std::pair<std::string, atx::vol::SurfaceDbCacheStats>> cache_stats() const; };`
    `get` resolves `db_id` via `Catalog::realm()`, opens lazily with `SurfaceDbOpenOpts{partition_cache_capacity}`, caches the `shared_ptr` in a mutex-guarded map. Unknown db_id => `Err(NotFound)`. SurfaceDb const queries are thread-safe (surface_db.hpp:384-397), so one shared instance serves all handler threads.

- [ ] **Step 1: Write test_support (fixture builder)**

Port `make_pricing`/`make_essvi` from `atx-vol/tests/surface_db_test.cpp:196-233` verbatim (constants `kArchS = 100.0`, `kArchR = 0.045`; includes: `atx/vol/black76.hpp`, `atx/vol/calib.hpp`, `atx/vol/priced_surface.hpp`, `atx/vol/surface_db.hpp`, `atx/vol/vol_curve.hpp`). Then:

```cpp
// test_support.cpp (make_fixture_db)
FixtureDb make_fixture_db(const std::filesystem::path &root, int n_days) {
  FixtureDb out;
  out.root = root.string();
  out.symbols = {"AAPL", "SPY"}; // canonical (upper) sorted order
  auto db = vol::SurfaceDb::create(out.root);
  if (!db.has_value()) { ADD_FAILURE() << db.error().to_string(); return out; }
  for (int i = 0; i < n_days; ++i) {
    const std::string key = "2026-07-2" + std::to_string(i);
    vol::PricedSurface spy = make_essvi(100u + static_cast<std::uint32_t>(i), 3);
    vol::PricedSurface aapl = make_essvi(200u + static_cast<std::uint32_t>(i), 4);
    std::vector<vol::SurfaceArchiveItem> items{
        {"SPY", &spy, vol::SurfaceProvenance{}},
        {"AAPL", &aapl, vol::SurfaceProvenance{}},
    };
    if (auto st = db->write_partition(key, items); !st.has_value()) {
      ADD_FAILURE() << key << ": " << st.error().to_string();
      return out;
    }
    out.keys.push_back(key);
  }
  auto cfg = vol::symbol_config_from_preset(vol::FitPreset::Populate);
  if (auto st = db->upsert_symbol("SPY", cfg); !st.has_value()) {
    ADD_FAILURE() << st.error().to_string();
  }
  return out;
}
```

- [ ] **Step 2: Write the failing registry test**

```cpp
// atx-server/tests/registry_test.cpp
#include <gtest/gtest.h>

#include <filesystem>

#include "atx/server/catalog.hpp"
#include "atx/server/surface_registry.hpp"
#include "test_support.hpp"

namespace atx::server {
namespace {
namespace fs = std::filesystem;

TEST(Registry, OpensRegisteredDbLazilyAndCaches) {
  const fs::path dir = fs::temp_directory_path() / "atx_srv_reg";
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto fixture = testing::make_fixture_db(dir / "db", 2);

  auto cat = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(cat.has_value());
  ASSERT_TRUE(cat->register_database("fix", "surface_db", fixture.root).has_value());

  SurfaceRegistry reg(*cat, 16);
  EXPECT_EQ(reg.get("nope").error().code(), core::ErrorCode::NotFound);

  auto db1 = reg.get("fix");
  ASSERT_TRUE(db1.has_value()) << db1.error().to_string();
  EXPECT_EQ((*db1)->partitions().size(), 2u);
  auto db2 = reg.get("fix");
  ASSERT_TRUE(db2.has_value());
  EXPECT_EQ(db1->get(), db2->get()); // same cached instance

  auto stats = reg.cache_stats();
  ASSERT_EQ(stats.size(), 1u);
  EXPECT_EQ(stats[0].first, "fix");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

} // namespace
} // namespace atx::server
```

- [ ] **Step 3: Run to verify failure** — compile failure.

- [ ] **Step 4: Implement surface_registry**

```cpp
// surface_registry.hpp
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/server/catalog.hpp"
#include "atx/vol/surface_db.hpp"

namespace atx::server {

// Lazily-opened SurfaceDb per db_id (spec §4.3). SurfaceDb const queries are
// documented thread-safe, so one shared instance serves all handler threads;
// this map only guards its own bookkeeping.
class SurfaceRegistry {
public:
  SurfaceRegistry(Catalog &cat, std::size_t partition_cache_capacity)
      : cat_{cat}, cache_capacity_{partition_cache_capacity} {}

  [[nodiscard]] core::Result<std::shared_ptr<vol::SurfaceDb>> get(std::string_view db_id);
  [[nodiscard]] std::vector<std::pair<std::string, vol::SurfaceDbCacheStats>> cache_stats() const;

private:
  Catalog &cat_;
  std::size_t cache_capacity_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<vol::SurfaceDb>> open_;
};

} // namespace atx::server
```

```cpp
// surface_registry.cpp
#include "atx/server/surface_registry.hpp"

namespace atx::server {

using core::Err;
using core::ErrorCode;
using core::Result;

Result<std::shared_ptr<vol::SurfaceDb>> SurfaceRegistry::get(std::string_view db_id) {
  {
    std::scoped_lock lk(mu_);
    if (auto it = open_.find(std::string(db_id)); it != open_.end()) return it->second;
  }
  // Resolve the root OUTSIDE our lock (catalog has its own), open OUTSIDE both.
  ATX_TRY(auto realm, cat_.realm());
  const RealmRow *row = nullptr;
  for (const auto &r : realm) {
    if (r.db_id == db_id) { row = &r; break; }
  }
  if (row == nullptr) return Err(ErrorCode::NotFound, "unknown db_id: " + std::string(db_id));
  vol::SurfaceDbOpenOpts opts;
  opts.partition_cache_capacity = cache_capacity_;
  ATX_TRY(auto db, vol::SurfaceDb::open(row->root, opts));
  auto shared = std::make_shared<vol::SurfaceDb>(std::move(db));
  std::scoped_lock lk(mu_);
  auto [it, inserted] = open_.try_emplace(std::string(db_id), std::move(shared));
  return it->second; // a racing opener's instance wins; both are valid
}

std::vector<std::pair<std::string, vol::SurfaceDbCacheStats>>
SurfaceRegistry::cache_stats() const {
  std::scoped_lock lk(mu_);
  std::vector<std::pair<std::string, vol::SurfaceDbCacheStats>> out;
  out.reserve(open_.size());
  for (const auto &[id, db] : open_) out.emplace_back(id, db->partition_cache_stats());
  return out;
}

} // namespace atx::server
```

- [ ] **Step 5: Run tests to verify they pass.** If `SurfaceArchiveItem`'s provenance field rejects aggregate init as written, check `surface_archive.hpp:342-348` and adjust (it is `std::optional<SurfaceProvenance>`; `{"SPY", &spy, vol::SurfaceProvenance{}}` is valid aggregate init).

- [ ] **Step 6: Commit**

```bash
git add atx-server/
git commit -m "feat(server): SurfaceRegistry with lazy shared SurfaceDb opens plus test fixture builder"
```

---

### Task 8: Catalog indexer — scan a SurfaceDb into the coverage index

**Files:**
- Create: `atx-server/include/atx/server/catalog_index.hpp`
- Create: `atx-server/src/catalog_index.cpp`
- Create: `atx-server/tests/index_test.cpp`
- Modify: `atx-server/CMakeLists.txt`, `atx-server/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `SurfaceDb::{manifest, generation, partitions, open_partition}`, `DbManifest::symbols()` (span of `DbSymbolRecord`), `decode_symbol_record`, `decode_symbol_provenance`, `SurfaceArchiveV2::{directory, map_all_with_provenance}`, `ArchiveV2DirEntry` fields (`symbol`, `symbol_len`, `n_slices`, `kind_bits`), `Catalog::db()/mu()`, Task 7's fixture.
- Produces:
  - `core::Status atx::server::index_surface_db(Catalog &cat, std::string_view db_id, const vol::SurfaceDb &sdb);` — one transaction: delete this db_id's rows from `db_source/symbol/partition/surface`, re-insert from the SurfaceDb, upsert `db_source(db_id, 'surface_db', generation, scanned_ns=now)`, rebuild `symbol_fts` (delete-all + repopulate from `symbol` across ALL dbs — contentless FTS5 supports only whole-table rebuild cheaply; symbol counts are small).
  - `core::Result<std::uint64_t> atx::server::indexed_generation(Catalog &cat, std::string_view db_id);` — `db_source.generation`, `NotFound` if the db was never indexed.
  - `core::Status atx::server::bootstrap_catalog(Catalog &cat, SurfaceRegistry &reg);` — for every realm row: open via registry; if `indexed_generation != SurfaceDb::generation()` (or NotFound) => `index_surface_db`. Covers cold start AND warm-start drift with one code path (spec §5.4).
- **Ordering constraint:** ALL archive IO (`open_partition`, `map_all_with_provenance`) happens BEFORE taking the catalog lock; the lock covers only the SQL transaction. Stage rows into plain structs first.

- [ ] **Step 1: Write the failing tests**

```cpp
// atx-server/tests/index_test.cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "atx/server/catalog.hpp"
#include "atx/server/catalog_index.hpp"
#include "atx/server/surface_registry.hpp"
#include "test_support.hpp"

namespace atx::server {
namespace {
namespace fs = std::filesystem;

// Dump every `main` row deterministically, excluding db_source.scanned_ns
// (spec §5.4: the one documented warm-vs-cold difference).
std::string dump_main(Catalog &cat) {
  std::string out;
  std::scoped_lock lk(cat.mu());
  const char *queries[] = {
      "SELECT db_id, kind, generation FROM db_source ORDER BY db_id",
      "SELECT db_id, symbol, enabled, preset, curve_kind, hex(config_blob) FROM symbol "
      "ORDER BY db_id, symbol",
      "SELECT db_id, key, surface_count, file_size, created_ns FROM partition "
      "ORDER BY db_id, key",
      "SELECT db_id, part_key, symbol, expiry_count, spot, model_kind, risk_state "
      "FROM surface ORDER BY db_id, part_key, symbol",
  };
  for (const char *sql : queries) {
    auto st = cat.db().prepare(sql);
    if (!st.has_value()) { ADD_FAILURE() << sql; return out; }
    while (true) {
      auto step = st->step();
      if (!step.has_value() || *step == core::db::Statement::Step::Done) break;
      for (core::i32 c = 0; c < st->column_count(); ++c) {
        out += std::string(st->column_text(c));
        out += '|';
      }
      out += '\n';
    }
  }
  return out;
}

struct IndexFixture : ::testing::Test {
  fs::path dir = fs::temp_directory_path() / "atx_srv_idx";
  void SetUp() override { fs::remove_all(dir); fs::create_directories(dir); }
  void TearDown() override { std::error_code ec; fs::remove_all(dir, ec); }
};

TEST_F(IndexFixture, IndexPopulatesAllTables) {
  auto fixture = testing::make_fixture_db(dir / "db", 3);
  auto cat = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(cat.has_value());
  auto sdb = vol::SurfaceDb::open(fixture.root);
  ASSERT_TRUE(sdb.has_value());

  ASSERT_TRUE(index_surface_db(*cat, "fix", *sdb).has_value());

  std::scoped_lock lk(cat->mu());
  auto count = [&](const char *sql) {
    auto st = cat->db().prepare(sql);
    EXPECT_TRUE(st.has_value());
    EXPECT_TRUE(st->step().has_value());
    return st->column_int(0);
  };
  EXPECT_EQ(count("SELECT COUNT(*) FROM db_source"), 1);
  EXPECT_EQ(count("SELECT COUNT(*) FROM symbol WHERE db_id='fix'"), 1);       // SPY config
  EXPECT_EQ(count("SELECT COUNT(*) FROM partition WHERE db_id='fix'"), 3);
  EXPECT_EQ(count("SELECT COUNT(*) FROM surface WHERE db_id='fix'"), 6);      // 3 days x 2 syms
  // the coverage query: SPY days
  EXPECT_EQ(count("SELECT COUNT(*) FROM surface WHERE db_id='fix' AND symbol='SPY'"), 3);
  // FTS finds SPY
  EXPECT_EQ(count("SELECT COUNT(*) FROM symbol_fts WHERE symbol_fts MATCH 'SPY'"), 1);
}

TEST_F(IndexFixture, ReindexIsDeterministic) {
  auto fixture = testing::make_fixture_db(dir / "db", 2);
  auto cat = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(cat.has_value());
  auto sdb = vol::SurfaceDb::open(fixture.root);
  ASSERT_TRUE(sdb.has_value());
  ASSERT_TRUE(index_surface_db(*cat, "fix", *sdb).has_value());
  const std::string first = dump_main(*cat);
  ASSERT_TRUE(index_surface_db(*cat, "fix", *sdb).has_value());
  EXPECT_EQ(dump_main(*cat), first);
}

TEST_F(IndexFixture, BootstrapIndexesOnlyDriftedDbs) {
  auto fixture = testing::make_fixture_db(dir / "db", 2);
  auto cat = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(cat.has_value());
  ASSERT_TRUE(cat->register_database("fix", "surface_db", fixture.root).has_value());
  SurfaceRegistry reg(*cat, 16);

  ASSERT_TRUE(bootstrap_catalog(*cat, reg).has_value());
  auto gen1 = indexed_generation(*cat, "fix");
  ASSERT_TRUE(gen1.has_value());

  // no drift => bootstrap must not change anything
  const std::string before = dump_main(*cat);
  ASSERT_TRUE(bootstrap_catalog(*cat, reg).has_value());
  EXPECT_EQ(dump_main(*cat), before);

  // drift: write a new partition through a second writer handle
  {
    auto writer = vol::SurfaceDb::open(fixture.root);
    ASSERT_TRUE(writer.has_value());
    auto s = testing::make_essvi(999, 3);
    std::vector<vol::SurfaceArchiveItem> items{{"SPY", &s, vol::SurfaceProvenance{}}};
    ASSERT_TRUE(writer->write_partition("2026-07-30", items).has_value());
  }
  ASSERT_TRUE((*reg.get("fix"))->refresh().has_value());
  ASSERT_TRUE(bootstrap_catalog(*cat, reg).has_value());
  auto gen2 = indexed_generation(*cat, "fix");
  ASSERT_TRUE(gen2.has_value());
  EXPECT_GT(*gen2, *gen1);
  std::scoped_lock lk(cat->mu());
  auto st = cat->db().prepare(
      "SELECT COUNT(*) FROM partition WHERE db_id='fix' AND key='2026-07-30'");
  ASSERT_TRUE(st.has_value());
  ASSERT_TRUE(st->step().has_value());
  EXPECT_EQ(st->column_int(0), 1);
}

} // namespace
} // namespace atx::server
```

- [ ] **Step 2: Run to verify failure** — compile failure.

- [ ] **Step 3: Implement catalog_index.cpp**

Structure (stage outside the lock, commit inside):

```cpp
#include "atx/server/catalog_index.hpp"

#include <chrono>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_db.hpp"

namespace atx::server {

using core::Err;
using core::ErrorCode;
using core::Ok;
using core::Result;
using core::Status;
namespace db = core::db;

namespace {

struct StagedSymbol {
  std::string symbol;
  bool enabled{};
  int preset{};
  int curve_kind{};
  std::vector<std::byte> config_blob; // raw 256-byte DbSymbolRecord
  bool has_provenance{};
};
struct StagedSurface {
  std::string part_key, symbol;
  int expiry_count{};
  double spot{};
  int model_kind{}; // ArchiveV2DirEntry::kind_bits (OR of 1<<VolCurveKind)
  int risk_state{};
};

} // namespace

Status index_surface_db(Catalog &cat, std::string_view db_id, const vol::SurfaceDb &sdb) {
  // ── Stage: ALL archive IO before the catalog lock ──────────────────────────
  const std::uint64_t generation = sdb.generation();
  auto manifest = sdb.manifest();

  std::vector<StagedSymbol> symbols;
  for (const vol::DbSymbolRecord &rec : manifest->symbols()) {
    StagedSymbol s;
    s.symbol.assign(rec.symbol, rec.symbol_len);
    const vol::SymbolFitConfig cfg = vol::decode_symbol_record(rec);
    s.enabled = cfg.enabled;
    s.preset = static_cast<int>(cfg.preset);
    s.curve_kind = static_cast<int>(cfg.curve.kind);
    s.config_blob.resize(sizeof(rec));
    std::memcpy(s.config_blob.data(), &rec, sizeof(rec));
    s.has_provenance = vol::decode_symbol_provenance(rec).has_value();
    symbols.push_back(std::move(s));
  }

  const std::vector<vol::DbPartitionInfo> partitions = sdb.partitions();

  std::vector<StagedSurface> surfaces;
  for (const auto &p : partitions) {
    ATX_TRY(auto archive, sdb.open_partition(p.key));
    ATX_TRY(auto views, archive.map_all_with_provenance()); // directory order
    const auto dir = archive.directory();
    for (std::size_t i = 0; i < dir.size(); ++i) {
      StagedSurface s;
      s.part_key = p.key;
      s.symbol.assign(dir[i].symbol, dir[i].symbol_len);
      s.expiry_count = static_cast<int>(dir[i].n_slices);
      s.model_kind = static_cast<int>(dir[i].kind_bits);
      s.spot = views[i].view.pricing().S;
      s.risk_state = static_cast<int>(views[i].provenance.state);
      surfaces.push_back(std::move(s));
    }
  }

  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

  // ── Commit: one transaction under the catalog lock ─────────────────────────
  std::scoped_lock lk(cat.mu());
  auto &d = cat.db();
  ATX_TRY(auto txn, db::Transaction::begin(d));
  const std::string id{db_id};
  for (const char *del : {"DELETE FROM surface WHERE db_id = ?1",
                          "DELETE FROM partition WHERE db_id = ?1",
                          "DELETE FROM symbol WHERE db_id = ?1",
                          "DELETE FROM db_source WHERE db_id = ?1"}) {
    ATX_TRY(auto stmt, d.prepare_cached(del));
    ATX_TRY_VOID(stmt->bind(1, id));
    ATX_TRY(auto step, stmt->step());
  }
  {
    ATX_TRY(auto stmt, d.prepare_cached(
        "INSERT INTO db_source(db_id, kind, generation, scanned_ns) "
        "VALUES(?1, 'surface_db', ?2, ?3)"));
    ATX_TRY_VOID(stmt->bind(1, id));
    ATX_TRY_VOID(stmt->bind(2, static_cast<core::i64>(generation)));
    ATX_TRY_VOID(stmt->bind(3, static_cast<core::i64>(now_ns)));
    ATX_TRY(auto step, stmt->step());
  }
  // symbol / partition / surface inserts: same bind/step pattern per staged row.
  // symbol: INSERT INTO symbol(db_id, symbol, enabled, preset, curve_kind,
  //                            config_blob, provenance_blob)
  //         VALUES(?1,?2,?3,?4,?5,?6, NULL)   (provenance_blob reserved; the
  //         has_provenance bit rides in the decoded proto path, Task 12)
  // partition: INSERT INTO partition(db_id, key, surface_count, file_size,
  //                                  created_ns) VALUES(?1,?2,?3,?4,?5)
  // surface: INSERT INTO surface(db_id, part_key, symbol, expiry_count, spot,
  //                              model_kind, risk_state) VALUES(?1,...,?7)
  // FTS rebuild (contentless: whole-table):
  ATX_TRY_VOID(d.exec("INSERT INTO symbol_fts(symbol_fts) VALUES('delete-all')"));
  ATX_TRY_VOID(d.exec(
      "INSERT INTO symbol_fts(rowid, symbol, tags) SELECT rowid, symbol, db_id FROM symbol"));
  return txn.commit();
}

Result<std::uint64_t> indexed_generation(Catalog &cat, std::string_view db_id) {
  std::scoped_lock lk(cat.mu());
  ATX_TRY(auto stmt, cat.db().prepare_cached(
      "SELECT generation FROM db_source WHERE db_id = ?1"));
  ATX_TRY_VOID(stmt->bind(1, db_id));
  ATX_TRY(auto step, stmt->step());
  if (step == db::Statement::Step::Done) {
    return Err(ErrorCode::NotFound, "db never indexed: " + std::string(db_id));
  }
  return static_cast<std::uint64_t>(stmt->column_int(0));
}

Status bootstrap_catalog(Catalog &cat, SurfaceRegistry &reg) {
  ATX_TRY(auto realm, cat.realm());
  for (const auto &row : realm) {
    ATX_TRY(auto sdb, reg.get(row.db_id));
    auto indexed = indexed_generation(cat, row.db_id);
    if (indexed.has_value() && *indexed == sdb->generation()) continue;
    ATX_TRY_VOID(index_surface_db(cat, row.db_id, *sdb));
  }
  return Ok();
}
```

(The elided symbol/partition/surface insert loops follow exactly the shown bind/step pattern — write them out fully; blob bind is `stmt->bind(6, std::span<const std::byte>{s.config_blob})`.)

- [ ] **Step 4: Run tests to verify they pass.** Determinism note: if `ReindexIsDeterministic` flakes on `hex(config_blob)`, the DbSymbolRecord staging copied uninitialized padding — it does not (the record comes off the manifest byte-for-byte), so a mismatch is a real bug.

- [ ] **Step 5: Commit**

```bash
git add atx-server/
git commit -m "feat(server): catalog indexer with staged-outside-lock scans and drift-aware bootstrap"
```

---

### Task 9: encode — pure domain-to-proto translators

**Files:**
- Create: `atx-server/include/atx/server/encode.hpp`
- Create: `atx-server/src/encode.cpp`
- Create: `atx-server/tests/encode_test.cpp`
- Modify: `atx-server/CMakeLists.txt`, `atx-server/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `vol::PricedSurface::{context, pricing, iv, total_variance, kind_at, n_slices, uid, greeks, fair_value}` (`context()` returns `std::span<const vol::SliceContext>`; `SliceContext{T, forward, borrow, q_eff, n_used, n_dropped}` from `surface_parity.hpp:246`), `vol::SurfaceProvenance`, `vol::SymbolFitConfig`, `vol::AmericanGreeks{delta,gamma,vega,theta,rho,vanna,volga,charm,price}`, proto messages from Task 2.
- Produces (all pure, no gRPC, no IO — the load-bearing testable boundary, spec §4.3):
  - `std::string atx::server::approx_expiry_iso(std::int64_t now_ns, double t_years);` — `now_ns + llround(t_years * 365.25 * 86400) * 1'000'000'000` → UTC `YYYY-MM-DD`.
  - `void atx::server::encode_provenance(const vol::SurfaceProvenance &p, atx::rpc::v1::Provenance *out);`
  - `void atx::server::encode_symbol_config(const vol::SymbolFitConfig &c, std::span<const std::byte> record_blob, atx::rpc::v1::SymbolFitConfig *out);`
  - `core::Status atx::server::encode_surface_meta(const vol::PricedSurface &s, const vol::SurfaceProvenance &prov, atx::rpc::v1::SurfaceMeta *out);` — fills everything except `key` and `meta` (the service layer owns those).
  - `core::Status atx::server::encode_curve(const vol::PricedSurface &s, std::uint32_t expiry_index, std::uint32_t n_points, double z_window, bool with_greeks, atx::rpc::v1::VolCurveSlice *out);` — same key/meta exclusion. Defaults/clamps: `n_points==0 => 41`, clamp `[5, 401]`; `z_window<=0 => 2.0`, clamp `(0, 6]`. `InvalidArgument` if `expiry_index >= n_slices` or ATM iv non-finite.

- [ ] **Step 1: Write the failing tests**

```cpp
// atx-server/tests/encode_test.cpp
#include <gtest/gtest.h>

#include <cmath>

#include "atx/rpc/v1/surface.pb.h"
#include "atx/server/encode.hpp"
#include "test_support.hpp"

namespace atx::server {
namespace {

TEST(Encode, ApproxExpiryIso) {
  // 2023-11-14T22:13:20Z = 1700000000 s (the fixture's now_ts_ns).
  EXPECT_EQ(approx_expiry_iso(1700000000000000000LL, 0.0), "2023-11-14");
  // +1.0y (365.25 d) => 2024-11-14
  EXPECT_EQ(approx_expiry_iso(1700000000000000000LL, 1.0), "2024-11-14");
}

TEST(Encode, SurfaceMetaMatchesFixture) {
  const auto s = testing::make_essvi(42, 3);
  rpc::v1::SurfaceMeta meta;
  ASSERT_TRUE(encode_surface_meta(s, vol::SurfaceProvenance{}, &meta).has_value());
  EXPECT_EQ(meta.uid(), 42u);
  EXPECT_EQ(meta.n_slices(), 3u);
  EXPECT_DOUBLE_EQ(meta.spot(), 100.0);
  EXPECT_DOUBLE_EQ(meta.rate(), 0.045);
  ASSERT_EQ(meta.expiries_size(), 3);
  const auto ctx = s.context();
  for (int i = 0; i < 3; ++i) {
    const auto &e = meta.expiries(static_cast<int>(i));
    EXPECT_EQ(e.index(), static_cast<std::uint32_t>(i));
    EXPECT_DOUBLE_EQ(e.years(), ctx[static_cast<std::size_t>(i)].T);
    EXPECT_DOUBLE_EQ(e.forward(), ctx[static_cast<std::size_t>(i)].forward);
    EXPECT_DOUBLE_EQ(e.atm_vol(),
                     s.iv(ctx[static_cast<std::size_t>(i)].forward,
                          ctx[static_cast<std::size_t>(i)].T));
    EXPECT_TRUE(std::isfinite(e.atm_vol()));
    EXPECT_EQ(e.strikes_used(), ctx[static_cast<std::size_t>(i)].n_used);
  }
  // ascending T
  EXPECT_LT(meta.expiries(0).years(), meta.expiries(2).years());
}

TEST(Encode, CurveSamplesMatchSurfaceIv) {
  const auto s = testing::make_essvi(7, 3);
  rpc::v1::VolCurveSlice slice;
  ASSERT_TRUE(encode_curve(s, 1, 21, 2.0, false, &slice).has_value());
  EXPECT_EQ(slice.expiry_index(), 1u);
  EXPECT_GT(slice.points_size(), 0);
  for (const auto &p : slice.points()) {
    EXPECT_TRUE(std::isfinite(p.iv()));
    EXPECT_DOUBLE_EQ(p.iv(), s.iv(p.strike(), slice.years()));
    EXPECT_DOUBLE_EQ(p.fair_value(), 0.0); // no greeks requested
  }
  // z grid spans the window symmetrically
  EXPECT_DOUBLE_EQ(slice.points(0).z(), -2.0);
  EXPECT_DOUBLE_EQ(slice.points(slice.points_size() - 1).z(), 2.0);
}

TEST(Encode, CurveWithGreeksPopulatesOtmSide) {
  const auto s = testing::make_essvi(7, 3);
  rpc::v1::VolCurveSlice slice;
  ASSERT_TRUE(encode_curve(s, 0, 11, 1.5, true, &slice).has_value());
  for (const auto &p : slice.points()) {
    EXPECT_GT(p.fair_value(), 0.0);
    EXPECT_GT(p.vega(), 0.0);
    // OTM convention: strike >= forward => call (delta in [0,1]), else put.
    if (p.strike() >= slice.forward()) {
      EXPECT_GE(p.delta(), 0.0);
    } else {
      EXPECT_LE(p.delta(), 0.0);
    }
  }
}

TEST(Encode, CurveRejectsBadExpiryIndex) {
  const auto s = testing::make_essvi(7, 3);
  rpc::v1::VolCurveSlice slice;
  EXPECT_EQ(encode_curve(s, 3, 21, 2.0, false, &slice).error().code(),
            core::ErrorCode::InvalidArgument);
}

} // namespace
} // namespace atx::server
```

- [ ] **Step 2: Run to verify failure** — compile failure.

- [ ] **Step 3: Implement encode.cpp**

```cpp
#include "atx/server/encode.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "atx/vol/american.hpp"
#include "atx/vol/priced_surface.hpp"

namespace atx::server {

using core::Err;
using core::ErrorCode;
using core::Ok;
using core::Status;

std::string approx_expiry_iso(std::int64_t now_ns, double t_years) {
  using namespace std::chrono;
  const std::int64_t expiry_s =
      now_ns / 1'000'000'000 + llround(t_years * 365.25 * 86400.0);
  const sys_days d = floor<days>(sys_seconds{seconds{expiry_s}});
  const year_month_day ymd{d};
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
  return buf;
}

void encode_provenance(const vol::SurfaceProvenance &p, rpc::v1::Provenance *out) {
  out->set_purpose(static_cast<std::uint32_t>(p.purpose));
  out->set_quality_mode(static_cast<std::uint32_t>(p.quality_mode));
  out->set_state(static_cast<std::uint32_t>(p.state));
  out->set_source_generation(p.source_generation);
  out->set_served_generation(p.served_generation);
  out->set_legacy_format(p.legacy_format);
}

void encode_symbol_config(const vol::SymbolFitConfig &c, std::span<const std::byte> record_blob,
                          rpc::v1::SymbolFitConfig *out) {
  out->set_enabled(c.enabled);
  out->set_preset(static_cast<std::uint32_t>(c.preset));
  out->set_pin_curve(c.pin_curve);
  out->set_curve_kind(static_cast<std::uint32_t>(c.curve.kind));
  out->set_band_k(c.band_k);
  out->set_calendar_repair(static_cast<std::uint32_t>(c.calendar_repair));
  out->set_use_correction_cache(c.use_correction_cache);
  out->set_score_parity(c.score_parity);
  out->set_enforce_calendar_floor(c.enforce_calendar_floor);
  out->set_use_deam_cache_for_fit(c.use_deam_cache_for_fit);
  out->set_record_blob(record_blob.data(), record_blob.size());
}

Status encode_surface_meta(const vol::PricedSurface &s, const vol::SurfaceProvenance &prov,
                           rpc::v1::SurfaceMeta *out) {
  const auto &pc = s.pricing();
  out->set_uid(s.uid());
  out->set_n_slices(static_cast<std::uint32_t>(s.n_slices()));
  out->set_spot(pc.S);
  out->set_rate(pc.r);
  out->set_now_ns(pc.now_ts_ns);
  encode_provenance(prov, out->mutable_provenance());
  const auto ctx = s.context();
  for (std::size_t i = 0; i < ctx.size(); ++i) {
    auto *e = out->add_expiries();
    e->set_index(static_cast<std::uint32_t>(i));
    e->set_years(ctx[i].T);
    e->set_expiry_iso(approx_expiry_iso(pc.now_ts_ns, ctx[i].T));
    e->set_forward(ctx[i].forward);
    e->set_atm_vol(s.iv(ctx[i].forward, ctx[i].T));
    e->set_atm_total_variance(s.total_variance(ctx[i].forward, ctx[i].T));
    e->set_strikes_used(ctx[i].n_used);
    e->set_strikes_dropped(ctx[i].n_dropped);
    e->set_curve_kind(static_cast<std::uint32_t>(s.kind_at(i)));
  }
  return Ok();
}

Status encode_curve(const vol::PricedSurface &s, std::uint32_t expiry_index,
                    std::uint32_t n_points, double z_window, bool with_greeks,
                    rpc::v1::VolCurveSlice *out) {
  const auto ctx = s.context();
  if (expiry_index >= ctx.size()) {
    return Err(ErrorCode::InvalidArgument,
               "expiry_index " + std::to_string(expiry_index) + " >= n_slices " +
                   std::to_string(ctx.size()));
  }
  const std::uint32_t n = std::clamp<std::uint32_t>(n_points == 0 ? 41 : n_points, 5, 401);
  const double w = std::clamp(z_window <= 0.0 ? 2.0 : z_window, 1e-6, 6.0);
  const double T = ctx[expiry_index].T;
  const double F = ctx[expiry_index].forward;
  const double atm = s.iv(F, T);
  if (!std::isfinite(atm) || atm <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "ATM iv not finite at expiry index " +
                                               std::to_string(expiry_index));
  }
  out->set_expiry_index(expiry_index);
  out->set_years(T);
  out->set_expiry_iso(approx_expiry_iso(s.pricing().now_ts_ns, T));
  out->set_forward(F);
  out->set_atm_vol(atm);
  out->set_with_greeks(with_greeks);
  std::uint32_t dropped = 0;
  const double sqrt_t = std::sqrt(T);
  for (std::uint32_t i = 0; i < n; ++i) {
    const double z = -w + 2.0 * w * static_cast<double>(i) / static_cast<double>(n - 1);
    const double strike = F * std::exp(z * atm * sqrt_t);
    const double iv = s.iv(strike, T);
    if (!std::isfinite(iv)) {
      ++dropped;
      continue;
    }
    auto *p = out->add_points();
    p->set_z(z);
    p->set_strike(strike);
    p->set_iv(iv);
    if (with_greeks) {
      // OTM side convention (matches the proto comment): strike >= F => call.
      const vol::Side side = strike >= F ? vol::Side::Call : vol::Side::Put;
      auto g = s.greeks(strike, T, side);
      if (!g.has_value()) {
        ++dropped;
        out->mutable_points()->RemoveLast();
        continue;
      }
      p->set_fair_value(g->price);
      p->set_delta(g->delta);
      p->set_gamma(g->gamma);
      p->set_theta(g->theta);
      p->set_vega(g->vega);
    }
  }
  out->set_n_dropped_points(dropped);
  return Ok();
}

} // namespace atx::server
```

- [ ] **Step 4: Run tests to verify they pass** (the OTM-side test in Step 1 pins the side convention).

- [ ] **Step 5: Commit**

```bash
git add atx-server/
git commit -m "feat(server): pure domain-to-proto encoders for meta, expiries, curves, and configs"
```

---

### Task 10: service_error — Result-to-grpc::Status mapping + handler wrapper

**Files:**
- Create: `atx-server/include/atx/server/service_error.hpp`
- Create: `atx-server/src/service_error.cpp`
- Create: `atx-server/tests/service_error_test.cpp`
- Modify: `atx-server/CMakeLists.txt`, `atx-server/tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `grpc::Status atx::server::to_grpc_status(const core::Error &e);` — spec §8 table: `NotFound=>NOT_FOUND`, `InvalidArgument/OutOfRange=>INVALID_ARGUMENT`, `ParseError=>DATA_LOSS`, `IoError/Unavailable=>UNAVAILABLE`, `PermissionDenied=>PERMISSION_DENIED`, `AlreadyExists=>ALREADY_EXISTS`, `NotImplemented=>UNIMPLEMENTED`, everything else `INTERNAL`.
  - `template <typename Fn> grpc::Status atx::server::run_handler(std::string_view rpc, Fn &&fn) noexcept;` — `fn` returns `core::Status`. Maps `Err` through `to_grpc_status`; catches ALL exceptions => `INTERNAL` with a generated incident id in the message, exception text logged server-side only (spec §8: never on the wire).
  - `std::string atx::server::next_incident_id();` — monotonic counter + steady-clock ticks, e.g. `"inc-42-8f3a91"`.

- [ ] **Step 1: Write the failing tests**

```cpp
// atx-server/tests/service_error_test.cpp
#include <gtest/gtest.h>

#include <stdexcept>

#include "atx/server/service_error.hpp"

namespace atx::server {
namespace {

TEST(ServiceError, MapsDomainCodes) {
  using core::Error;
  using core::ErrorCode;
  EXPECT_EQ(to_grpc_status(Error{ErrorCode::NotFound}).error_code(), grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(to_grpc_status(Error{ErrorCode::InvalidArgument}).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(to_grpc_status(Error{ErrorCode::ParseError}).error_code(),
            grpc::StatusCode::DATA_LOSS);
  EXPECT_EQ(to_grpc_status(Error{ErrorCode::IoError}).error_code(),
            grpc::StatusCode::UNAVAILABLE);
  EXPECT_EQ(to_grpc_status(Error{ErrorCode::PermissionDenied}).error_code(),
            grpc::StatusCode::PERMISSION_DENIED);
  EXPECT_EQ(to_grpc_status(Error{ErrorCode::Internal}).error_code(),
            grpc::StatusCode::INTERNAL);
  // message travels
  EXPECT_EQ(to_grpc_status(Error{ErrorCode::NotFound, "no such db"}).error_message(),
            "no such db");
}

TEST(ServiceError, RunHandlerOkAndErr) {
  EXPECT_TRUE(run_handler("test", [] { return core::Ok(); }).ok());
  const auto st = run_handler("test", [] {
    return core::Err(core::ErrorCode::NotFound, "gone");
  });
  EXPECT_EQ(st.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST(ServiceError, RunHandlerSwallowsExceptionText) {
  const auto st = run_handler("test", []() -> core::Status {
    throw std::runtime_error("SECRET-INTERNAL-PATH C:/keys/x");
  });
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(st.error_message().find("SECRET"), std::string::npos);
  EXPECT_NE(st.error_message().find("inc-"), std::string::npos); // incident id present
}

} // namespace
} // namespace atx::server
```

- [ ] **Step 2: Run to verify failure.**

- [ ] **Step 3: Implement**

```cpp
// service_error.hpp
#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "atx/core/error.hpp"
#include "atx/core/log.hpp"

namespace atx::server {

[[nodiscard]] grpc::Status to_grpc_status(const core::Error &e);
[[nodiscard]] std::string next_incident_id();

// Every RPC body runs inside this: expected failures come back as mapped
// grpc::Status; NO exception escapes into gRPC (spec §8). The exception text
// goes to the server log only — the wire carries just the incident id.
template <typename Fn> [[nodiscard]] grpc::Status run_handler(std::string_view rpc, Fn &&fn) noexcept {
  try {
    core::Status st = std::forward<Fn>(fn)();
    if (st.has_value()) return grpc::Status::OK;
    return to_grpc_status(st.error());
  } catch (const std::exception &ex) {
    const std::string id = next_incident_id();
    ATX_LOG_ERROR("rpc {} incident {}: {}", rpc, id, ex.what());
    return {grpc::StatusCode::INTERNAL, "internal error, incident " + id};
  } catch (...) {
    const std::string id = next_incident_id();
    ATX_LOG_ERROR("rpc {} incident {}: non-std exception", rpc, id);
    return {grpc::StatusCode::INTERNAL, "internal error, incident " + id};
  }
}

} // namespace atx::server
```

Check `atx/core/log.hpp` for the actual logging macro spelling (it wraps spdlog; ~2 KB header) and use what exists — if there is no error macro, `spdlog::error` directly is acceptable in the .cpp-only path (move the catch bodies into a non-template helper `grpc::Status internal_incident(std::string_view rpc, std::string_view what) noexcept;` in service_error.cpp so the template stays log-free).

```cpp
// service_error.cpp
#include "atx/server/service_error.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>

namespace atx::server {

grpc::Status to_grpc_status(const core::Error &e) {
  using core::ErrorCode;
  grpc::StatusCode code = grpc::StatusCode::INTERNAL;
  switch (e.code()) {
  case ErrorCode::NotFound: code = grpc::StatusCode::NOT_FOUND; break;
  case ErrorCode::InvalidArgument:
  case ErrorCode::OutOfRange: code = grpc::StatusCode::INVALID_ARGUMENT; break;
  case ErrorCode::ParseError: code = grpc::StatusCode::DATA_LOSS; break;
  case ErrorCode::IoError:
  case ErrorCode::Unavailable: code = grpc::StatusCode::UNAVAILABLE; break;
  case ErrorCode::PermissionDenied: code = grpc::StatusCode::PERMISSION_DENIED; break;
  case ErrorCode::AlreadyExists: code = grpc::StatusCode::ALREADY_EXISTS; break;
  case ErrorCode::NotImplemented: code = grpc::StatusCode::UNIMPLEMENTED; break;
  case ErrorCode::Unknown:
  case ErrorCode::Internal: code = grpc::StatusCode::INTERNAL; break;
  }
  return {code, e.message()};
}

std::string next_incident_id() {
  static std::atomic<std::uint64_t> counter{0};
  const auto n = counter.fetch_add(1, std::memory_order_relaxed);
  const auto t = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  char buf[40];
  std::snprintf(buf, sizeof(buf), "inc-%llu-%06llx",
                static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(t & 0xFFFFFF));
  return buf;
}

} // namespace atx::server
```

- [ ] **Step 4: Run tests to verify they pass.**

- [ ] **Step 5: Commit**

```bash
git add atx-server/
git commit -m "feat(server): grpc status mapping and exception-proof handler wrapper with incident ids"
```

---

### Task 11: AdminService + in-process test harness + Server assembly

**Files:**
- Create: `atx-server/include/atx/server/service_admin.hpp`
- Create: `atx-server/src/service_admin.cpp`
- Create: `atx-server/include/atx/server/server.hpp`
- Create: `atx-server/src/server.cpp`
- Create: `atx-server/tests/service_test.cpp` (harness + admin tests; surface tests appended in Tasks 12-13)
- Modify: `atx-server/CMakeLists.txt`, `atx-server/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 5-10 (`Catalog`, `authorize_token`, `SurfaceRegistry`, `index_surface_db`, `run_handler`, proto stubs).
- Produces:
  - `struct ServerDeps { Catalog &catalog; SurfaceRegistry &registry; const ServerConfig &config; };` (aggregate, passed by value into each service impl).
  - `class AdminServiceImpl final : public rpc::v1::AdminService::Service { public: AdminServiceImpl(ServerDeps deps, std::int64_t start_ns); ... };` — overrides all five RPCs. Every RPC (including Health) requires a valid token; auth failure => `UNAUTHENTICATED`.
  - `RegisterDatabase` = `Catalog::register_database` + open via registry + `index_surface_db`; response carries `indexed=true` and the generation.
  - `std::atomic<std::uint64_t> &requests_total()/requests_failed()` counters on `AdminServiceImpl`, shared via `ServerStatsCounters` — simplest: `struct StatsCounters { std::atomic<std::uint64_t> total{0}, failed{0}; };` owned by `Server`/test harness, `ServerDeps` gains `StatsCounters &stats;`.
  - `class Server { public: static core::Result<Server> build(const ServerConfig &cfg, Catalog &cat, SurfaceRegistry &reg, StatsCounters &stats); [[nodiscard]] grpc::Server &grpc(); [[nodiscard]] int port() const; void shutdown(); void wait(); };` — validates listen (Task 3), insecure creds on loopback / TLS creds otherwise, `SetMaxSendMessageSize(64 << 20)`, `AddListeningPort(..., &port_)`.
  - Auth pattern for EVERY handler in Tasks 11-13:

```cpp
grpc::Status AdminServiceImpl::Health(grpc::ServerContext *ctx, const rpc::v1::HealthRequest *,
                                      rpc::v1::HealthResponse *resp) {
  stats_hit();
  auto ent = authenticate(ctx);
  if (!ent.has_value()) return unauthenticated(ent.error());
  return run_handler("Health", [&]() -> core::Status {
    resp->set_serving(true);
    return core::Ok();
  });
}
```

  where the shared helpers live in a small `service_common.hpp` (created here):

```cpp
// atx-server/include/atx/server/service_common.hpp
#pragma once
#include <atomic>
#include <string>
#include <grpcpp/grpcpp.h>
#include "atx/server/auth.hpp"
#include "atx/server/catalog.hpp"
#include "atx/server/config.hpp"
#include "atx/server/surface_registry.hpp"

namespace atx::server {

struct StatsCounters {
  std::atomic<std::uint64_t> total{0}, failed{0};
};
struct ServerDeps {
  Catalog &catalog;
  SurfaceRegistry &registry;
  const ServerConfig &config;
  StatsCounters &stats;
};

// Pull x-atx-token from metadata and resolve entitlements.
[[nodiscard]] inline core::Result<Entitlements> authenticate(const grpc::ServerContext *ctx,
                                                             Catalog &cat) {
  const auto &md = ctx->client_metadata();
  const auto it = md.find(std::string{kTokenMetadataKey});
  if (it == md.end()) {
    return core::Err(core::ErrorCode::PermissionDenied, "missing x-atx-token metadata");
  }
  return authorize_token(std::string_view{it->second.data(), it->second.size()}, cat);
}

[[nodiscard]] inline grpc::Status unauthenticated(const core::Error &e) {
  return {grpc::StatusCode::UNAUTHENTICATED, e.message()};
}

} // namespace atx::server
```

- [ ] **Step 1: Write the failing harness + admin tests**

```cpp
// atx-server/tests/service_test.cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

#include <grpcpp/grpcpp.h>

#include "atx/rpc/v1/admin.grpc.pb.h"
#include "atx/rpc/v1/surface_service.grpc.pb.h"
#include "atx/server/catalog.hpp"
#include "atx/server/catalog_index.hpp"
#include "atx/server/service_admin.hpp"
#include "atx/server/service_common.hpp"
#include "atx/server/surface_registry.hpp"
#include "test_support.hpp"

namespace atx::server {
namespace {
namespace fs = std::filesystem;

constexpr const char *kGoodToken = "test-token-1";
constexpr const char *kNoGrantToken = "test-token-2";

// One in-process server + fixture db shared by all service tests. No port.
class ServiceFixture : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / "atx_srv_svc";
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    fixture_ = testing::make_fixture_db(dir_ / "db", 3);

    auto cat = Catalog::open((dir_ / "state.db").string());
    ASSERT_TRUE(cat.has_value());
    cat_ = std::make_unique<Catalog>(std::move(*cat));
    ASSERT_TRUE(cat_->register_database("fix", "surface_db", fixture_.root).has_value());
    ASSERT_TRUE(cat_->add_token("good", token_digest(kGoodToken).value()).has_value());
    ASSERT_TRUE(cat_->grant(token_digest(kGoodToken).value(), "fix").has_value());
    ASSERT_TRUE(cat_->add_token("nogrant", token_digest(kNoGrantToken).value()).has_value());

    reg_ = std::make_unique<SurfaceRegistry>(*cat_, 16);
    ASSERT_TRUE(bootstrap_catalog(*cat_, *reg_).has_value());

    deps_ = std::make_unique<ServerDeps>(
        ServerDeps{*cat_, *reg_, config_, stats_});
    admin_ = std::make_unique<AdminServiceImpl>(*deps_, /*start_ns=*/123);
    grpc::ServerBuilder builder;
    builder.RegisterService(admin_.get());
    register_extra_services(builder); // Task 12 overrides; base = no-op
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    channel_ = server_->InProcessChannel(grpc::ChannelArguments{});
  }
  void TearDown() override {
    if (server_) server_->Shutdown();
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  virtual void register_extra_services(grpc::ServerBuilder &) {}

  static void add_token(grpc::ClientContext &ctx, const char *token) {
    ctx.AddMetadata(std::string{kTokenMetadataKey}, token);
  }

  fs::path dir_;
  testing::FixtureDb fixture_;
  ServerConfig config_;
  StatsCounters stats_;
  std::unique_ptr<Catalog> cat_;
  std::unique_ptr<SurfaceRegistry> reg_;
  std::unique_ptr<ServerDeps> deps_;
  std::unique_ptr<AdminServiceImpl> admin_;
  std::unique_ptr<grpc::Server> server_;
  std::shared_ptr<grpc::Channel> channel_;
};

TEST_F(ServiceFixture, HealthRequiresToken) {
  auto stub = rpc::v1::AdminService::NewStub(channel_);
  rpc::v1::HealthResponse resp;
  {
    grpc::ClientContext ctx; // no token
    rpc::v1::HealthRequest req;
    EXPECT_EQ(stub->Health(&ctx, req, &resp).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
  }
  {
    grpc::ClientContext ctx;
    add_token(ctx, kGoodToken);
    rpc::v1::HealthRequest req;
    ASSERT_TRUE(stub->Health(&ctx, req, &resp).ok());
    EXPECT_TRUE(resp.serving());
  }
}

TEST_F(ServiceFixture, ServerInfoAndStats) {
  auto stub = rpc::v1::AdminService::NewStub(channel_);
  grpc::ClientContext ctx;
  add_token(ctx, kGoodToken);
  rpc::v1::ServerInfo info;
  ASSERT_TRUE(stub->GetServerInfo(&ctx, rpc::v1::GetServerInfoRequest{}, &info).ok());
  EXPECT_EQ(info.version(), "0.1.0");
  EXPECT_FALSE(info.server_uuid().empty());
  EXPECT_EQ(info.database_count(), 1u);

  grpc::ClientContext ctx2;
  add_token(ctx2, kGoodToken);
  rpc::v1::ServerStats stats;
  ASSERT_TRUE(stub->GetStats(&ctx2, rpc::v1::GetStatsRequest{}, &stats).ok());
  EXPECT_GE(stats.requests_total(), 1u);
}

TEST_F(ServiceFixture, ExportRealmRoundTripsRegisterDatabase) {
  auto stub = rpc::v1::AdminService::NewStub(channel_);
  grpc::ClientContext ctx;
  add_token(ctx, kGoodToken);
  rpc::v1::RealmConfig realm;
  ASSERT_TRUE(stub->ExportRealm(&ctx, rpc::v1::ExportRealmRequest{}, &realm).ok());
  ASSERT_EQ(realm.databases_size(), 1);
  EXPECT_EQ(realm.databases(0).db_id(), "fix");
  EXPECT_EQ(realm.databases(0).root(), fixture_.root);
}

} // namespace
} // namespace atx::server
```

- [ ] **Step 2: Run to verify failure.**

- [ ] **Step 3: Implement service_admin + service_common + server**

`AdminServiceImpl` (service_admin.hpp declares the class + the five overrides; .cpp implements). Representative bodies:

```cpp
grpc::Status AdminServiceImpl::GetServerInfo(grpc::ServerContext *ctx,
                                             const rpc::v1::GetServerInfoRequest *,
                                             rpc::v1::ServerInfo *resp) {
  deps_.stats.total.fetch_add(1, std::memory_order_relaxed);
  auto ent = authenticate(ctx, deps_.catalog);
  if (!ent.has_value()) return unauthenticated(ent.error());
  return run_handler("GetServerInfo", [&]() -> core::Status {
    resp->set_version("0.1.0");
    resp->set_server_uuid(server_uuid()); // lazily created+persisted in state.kv
    resp->set_start_ns(start_ns_);
    resp->set_uptime_ns(now_ns() - start_ns_);
    ATX_TRY(auto realm, deps_.catalog.realm());
    resp->set_database_count(static_cast<std::uint32_t>(realm.size()));
    return core::Ok();
  });
}
```

`server_uuid()`: `SELECT value FROM state.kv WHERE key='server_uuid'`; if absent, generate 16 random bytes (`std::random_device`) hex-encoded, INSERT, return. `RegisterDatabase`: `register_database` + `registry.get` + `index_surface_db` + respond `indexed=true, generation`. `ExportRealm`: loop `realm()` rows into the proto. `GetStats`: counters + `registry.cache_stats()` loop. Failed-request accounting: bump `deps_.stats.failed` before returning any non-OK status (wrap in a tiny local lambda `auto fail = [&](grpc::Status s){ deps_.stats.failed.fetch_add(1,...); return s; }`).

`Server::build` (server.cpp):

```cpp
core::Result<Server> Server::build(const ServerConfig &cfg, Catalog &cat, SurfaceRegistry &reg,
                                   StatsCounters &stats) {
  ATX_TRY_VOID(validate_listen(cfg));
  Server out;
  out.deps_ = std::make_unique<ServerDeps>(ServerDeps{cat, reg, cfg, stats});
  out.admin_ = std::make_unique<AdminServiceImpl>(*out.deps_, now_ns());
  out.surface_ = std::make_unique<SurfaceServiceImpl>(*out.deps_); // Task 12
  std::shared_ptr<grpc::ServerCredentials> creds;
  if (!cfg.tls_cert_path.empty()) {
    ATX_TRY(auto cert, read_file(cfg.tls_cert_path)); // small local helper
    ATX_TRY(auto key, read_file(cfg.tls_key_path));
    grpc::SslServerCredentialsOptions ssl;
    ssl.pem_key_cert_pairs.push_back({key, cert});
    creds = grpc::SslServerCredentials(ssl);
  } else {
    creds = grpc::InsecureServerCredentials();
  }
  grpc::ServerBuilder b;
  b.SetMaxSendMessageSize(64 << 20);
  b.AddListeningPort(cfg.listen, creds, &out.port_);
  b.RegisterService(out.admin_.get());
  b.RegisterService(out.surface_.get());
  out.server_ = b.BuildAndStart();
  if (out.server_ == nullptr) {
    return Err(ErrorCode::Unavailable, "grpc server failed to start on " + cfg.listen);
  }
  return out;
}
```

Sequencing note: `Server::build` references `SurfaceServiceImpl`, which arrives in Task 12. For THIS task compile `server.cpp` with the surface registration lines commented out and a `// Task 12` marker, OR defer adding `src/server.cpp` to the build until Task 12 — pick deferral (cleaner): write `server.hpp/.cpp` now, add to CMake in Task 12. The in-process tests here register `AdminServiceImpl` directly and do not need `Server`.

- [ ] **Step 4: Run tests to verify they pass.**

- [ ] **Step 5: Commit**

```bash
git add atx-server/
git commit -m "feat(server): AdminService with token-gated health, stats, realm export, and db registration"
```

---

### Task 12: SurfaceService — catalog-backed RPCs + entitlement enforcement

**Files:**
- Create: `atx-server/include/atx/server/service_surface.hpp`
- Create: `atx-server/src/service_surface.cpp`
- Modify: `atx-server/src/server.cpp` into the build (Task 11's deferral)
- Modify: `atx-server/tests/service_test.cpp` (append surface tests)
- Modify: `atx-server/CMakeLists.txt`

**Interfaces:**
- Consumes: everything through Task 11.
- Produces: `class SurfaceServiceImpl final : public rpc::v1::SurfaceService::Service { public: explicit SurfaceServiceImpl(ServerDeps deps); };` implementing `ListDatabases`, `ListSymbols`, `ListPartitions`, `ListSurfaces`, `GetSymbolConfig` in this task (data RPCs in Task 13 return `UNIMPLEMENTED` until then via the base class default).
- Every handler: bump `stats.total` → `authenticate` → entitlement check on the request's `db_id` (`PERMISSION_DENIED` if unentitled — including for db_ids that do not exist; entitlement is checked BEFORE existence so unentitled probing cannot enumerate, spec §8 note still holds: ids are not secret but unentitled access is uniformly denied) → `run_handler` body.
- Pagination helper (service_surface.cpp, file-local): `limit = req.page().limit() == 0 ? 500 : min(req.page().limit(), 5000)`; SQL `WHERE <key> > ?after ORDER BY <key> LIMIT ?limit+1`; if `limit+1` rows come back, drop the last and set `page_info.truncated=true, next_after=<last kept key>`.
- `ResponseMeta` fill helper: `fill_meta(Catalog&, db_id, ResponseMeta*)` — `db_generation` from `indexed_generation` (0 if NotFound), `content_hash=0`, `server_ns=now`.

- [ ] **Step 1: Append the failing tests to service_test.cpp**

Change `ServiceFixture::register_extra_services` usage: construct `surface_ = std::make_unique<SurfaceServiceImpl>(*deps_);` in `SetUp` and register it alongside admin (drop the virtual hook — register both unconditionally now).

```cpp
TEST_F(ServiceFixture, ListDatabasesFiltersByEntitlement) {
  auto stub = rpc::v1::SurfaceService::NewStub(channel_);
  {
    grpc::ClientContext ctx;
    add_token(ctx, kGoodToken);
    rpc::v1::ListDatabasesResponse resp;
    ASSERT_TRUE(stub->ListDatabases(&ctx, rpc::v1::ListDatabasesRequest{}, &resp).ok());
    ASSERT_EQ(resp.databases_size(), 1);
    const auto &db = resp.databases(0);
    EXPECT_EQ(db.db_id(), "fix");
    EXPECT_EQ(db.kind(), "surface_db");
    EXPECT_EQ(db.symbol_count(), 1u);
    EXPECT_EQ(db.partition_count(), 3u);
    EXPECT_EQ(db.surface_count(), 6u);
    EXPECT_GT(db.generation(), 0u);
  }
  {
    grpc::ClientContext ctx;
    add_token(ctx, kNoGrantToken); // valid token, zero grants
    rpc::v1::ListDatabasesResponse resp;
    ASSERT_TRUE(stub->ListDatabases(&ctx, rpc::v1::ListDatabasesRequest{}, &resp).ok());
    EXPECT_EQ(resp.databases_size(), 0);
  }
}

TEST_F(ServiceFixture, UnentitledDbIsPermissionDenied) {
  auto stub = rpc::v1::SurfaceService::NewStub(channel_);
  grpc::ClientContext ctx;
  add_token(ctx, kNoGrantToken);
  rpc::v1::ListSymbolsRequest req;
  req.set_db_id("fix");
  rpc::v1::ListSymbolsResponse resp;
  EXPECT_EQ(stub->ListSymbols(&ctx, req, &resp).error_code(),
            grpc::StatusCode::PERMISSION_DENIED);
}

TEST_F(ServiceFixture, ListSymbolsAndFtsQuery) {
  auto stub = rpc::v1::SurfaceService::NewStub(channel_);
  {
    grpc::ClientContext ctx;
    add_token(ctx, kGoodToken);
    rpc::v1::ListSymbolsRequest req;
    req.set_db_id("fix");
    rpc::v1::ListSymbolsResponse resp;
    ASSERT_TRUE(stub->ListSymbols(&ctx, req, &resp).ok());
    ASSERT_EQ(resp.symbols_size(), 1); // only SPY has a stored config
    EXPECT_EQ(resp.symbols(0).symbol(), "SPY");
    EXPECT_TRUE(resp.symbols(0).enabled());
  }
  {
    grpc::ClientContext ctx;
    add_token(ctx, kGoodToken);
    rpc::v1::ListSymbolsRequest req;
    req.set_db_id("fix");
    req.set_query("AAPL"); // configured symbols only; AAPL has none
    rpc::v1::ListSymbolsResponse resp;
    ASSERT_TRUE(stub->ListSymbols(&ctx, req, &resp).ok());
    EXPECT_EQ(resp.symbols_size(), 0);
  }
}

TEST_F(ServiceFixture, ListPartitionsPaged) {
  auto stub = rpc::v1::SurfaceService::NewStub(channel_);
  grpc::ClientContext ctx;
  add_token(ctx, kGoodToken);
  rpc::v1::ListPartitionsRequest req;
  req.set_db_id("fix");
  req.mutable_page()->set_limit(2);
  rpc::v1::ListPartitionsResponse resp;
  ASSERT_TRUE(stub->ListPartitions(&ctx, req, &resp).ok());
  ASSERT_EQ(resp.partitions_size(), 2);
  EXPECT_TRUE(resp.page().truncated());
  ASSERT_FALSE(resp.page().next_after().empty());

  grpc::ClientContext ctx2;
  add_token(ctx2, kGoodToken);
  req.mutable_page()->set_after(resp.page().next_after());
  rpc::v1::ListPartitionsResponse resp2;
  ASSERT_TRUE(stub->ListPartitions(&ctx2, req, &resp2).ok());
  ASSERT_EQ(resp2.partitions_size(), 1);
  EXPECT_FALSE(resp2.page().truncated());
  EXPECT_NE(resp.partitions(0).key(), resp2.partitions(0).key());
}

TEST_F(ServiceFixture, ListSurfacesCoverageQuery) {
  auto stub = rpc::v1::SurfaceService::NewStub(channel_);
  grpc::ClientContext ctx;
  add_token(ctx, kGoodToken);
  rpc::v1::ListSurfacesRequest req;
  req.set_db_id("fix");
  req.set_symbol("SPY");
  req.set_key_from("2026-07-21"); // excludes day 0 (2026-07-20)
  rpc::v1::ListSurfacesResponse resp;
  ASSERT_TRUE(stub->ListSurfaces(&ctx, req, &resp).ok());
  ASSERT_EQ(resp.surfaces_size(), 2);
  for (const auto &s : resp.surfaces()) {
    EXPECT_EQ(s.symbol(), "SPY");
    EXPECT_EQ(s.expiry_count(), 3u);
    EXPECT_DOUBLE_EQ(s.spot(), 100.0);
    EXPECT_GE(s.partition_key(), "2026-07-21");
  }
}

TEST_F(ServiceFixture, GetSymbolConfigDecodedPlusBlob) {
  auto stub = rpc::v1::SurfaceService::NewStub(channel_);
  grpc::ClientContext ctx;
  add_token(ctx, kGoodToken);
  rpc::v1::GetSymbolConfigRequest req;
  req.set_db_id("fix");
  req.set_symbol("SPY");
  rpc::v1::GetSymbolConfigResponse resp;
  ASSERT_TRUE(stub->GetSymbolConfig(&ctx, req, &resp).ok());
  EXPECT_TRUE(resp.config().enabled());
  EXPECT_EQ(resp.config().record_blob().size(), 256u); // sizeof(DbSymbolRecord)
  grpc::ClientContext ctx2;
  add_token(ctx2, kGoodToken);
  req.set_symbol("MSFT");
  EXPECT_EQ(stub->GetSymbolConfig(&ctx2, req, &resp).error_code(),
            grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 2: Run to verify failure.**

- [ ] **Step 3: Implement service_surface.cpp (catalog half)**

All five handlers are catalog SQL under `cat.mu()` — no archive IO. Representative handler showing the full pattern (entitlement, pagination, meta):

```cpp
grpc::Status SurfaceServiceImpl::ListPartitions(grpc::ServerContext *ctx,
                                                const rpc::v1::ListPartitionsRequest *req,
                                                rpc::v1::ListPartitionsResponse *resp) {
  deps_.stats.total.fetch_add(1, std::memory_order_relaxed);
  auto ent = authenticate(ctx, deps_.catalog);
  if (!ent.has_value()) return fail(unauthenticated(ent.error()));
  if (!ent->entitled(req->db_id())) {
    return fail({grpc::StatusCode::PERMISSION_DENIED, "not entitled to " + req->db_id()});
  }
  return count_fail(run_handler("ListPartitions", [&]() -> core::Status {
    const auto [limit, after] = page_of(req->page());
    std::scoped_lock lk(deps_.catalog.mu());
    ATX_TRY(auto stmt, deps_.catalog.db().prepare_cached(
        "SELECT key, surface_count, file_size, created_ns FROM partition "
        "WHERE db_id = ?1 AND key > ?2 ORDER BY key LIMIT ?3"));
    ATX_TRY_VOID(stmt->bind(1, req->db_id()));
    ATX_TRY_VOID(stmt->bind(2, after));
    ATX_TRY_VOID(stmt->bind(3, static_cast<core::i64>(limit) + 1));
    while (true) {
      ATX_TRY(auto step, stmt->step());
      if (step == core::db::Statement::Step::Done) break;
      if (resp->partitions_size() == static_cast<int>(limit)) {
        resp->mutable_page()->set_truncated(true);
        resp->mutable_page()->set_next_after(
            resp->partitions(resp->partitions_size() - 1).key());
        break;
      }
      auto *p = resp->add_partitions();
      p->set_key(std::string(stmt->column_text(0)));
      p->set_surface_count(static_cast<std::uint32_t>(stmt->column_int(1)));
      p->set_file_size(static_cast<std::uint64_t>(stmt->column_int(2)));
      p->set_created_ns(stmt->column_int(3));
    }
    fill_meta_locked(req->db_id(), resp->mutable_meta());
    return core::Ok();
  }));
}
```

File-local helpers in service_surface.cpp:
- `std::pair<std::uint32_t, std::string> page_of(const rpc::v1::Page &p)` — the clamp described in Interfaces.
- `grpc::Status fail(grpc::Status s)` / `grpc::Status count_fail(grpc::Status s)` — bump `stats.failed` when `!s.ok()`, pass through.
- `void fill_meta_locked(std::string_view db_id, rpc::v1::ResponseMeta *m)` — assumes `cat.mu()` held: read `db_source.generation` (0 if absent), `content_hash=0`, `server_ns=now`.

Remaining handlers:
- `ListDatabases`: loop `ent->db_ids`; for each, one `SELECT generation FROM db_source WHERE db_id=?` + three `SELECT COUNT(*)` (symbol/partition/surface). Skip ids with no `db_source` row (registered but never indexed => generation 0, counts 0 — still listed).
- `ListSymbols`: no query => `SELECT symbol, enabled, preset FROM symbol WHERE db_id=?1 AND symbol > ?2 ORDER BY symbol LIMIT ?3`; with query => `SELECT s.symbol, s.enabled, s.preset FROM symbol_fts f JOIN symbol s ON s.rowid = f.rowid WHERE symbol_fts MATCH ?q AND s.db_id = ?1 AND s.symbol > ?2 ORDER BY s.symbol LIMIT ?3`. An FTS syntax error from a malformed query maps to `InvalidArgument` (wrap the prepare/step error).
- `ListSurfaces`: `WHERE db_id=?1 [AND symbol=?2] [AND part_key>=?from] [AND part_key<=?to] AND (part_key || '|' || symbol) > ?after ORDER BY part_key, symbol LIMIT ?n+1` — composite keyset cursor `part_key + "|" + symbol` (symbols are upper-case alnum; `'|'` cannot collide).
- `GetSymbolConfig`: `SELECT config_blob FROM symbol WHERE db_id=?1 AND symbol=?2` (canonicalize the request symbol to upper first — match `SurfaceDb`'s canonicalization); `Done` => `Err(NotFound)`. Decode via `std::memcpy` into `vol::DbSymbolRecord` (validate blob size == `sizeof(DbSymbolRecord)`, else `ParseError`), then `vol::decode_symbol_record` + `vol::decode_symbol_provenance` → `encode_symbol_config` / `encode_provenance`, `has_provenance` accordingly.

Also this task: add `src/server.cpp` + `src/service_surface.cpp` + `src/service_admin.cpp` to `atx-server-lib` sources (server.cpp now compiles — both services exist).

- [ ] **Step 4: Run tests to verify they pass.**

- [ ] **Step 5: Commit**

```bash
git add atx-server/
git commit -m "feat(server): SurfaceService catalog RPCs with entitlement gates, keyset paging, and FTS symbol search"
```

---

### Task 13: SurfaceService — data RPCs (meta, curve, blob) + blob fidelity

**Files:**
- Modify: `atx-server/include/atx/server/service_surface.hpp`, `atx-server/src/service_surface.cpp`
- Modify: `atx-server/tests/service_test.cpp` (meta/curve tests)
- Create: `atx-server/tests/blob_fidelity_test.cpp`
- Modify: `atx-server/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `SurfaceDb::{load_surface, open_partition, root}`, `SurfaceArchiveV2::{open_borrowed, find, header, provenance}`, `vol::PricedSurfaceView::create_over_record`, encoders (Task 9), `vol::kSurfaceDbPartitionDir`/`kSurfaceDbPartitionExt` constants.
- Produces: `GetSurfaceMeta`, `GetCurve`, `GetSurfaceBlob` handlers. Pattern: entitle → resolve OUTSIDE the catalog lock (all archive IO lock-free; catalog touched only for `fill_meta`).
- `GetSurfaceMeta`/`GetCurve`: `sdb = registry.get(db_id)` → `sdb->load_surface(partition_key, symbol)` (owned `PricedSurface` — exposes `context()`, needed by the encoders; the zero-copy view does not) → archive provenance via `sdb->open_partition(key)` + `archive.provenance(symbol)` → encode → set `key` + `meta`. `content_hash` = the record's directory `payload_crc32c` (widened to fixed64).
- `GetSurfaceBlob`: read the partition FILE bytes (path = `root/partitions/<key>.atxvsa` composed from the public constants; the key from the request is used verbatim after upper-casing — reject keys containing `/`, `\`, or `..` with `InvalidArgument` BEFORE path composition), `SurfaceArchiveV2::open_borrowed(span_over_local_buffer, nullptr_owner)` validates framing, `find(symbol)` → record span `[surface_offset, +surface_size)`. Enforce `max_blob_bytes` (`RESOURCE_EXHAUSTED`, message carries actual size). Schema-hash gate: nonzero `client_atxvsa_schema_hash != header().schema_hash` => `FAILED_PRECONDITION` with both hashes in the message.

- [ ] **Step 1: Append meta/curve tests to service_test.cpp**

```cpp
TEST_F(ServiceFixture, GetSurfaceMetaEndToEnd) {
  auto stub = rpc::v1::SurfaceService::NewStub(channel_);
  grpc::ClientContext ctx;
  add_token(ctx, kGoodToken);
  rpc::v1::GetSurfaceMetaRequest req;
  req.mutable_key()->set_db_id("fix");
  req.mutable_key()->set_partition_key(fixture_.keys[0]);
  req.mutable_key()->set_symbol("SPY");
  rpc::v1::SurfaceMeta meta;
  ASSERT_TRUE(stub->GetSurfaceMeta(&ctx, req, &meta).ok());
  EXPECT_EQ(meta.key().symbol(), "SPY");
  EXPECT_EQ(meta.n_slices(), 3u);
  EXPECT_EQ(meta.expiries_size(), 3);
  EXPECT_DOUBLE_EQ(meta.spot(), 100.0);
  EXPECT_GT(meta.meta().db_generation(), 0u);
}

TEST_F(ServiceFixture, GetCurveMatchesLocalSurface) {
  auto stub = rpc::v1::SurfaceService::NewStub(channel_);
  grpc::ClientContext ctx;
  add_token(ctx, kGoodToken);
  rpc::v1::GetCurveRequest req;
  req.mutable_key()->set_db_id("fix");
  req.mutable_key()->set_partition_key(fixture_.keys[1]);
  req.mutable_key()->set_symbol("AAPL");
  req.set_expiry_index(2);
  req.set_n_points(21);
  rpc::v1::VolCurveSlice slice;
  ASSERT_TRUE(stub->GetCurve(&ctx, req, &slice).ok());
  ASSERT_GT(slice.points_size(), 0);
  // ground truth: same query straight off the SurfaceDb
  auto sdb = vol::SurfaceDb::open(fixture_.root);
  ASSERT_TRUE(sdb.has_value());
  auto local = sdb->load_surface(fixture_.keys[1], "AAPL");
  ASSERT_TRUE(local.has_value());
  for (const auto &p : slice.points()) {
    EXPECT_DOUBLE_EQ(p.iv(), local->iv(p.strike(), slice.years()));
  }
}

TEST_F(ServiceFixture, GetSurfaceMetaUnknownSymbolIsNotFound) {
  auto stub = rpc::v1::SurfaceService::NewStub(channel_);
  grpc::ClientContext ctx;
  add_token(ctx, kGoodToken);
  rpc::v1::GetSurfaceMetaRequest req;
  req.mutable_key()->set_db_id("fix");
  req.mutable_key()->set_partition_key(fixture_.keys[0]);
  req.mutable_key()->set_symbol("MSFT");
  rpc::v1::SurfaceMeta meta;
  EXPECT_EQ(stub->GetSurfaceMeta(&ctx, req, &meta).error_code(),
            grpc::StatusCode::NOT_FOUND);
}
```

- [ ] **Step 2: Write the failing blob fidelity test (the spec §11 bit-identical gate)**

```cpp
// atx-server/tests/blob_fidelity_test.cpp
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <new>

#include <grpcpp/grpcpp.h>

#include "atx/rpc/v1/surface_service.grpc.pb.h"
#include "atx/server/auth.hpp"
#include "atx/server/catalog.hpp"
#include "atx/server/catalog_index.hpp"
#include "atx/server/service_admin.hpp"
#include "atx/server/service_common.hpp"
#include "atx/server/service_surface.hpp"
#include "atx/vol/priced_surface_view.hpp"
#include "atx/vol/surface_db.hpp"
#include "test_support.hpp"

namespace atx::server {
namespace {
namespace fs = std::filesystem;

TEST(BlobFidelity, RemoteBlobPricesBitIdenticalToLocal) {
  const fs::path dir = fs::temp_directory_path() / "atx_srv_blob";
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto fixture = testing::make_fixture_db(dir / "db", 1);

  auto cat = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(cat.has_value());
  ASSERT_TRUE(cat->register_database("fix", "surface_db", fixture.root).has_value());
  const char *token = "blob-token";
  ASSERT_TRUE(cat->add_token("t", token_digest(token).value()).has_value());
  ASSERT_TRUE(cat->grant(token_digest(token).value(), "fix").has_value());
  SurfaceRegistry reg(*cat, 16);
  ASSERT_TRUE(bootstrap_catalog(*cat, reg).has_value());

  ServerConfig config;
  StatsCounters stats;
  ServerDeps deps{*cat, reg, config, stats};
  SurfaceServiceImpl surface{deps};
  grpc::ServerBuilder b;
  b.RegisterService(&surface);
  auto server = b.BuildAndStart();
  ASSERT_NE(server, nullptr);
  auto stub = rpc::v1::SurfaceService::NewStub(server->InProcessChannel({}));

  grpc::ClientContext ctx;
  ctx.AddMetadata(std::string{kTokenMetadataKey}, token);
  rpc::v1::GetSurfaceBlobRequest req;
  req.mutable_key()->set_db_id("fix");
  req.mutable_key()->set_partition_key(fixture.keys[0]);
  req.mutable_key()->set_symbol("SPY");
  rpc::v1::SurfaceBlob blob;
  ASSERT_TRUE(stub->GetSurfaceBlob(&ctx, req, &blob).ok());
  ASSERT_GT(blob.record().size(), 0u);
  EXPECT_NE(blob.atxvsa_schema_hash(), 0u);

  // Client-side reconstruction: 64-byte-aligned copy (proto comment contract).
  const std::size_t n = blob.record().size();
  auto aligned = std::unique_ptr<std::byte[], void (*)(std::byte *)>(
      static_cast<std::byte *>(::operator new[](n, std::align_val_t{64})),
      [](std::byte *p) { ::operator delete[](p, std::align_val_t{64}); });
  std::memcpy(aligned.get(), blob.record().data(), n);
  auto view = vol::PricedSurfaceView::create_over_record({aligned.get(), n});
  ASSERT_TRUE(view.has_value()) << view.error().to_string();

  // Ground truth: the same surface straight off the SurfaceDb.
  auto sdb = vol::SurfaceDb::open(fixture.root);
  ASSERT_TRUE(sdb.has_value());
  auto local = sdb->map_surface(fixture.keys[0], "SPY");
  ASSERT_TRUE(local.has_value());

  const double T = 0.15; // inside the fixture's [0.05, 0.25] slice range
  for (double k : {80.0, 90.0, 100.0, 110.0, 125.0}) {
    const auto remote_fv = view->fair_value(k, T, vol::Side::Call);
    const auto local_fv = (*local)->fair_value(k, T, vol::Side::Call);
    ASSERT_TRUE(remote_fv.has_value() && local_fv.has_value()) << k;
    // bit-identical, not approximately equal (spec §11)
    EXPECT_EQ(*remote_fv, *local_fv) << k;
    EXPECT_EQ(view->iv(k, T), (*local)->iv(k, T)) << k;
  }
  server->Shutdown();
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(BlobFidelity, SchemaHashMismatchIsFailedPrecondition) {
  // Same setup as above trimmed to one call: send a wrong client hash.
  // (Refactor the setup above into a small local struct if duplication annoys —
  // but keep both tests independent and self-contained.)
  const fs::path dir = fs::temp_directory_path() / "atx_srv_blob2";
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto fixture = testing::make_fixture_db(dir / "db", 1);
  auto cat = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(cat.has_value());
  ASSERT_TRUE(cat->register_database("fix", "surface_db", fixture.root).has_value());
  const char *token = "blob-token2";
  ASSERT_TRUE(cat->add_token("t", token_digest(token).value()).has_value());
  ASSERT_TRUE(cat->grant(token_digest(token).value(), "fix").has_value());
  SurfaceRegistry reg(*cat, 16);
  ASSERT_TRUE(bootstrap_catalog(*cat, reg).has_value());
  ServerConfig config;
  StatsCounters stats;
  ServerDeps deps{*cat, reg, config, stats};
  SurfaceServiceImpl surface{deps};
  grpc::ServerBuilder b;
  b.RegisterService(&surface);
  auto server = b.BuildAndStart();
  auto stub = rpc::v1::SurfaceService::NewStub(server->InProcessChannel({}));
  grpc::ClientContext ctx;
  ctx.AddMetadata(std::string{kTokenMetadataKey}, token);
  rpc::v1::GetSurfaceBlobRequest req;
  req.mutable_key()->set_db_id("fix");
  req.mutable_key()->set_partition_key(fixture.keys[0]);
  req.mutable_key()->set_symbol("SPY");
  req.set_client_atxvsa_schema_hash(0xDEADBEEFULL);
  rpc::v1::SurfaceBlob blob;
  const auto st = stub->GetSurfaceBlob(&ctx, req, &blob);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(st.error_message().find("deadbeef"), std::string::npos);
  server->Shutdown();
  std::error_code ec;
  fs::remove_all(dir, ec);
}
```

- [ ] **Step 3: Run to verify failure** (`UNIMPLEMENTED` from the base class).

- [ ] **Step 4: Implement the three handlers**

`GetSurfaceMeta` / `GetCurve` core (after the standard entitle preamble):

```cpp
ATX_TRY(auto sdb, deps_.registry.get(req->key().db_id()));
ATX_TRY(auto surface, sdb->load_surface(req->key().partition_key(), req->key().symbol()));
ATX_TRY(auto archive, sdb->open_partition(req->key().partition_key()));
ATX_TRY(auto prov, archive.provenance(req->key().symbol()));
ATX_TRY_VOID(encode_surface_meta(surface, prov, resp));       // or encode_curve(...)
*resp->mutable_key() = req->key();
{
  std::scoped_lock lk(deps_.catalog.mu());
  fill_meta_locked(req->key().db_id(), resp->mutable_meta());
}
// content_hash: the record's directory CRC
ATX_TRY(auto entry, archive.find(req->key().symbol()));
resp->mutable_meta()->set_content_hash(entry.payload_crc32c);
```

`GetSurfaceBlob` core:

```cpp
const std::string &key = req->key().partition_key();
if (key.empty() || key.find_first_of("/\\") != std::string::npos ||
    key.find("..") != std::string::npos) {
  return Err(ErrorCode::InvalidArgument, "malformed partition key");
}
ATX_TRY(auto sdb, deps_.registry.get(req->key().db_id()));
std::string canonical = key; // SurfaceDb canonicalizes keys to lower... VERIFY:
// read surface_db.cpp's canonical_key helper and match it exactly (the test
// fixture uses keys like "2026-07-20", which are case-stable either way).
const std::string path = sdb->root() + "/" + std::string(vol::kSurfaceDbPartitionDir) +
                         "/" + canonical + std::string(vol::kSurfaceDbPartitionExt);
ATX_TRY(auto bytes, read_file_bytes(path)); // local helper: ifstream binary read;
                                            // NotFound if absent, IoError otherwise
ATX_TRY(auto archive, vol::SurfaceArchiveV2::open_borrowed(
    {bytes.data(), bytes.size()}, std::shared_ptr<const void>{}));
if (req->client_atxvsa_schema_hash() != 0 &&
    req->client_atxvsa_schema_hash() != archive.header().schema_hash) {
  char msg[96];
  std::snprintf(msg, sizeof(msg), "client atxvsa schema %016llx != server %016llx",
                static_cast<unsigned long long>(req->client_atxvsa_schema_hash()),
                static_cast<unsigned long long>(archive.header().schema_hash));
  return fail_precondition(msg); // helper returning a sentinel mapped below
}
ATX_TRY(auto entry, archive.find(req->key().symbol()));
if (entry.surface_size > deps_.config.max_blob_bytes) {
  return Err(ErrorCode::OutOfRange, /* mapped specially, see below */
             "blob is " + std::to_string(entry.surface_size) + " bytes, limit " +
                 std::to_string(deps_.config.max_blob_bytes));
}
resp->set_record(bytes.data() + entry.surface_offset,
                 static_cast<std::size_t>(entry.surface_size));
resp->set_atxvsa_schema_hash(archive.header().schema_hash);
resp->set_payload_crc32c(entry.payload_crc32c);
resp->set_uid(entry.uid);
resp->set_n_slices(entry.n_slices);
```

Status-code plumbing for the two non-table codes (spec §8: `RESOURCE_EXHAUSTED` for oversize, `FAILED_PRECONDITION` for schema mismatch): `run_handler` maps `core::ErrorCode`s only, and the table has no slots for these. Do NOT bend the table — return `grpc::Status` directly from the handler for these two cases, before/around the `run_handler` body, i.e. structure `GetSurfaceBlob` as a plain function returning `grpc::Status` that uses a local `core::Status`-returning lambda ONLY for the table-mapped portion, or simplest: implement `GetSurfaceBlob` without `run_handler`, wrapping the body in its own try/catch that reuses `internal_incident`. Keep the two special statuses:
`{grpc::StatusCode::RESOURCE_EXHAUSTED, "<size> bytes exceeds max_blob_bytes <limit>"}` and
`{grpc::StatusCode::FAILED_PRECONDITION, "<both hashes>"}` (hex, lowercase — the test greps "deadbeef").

Canonicalization VERIFY step: before coding, read `surface_db.cpp`'s key-canonicalization (grep `canonical`) and mirror it for the path composition; add a one-line comment citing the function you mirrored.

- [ ] **Step 5: Run all tests** — `ctest --test-dir build-server -L atx_server --output-on-failure`. The fidelity EXPECT_EQ on doubles is the gate: if it fails with tiny diffs, the server path and local path diverged in method (e.g. `load_surface` reconstruct vs `map_surface` view) — the BLOB path must compare view-vs-view as written; do not loosen to NEAR.

- [ ] **Step 6: Commit**

```bash
git add atx-server/
git commit -m "feat(server): surface meta, curve, and raw ATXVSA blob RPCs with schema-hash and size gates"
```

---

### Task 14: CatalogRefresher, warm start, concurrency test

**Files:**
- Create: `atx-server/include/atx/server/refresher.hpp`
- Create: `atx-server/src/refresher.cpp`
- Create: `atx-server/tests/lifecycle_test.cpp`
- Modify: `atx-server/CMakeLists.txt`, `atx-server/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `bootstrap_catalog`, `index_surface_db`, `indexed_generation`, `SurfaceDb::{generation, refresh}`, `Catalog::{snapshot_to, restore_main_from}`.
- Produces:
  - `class CatalogRefresher { public: CatalogRefresher(Catalog &cat, SurfaceRegistry &reg, int interval_ms); ~CatalogRefresher(); void start(); void stop(); };` — background thread; each tick: for every realm row, `reg.get(id)` → `sdb->refresh()` → if `sdb->generation() != indexed_generation(cat, id)` => `index_surface_db`. Stop is prompt (`condition_variable::wait_for`, notified in `stop()`); destructor calls `stop()`. Tick errors are logged and swallowed — a broken db must not kill the refresher loop.
  - Warm-start free function: `core::Status warm_start(Catalog &cat, SurfaceRegistry &reg, std::string_view snapshot_path);` — if snapshot exists: `restore_main_from` then `bootstrap_catalog` (drift-only reindex); else plain `bootstrap_catalog` (cold full scan). Lives in `catalog_index.hpp`.

- [ ] **Step 1: Write the failing tests**

```cpp
// atx-server/tests/lifecycle_test.cpp
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#include "atx/server/catalog.hpp"
#include "atx/server/catalog_index.hpp"
#include "atx/server/refresher.hpp"
#include "atx/server/surface_registry.hpp"
#include "test_support.hpp"

namespace atx::server {
namespace {
namespace fs = std::filesystem;

struct LifecycleFixture : ::testing::Test {
  fs::path dir = fs::temp_directory_path() / "atx_srv_life";
  void SetUp() override { fs::remove_all(dir); fs::create_directories(dir); }
  void TearDown() override { std::error_code ec; fs::remove_all(dir, ec); }
};

TEST_F(LifecycleFixture, RefresherPicksUpExternalWriter) {
  auto fixture = testing::make_fixture_db(dir / "db", 1);
  auto cat = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(cat.has_value());
  ASSERT_TRUE(cat->register_database("fix", "surface_db", fixture.root).has_value());
  SurfaceRegistry reg(*cat, 16);
  ASSERT_TRUE(bootstrap_catalog(*cat, reg).has_value());
  const auto gen0 = indexed_generation(*cat, "fix").value();

  CatalogRefresher refresher(*cat, reg, /*interval_ms=*/50);
  refresher.start();

  { // external writer bumps the manifest generation
    auto writer = vol::SurfaceDb::open(fixture.root);
    ASSERT_TRUE(writer.has_value());
    auto s = testing::make_essvi(500, 3);
    std::vector<vol::SurfaceArchiveItem> items{{"SPY", &s, vol::SurfaceProvenance{}}};
    ASSERT_TRUE(writer->write_partition("2026-08-01", items).has_value());
  }
  // wait (bounded) for the refresher to index the drift
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (indexed_generation(*cat, "fix").value() > gen0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  refresher.stop();
  EXPECT_GT(indexed_generation(*cat, "fix").value(), gen0);
}

TEST_F(LifecycleFixture, WarmStartEqualsColdScan) {
  auto fixture = testing::make_fixture_db(dir / "db", 2);
  const std::string snap = (dir / "catalog_snapshot.db").string();

  std::string cold_dump;
  { // process 1: cold scan, then snapshot on the way down
    auto cat = Catalog::open((dir / "state.db").string());
    ASSERT_TRUE(cat.has_value());
    ASSERT_TRUE(cat->register_database("fix", "surface_db", fixture.root).has_value());
    SurfaceRegistry reg(*cat, 16);
    ASSERT_TRUE(warm_start(*cat, reg, snap).has_value()); // no snapshot => cold
    cold_dump = testing::dump_main_no_ts(*cat);
    ASSERT_TRUE(cat->snapshot_to(snap).has_value());
  }
  { // process 2: warm start from the snapshot, no drift => no rescan needed
    auto cat = Catalog::open((dir / "state.db").string());
    ASSERT_TRUE(cat.has_value());
    SurfaceRegistry reg(*cat, 16);
    ASSERT_TRUE(warm_start(*cat, reg, snap).has_value());
    EXPECT_EQ(testing::dump_main_no_ts(*cat), cold_dump);
  }
  { // process 3: drift between snapshot and restart is caught
    auto writer = vol::SurfaceDb::open(fixture.root);
    ASSERT_TRUE(writer.has_value());
    auto s = testing::make_essvi(600, 3);
    std::vector<vol::SurfaceArchiveItem> items{{"AAPL", &s, vol::SurfaceProvenance{}}};
    ASSERT_TRUE(writer->write_partition("2026-08-02", items).has_value());

    auto cat = Catalog::open((dir / "state.db").string());
    ASSERT_TRUE(cat.has_value());
    SurfaceRegistry reg(*cat, 16);
    ASSERT_TRUE(warm_start(*cat, reg, snap).has_value());
    EXPECT_NE(testing::dump_main_no_ts(*cat), cold_dump); // new partition indexed
  }
}

TEST_F(LifecycleFixture, ConcurrentReadsSurviveReindex) {
  auto fixture = testing::make_fixture_db(dir / "db", 2);
  auto cat = Catalog::open((dir / "state.db").string());
  ASSERT_TRUE(cat.has_value());
  ASSERT_TRUE(cat->register_database("fix", "surface_db", fixture.root).has_value());
  SurfaceRegistry reg(*cat, 16);
  ASSERT_TRUE(bootstrap_catalog(*cat, reg).has_value());

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::atomic<std::uint64_t> last_gen{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        auto sdb = reg.get("fix");
        if (!sdb.has_value()) { ++failures; continue; }
        auto surf = (*sdb)->load_surface(fixture.keys[0], "SPY");
        if (!surf.has_value() || !std::isfinite(surf->iv(100.0, 0.15))) ++failures;
        auto gen = indexed_generation(*cat, "fix");
        if (gen.has_value()) {
          // generations only move forward under concurrent reindex
          auto prev = last_gen.load(std::memory_order_relaxed);
          if (*gen < prev) ++failures;
          while (prev < *gen &&
                 !last_gen.compare_exchange_weak(prev, *gen, std::memory_order_relaxed)) {
          }
        }
      }
    });
  }
  // writer thread: 5 partition writes + reindex each
  for (int i = 0; i < 5; ++i) {
    auto writer = vol::SurfaceDb::open(fixture.root);
    ASSERT_TRUE(writer.has_value());
    auto s = testing::make_essvi(700u + static_cast<std::uint32_t>(i), 3);
    std::vector<vol::SurfaceArchiveItem> items{{"SPY", &s, vol::SurfaceProvenance{}}};
    ASSERT_TRUE(writer->write_partition("2026-09-0" + std::to_string(i), items).has_value());
    ASSERT_TRUE((*reg.get("fix"))->refresh().has_value());
    ASSERT_TRUE(index_surface_db(*cat, "fix", **reg.get("fix")).has_value());
  }
  stop = true;
  for (auto &t : readers) t.join();
  EXPECT_EQ(failures.load(), 0);
}
```

Move the `dump_main` helper from `index_test.cpp` into `test_support.hpp` as `atx::server::testing::dump_main_no_ts(Catalog &)` (same body; delete the local copy in index_test.cpp and update its call sites).

- [ ] **Step 2: Run to verify failure** — `refresher.hpp` / `warm_start` missing.

- [ ] **Step 3: Implement**

```cpp
// refresher.hpp
#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

#include "atx/server/catalog.hpp"
#include "atx/server/surface_registry.hpp"

namespace atx::server {

// The cache/catchup analogue (spec §5.3): polls every realm db's manifest
// generation (one header read) and re-indexes only drifted dbs.
class CatalogRefresher {
public:
  CatalogRefresher(Catalog &cat, SurfaceRegistry &reg, int interval_ms)
      : cat_{cat}, reg_{reg}, interval_ms_{interval_ms} {}
  ~CatalogRefresher() { stop(); }
  void start();
  void stop();

private:
  void tick();
  Catalog &cat_;
  SurfaceRegistry &reg_;
  int interval_ms_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stopping_{false};
  std::thread thread_;
};

} // namespace atx::server
```

```cpp
// refresher.cpp
#include "atx/server/refresher.hpp"

#include <chrono>

#include "atx/server/catalog_index.hpp"

namespace atx::server {

void CatalogRefresher::start() {
  thread_ = std::thread([this] {
    std::unique_lock lk(mu_);
    while (!stopping_) {
      lk.unlock();
      tick();
      lk.lock();
      cv_.wait_for(lk, std::chrono::milliseconds(interval_ms_), [this] { return stopping_; });
    }
  });
}

void CatalogRefresher::stop() {
  {
    std::scoped_lock lk(mu_);
    if (stopping_) { if (thread_.joinable()) thread_.join(); return; }
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void CatalogRefresher::tick() {
  auto realm = cat_.realm();
  if (!realm.has_value()) return; // logged-and-swallowed tier: catalog unreadable
  for (const auto &row : *realm) {
    auto sdb = reg_.get(row.db_id);
    if (!sdb.has_value()) continue;
    if (auto st = (*sdb)->refresh(); !st.has_value()) continue;
    auto indexed = indexed_generation(cat_, row.db_id);
    if (indexed.has_value() && *indexed == (*sdb)->generation()) continue;
    (void)index_surface_db(cat_, row.db_id, **sdb); // errors: next tick retries
  }
}

} // namespace atx::server
```

`warm_start` (append to catalog_index.cpp/.hpp):

```cpp
Status warm_start(Catalog &cat, SurfaceRegistry &reg, std::string_view snapshot_path) {
  if (std::filesystem::exists(snapshot_path)) {
    if (auto st = cat.restore_main_from(snapshot_path); !st.has_value()) {
      // A corrupt snapshot must not block startup: fall through to cold scan.
      // (main may hold partial rows; bootstrap reindexes any db whose
      //  generation mismatches, and a partial restore mismatches.)
    }
  }
  return bootstrap_catalog(cat, reg);
}
```

Wait — a partial restore of an up-to-date generation row with missing surface rows would NOT mismatch. Harden: on `restore_main_from` failure, wipe `main` (`DELETE FROM surface; DELETE FROM partition; DELETE FROM symbol; DELETE FROM db_source;` under the lock) before `bootstrap_catalog`. Implement that wipe as `Catalog::clear_main()` and call it on the failure path. Write it exactly so.

- [ ] **Step 4: Run tests to verify they pass.** The concurrency test exercises the spec §11 row: readers never observe a torn catalog (SQLite transaction), generations monotone, `SurfaceDb` reads safe across `refresh()`.

- [ ] **Step 5: Commit**

```bash
git add atx-server/
git commit -m "feat(server): generation-drift refresher, snapshot warm start, and concurrent-read lifecycle tests"
```

---

### Task 15: Binaries — atx-server, atx-server-cli, mkfixture + process smoke

**Files:**
- Create: `atx-server/tools/main.cpp`
- Create: `atx-server/tools/cli.cpp`
- Create: `atx-server/tools/mkfixture.cpp`
- Create: `atx-server/tests/smoke.ps1`
- Modify: `atx-server/CMakeLists.txt` (three executables + smoke ctest entry)

**Interfaces:**
- Consumes: everything prior.
- Produces:
  - `atx-server` exe. Flags: `--state DIR` (default `atx-server-state`), `--listen ADDR`, `--tls-cert F --tls-key F`, `--realm-import FILE.json`, `--add-token LABEL`, `--grant LABEL DB_ID`, `--list-tokens`, `--max-blob-bytes N`, `--refresh-ms N`, `--help`. Startup order: parse → `Catalog::open(state/state.db)` → admin one-shots (`--add-token`/`--grant`/`--list-tokens` run then EXIT 0) → `--realm-import` (proto `RealmConfig` via `google::protobuf::util::JsonStringToMessage`, `register_database` per entry) → `SurfaceRegistry` → `warm_start(cat, reg, state/catalog_snapshot.db)` → `Server::build` (which enforces `validate_listen` → non-loopback-without-TLS exits nonzero with the Task 3 message) → start `CatalogRefresher` → print `ATX_SERVER_LISTENING port=<port> state=<dir>` → `wait()`. On Ctrl-C (`SetConsoleCtrlHandler` → `Server::shutdown()`): stop refresher, `snapshot_to(state/catalog_snapshot.db)`, exit 0.
  - `--add-token LABEL`: 32 bytes from `std::random_device`, hex-encode → the token; print `ATX_TOKEN <hex>` ONCE; store `token_digest(token)` via `add_token`. `--grant LABEL DB_ID`: `digest_by_label` + `grant`.
  - `atx-server-cli` exe. Global flags `--server ADDR` (default `127.0.0.1:50051`), `--token TOK`. Subcommands: `health`, `info`, `stats`, `dbs`, `symbols --db ID [--query Q]`, `partitions --db ID`, `surfaces --db ID [--symbol S] [--from K] [--to K]`, `config --db ID --symbol S`, `meta --db ID --key K --symbol S`, `curve --db ID --key K --symbol S --expiry N [--points N] [--greeks]`, `blob --db ID --key K --symbol S --out FILE`. Output: header line + tab-separated rows to stdout; non-OK RPC → print `RPC_ERROR <code> <message>` to stderr, exit 1. Every call sets a 10 s deadline (`ctx.set_deadline`).
  - `atx-server-mkfixture` exe (test-only; built when `ATX_BUILD_TESTS`): `mkfixture <dir> [n_days]` → builds the Task 7 fixture SurfaceDb at `<dir>`, prints the partition keys. Links `test_support.cpp` (add `tests/test_support.cpp` to its sources directly; keep gtest out by guarding `ADD_FAILURE` — replace with `fprintf(stderr,...)+abort()` via a tiny `ATX_FIXTURE_CHECK` macro so test_support compiles in both contexts: simplest is to have test_support take failures via `assert`-style macro defined per-target: `#ifdef ATX_FIXTURE_NO_GTEST ...`).
  - Smoke ctest: `atx-server-smoke` running `smoke.ps1` with `-ServerExe $<TARGET_FILE:atx-server> -CliExe $<TARGET_FILE:atx-server-cli> -MkfixtureExe $<TARGET_FILE:atx-server-mkfixture>`, label `atx_server`.

- [ ] **Step 1: Write smoke.ps1 first (it is the failing test)**

```powershell
# atx-server/tests/smoke.ps1 — end-to-end process smoke (spec §12 criteria 2-4).
param(
  [Parameter(Mandatory)][string]$ServerExe,
  [Parameter(Mandatory)][string]$CliExe,
  [Parameter(Mandatory)][string]$MkfixtureExe
)
$ErrorActionPreference = 'Stop'
$work = Join-Path $env:TEMP ("atx_smoke_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
$server = $null
try {
  & $MkfixtureExe (Join-Path $work 'db') 2 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "mkfixture failed" }
  $state = Join-Path $work 'state'

  # token admin one-shots
  $tokenLine = & $ServerExe --state $state --add-token smoke | Select-String 'ATX_TOKEN '
  if (-not $tokenLine) { throw 'no ATX_TOKEN line' }
  $token = ($tokenLine.Line -split ' ')[1]

  # realm import file
  $realm = @{ databases = @(@{ db_id = 'fix'; kind = 'surface_db'; root = (Join-Path $work 'db') }) } |
    ConvertTo-Json -Depth 4
  $realmFile = Join-Path $work 'realm.json'
  [System.IO.File]::WriteAllText($realmFile, $realm)

  # non-loopback without TLS must refuse (criterion 7)
  & $ServerExe --state $state --listen 0.0.0.0:50999 2>$null
  if ($LASTEXITCODE -eq 0) { throw 'non-loopback without TLS did not refuse' }

  # start the real server on an ephemeral loopback port
  $out = Join-Path $work 'server_out.txt'
  $server = Start-Process -FilePath $ServerExe -PassThru -RedirectStandardOutput $out `
    -ArgumentList @('--state', $state, '--listen', '127.0.0.1:0', '--realm-import', $realmFile)
  $port = $null
  foreach ($i in 1..100) {
    Start-Sleep -Milliseconds 100
    if (Test-Path $out) {
      $m = Select-String -Path $out -Pattern 'ATX_SERVER_LISTENING port=(\d+)' -ErrorAction SilentlyContinue
      if ($m) { $port = $m.Matches[0].Groups[1].Value; break }
    }
  }
  if (-not $port) { throw 'server never printed ATX_SERVER_LISTENING' }
  $addr = "127.0.0.1:$port"

  # grant the smoke token (one-shot against the SAME state dir requires the
  # server's state.db via WAL — safe: token admin opens its own connection)
  & $ServerExe --state $state --grant smoke fix | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'grant failed' }

  & $CliExe --server $addr --token $token dbs | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'cli dbs failed' }
  $surfaces = & $CliExe --server $addr --token $token surfaces --db fix --symbol SPY
  if ($LASTEXITCODE -ne 0 -or -not ($surfaces | Select-String 'SPY')) { throw 'cli surfaces failed' }
  $curve = & $CliExe --server $addr --token $token curve --db fix --key 2026-07-20 --symbol SPY --expiry 0
  if ($LASTEXITCODE -ne 0) { throw 'cli curve failed' }
  $blobFile = Join-Path $work 'spy.atxrec'
  & $CliExe --server $addr --token $token blob --db fix --key 2026-07-20 --symbol SPY --out $blobFile | Out-Null
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $blobFile) -or (Get-Item $blobFile).Length -eq 0) {
    throw 'cli blob failed'
  }
  Write-Output 'SMOKE_OK'
} finally {
  if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
```

Design note the script depends on: the grant one-shot runs while the server is up — both processes open `state.db` (WAL + busy_timeout, cross-process single-writer is fine). The server picks up the new entitlement on the next `authenticate` (reads state.token/entitlement live). No restart needed.

CMake:

```cmake
add_test(NAME atx-server-smoke
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass
          -File ${CMAKE_CURRENT_SOURCE_DIR}/smoke.ps1
          -ServerExe $<TARGET_FILE:atx-server>
          -CliExe $<TARGET_FILE:atx-server-cli>
          -MkfixtureExe $<TARGET_FILE:atx-server-mkfixture>)
set_tests_properties(atx-server-smoke PROPERTIES LABELS atx_server TIMEOUT 300)
```

- [ ] **Step 2: Run to verify failure** — the three exes don't exist.

- [ ] **Step 3: Implement main.cpp, cli.cpp, mkfixture.cpp**

`main.cpp` skeleton (arg loop mirrors `atx-ui/src/main.cpp:31-78`'s `next()` pattern):

```cpp
int main(int argc, char **argv) {
  atx::server::ServerConfig cfg;
  std::string realm_import, add_token_label, grant_label, grant_db;
  bool list_tokens = false;
  // ... parse flags per the Interfaces block; unknown flag => usage + exit 1 ...

  namespace fs = std::filesystem;
  fs::create_directories(cfg.state_dir);
  auto cat = atx::server::Catalog::open(
      (fs::path(cfg.state_dir) / "state.db").string());
  if (!cat.has_value()) { std::cerr << cat.error().to_string() << '\n'; return 1; }

  if (!add_token_label.empty()) { /* random 32B -> hex, print ATX_TOKEN, add_token, return 0 */ }
  if (!grant_label.empty())     { /* digest_by_label + grant, return 0/1 */ }
  if (list_tokens)              { /* SELECT label, disabled FROM state.token, return 0 */ }

  if (!realm_import.empty()) {
    // read file, JsonStringToMessage into rpc::v1::RealmConfig, register each
  }
  atx::server::SurfaceRegistry reg(*cat, cfg.partition_cache_capacity);
  const std::string snap = (fs::path(cfg.state_dir) / "catalog_snapshot.db").string();
  if (auto st = atx::server::warm_start(*cat, reg, snap); !st.has_value()) {
    std::cerr << st.error().to_string() << '\n';
    return 1;
  }
  atx::server::StatsCounters stats;
  auto server = atx::server::Server::build(cfg, *cat, reg, stats);
  if (!server.has_value()) { std::cerr << server.error().to_string() << '\n'; return 1; }
  atx::server::CatalogRefresher refresher(*cat, reg, cfg.refresh_interval_ms);
  refresher.start();
  std::cout << "ATX_SERVER_LISTENING port=" << server->port()
            << " state=" << cfg.state_dir << std::endl; // endl: flush matters (smoke greps it)
  install_ctrl_handler([&] { server->shutdown(); });
  server->wait();
  refresher.stop();
  (void)cat->snapshot_to(snap);
  return 0;
}
```

`install_ctrl_handler`: `SetConsoleCtrlHandler` storing a `std::function` in a file-static; returns TRUE from the handler after invoking it.

`cli.cpp`: one function per subcommand taking the stub(s); straight request-fill / print loops. `blob` writes `resp.record()` bytes with `std::ofstream(path, std::ios::binary)`, prints `schema_hash=<hex> crc=<hex> bytes=<n>`.

`mkfixture.cpp`: `#define ATX_FIXTURE_NO_GTEST` before including `test_support.hpp`; in test_support, define

```cpp
#ifdef ATX_FIXTURE_NO_GTEST
#define ATX_FIXTURE_FAIL(msg) do { std::fprintf(stderr, "fixture: %s\n", std::string(msg).c_str()); std::abort(); } while (0)
#else
#define ATX_FIXTURE_FAIL(msg) ADD_FAILURE() << (msg)
#endif
```

and replace the `ADD_FAILURE()` uses in test_support.cpp with `ATX_FIXTURE_FAIL`.

- [ ] **Step 4: Build all three, run the smoke test**

Run: `cmake --build --preset server && ctest --test-dir build-server -R atx-server-smoke --output-on-failure`
Expected: `SMOKE_OK`.

- [ ] **Step 5: Run the FULL server suite** — `ctest --test-dir build-server -L atx_server --output-on-failure` — all green.

- [ ] **Step 6: Commit**

```bash
git add atx-server/
git commit -m "feat(server): atx-server and atx-server-cli binaries with realm import, token admin, and process smoke"
```

---

### Task 16: README + final acceptance gate

**Files:**
- Create: `atx-server/README.md`
- Verify: full suite + spec §12 acceptance criteria

- [ ] **Step 1: Write README.md**

Cover, concretely (README follows `atx-ui/README.md`'s shape — what it is, boundaries, build/run, test):
- What atx-server is (one paragraph, cite the spec path).
- Build: `cmake --preset server && cmake --build --preset server`.
- First run walkthrough: `--add-token`, realm.json shape (paste a 5-line example), `--realm-import`, `--grant`, `ATX_SERVER_LISTENING`.
- CLI examples: the exact commands from smoke.ps1 (dbs / surfaces / curve / blob).
- Security defaults: loopback + token required; non-loopback needs TLS; tokens stored as sha256 only.
- The catalog: two-tier design in 4 sentences, snapshot file, what `state/` contains.
- Test: `ctest --test-dir build-server -L atx_server`.

- [ ] **Step 2: Walk spec §12 acceptance criteria 1-8**

| # | Criterion | Verified by |
|---|---|---|
| 1 | server preset builds; dev preset untouched | Task 1 steps 4-5 + full build now |
| 2 | `--realm-import` + loopback start | smoke.ps1 |
| 3 | cli surfaces query | smoke.ps1 |
| 4 | cli curve matches local | `GetCurveMatchesLocalSurface` |
| 5 | blob fidelity bit-identical | `BlobFidelity.RemoteBlobPricesBitIdenticalToLocal` |
| 6 | `ctest -L atx_server` green | Task 15 step 5, rerun now |
| 7 | non-loopback refusal, nonzero exit | `Config.NonLoopbackWithoutTlsRefused` + smoke.ps1 |
| 8 | restart restores catalog == cold scan | `LifecycleFixture.WarmStartEqualsColdScan` |

Run each verification command; paste actual output into the task log. Any red row blocks completion.

- [ ] **Step 3: Hygiene sweep**

Run: `cmake --preset dev && cmake --build --preset dev --target atx-vol-tests` — confirm the amended `sqlite.hpp` comment (Task 4) broke nothing and the dev build still never touches gRPC.

- [ ] **Step 4: Commit**

```bash
git add atx-server/README.md
git commit -m "docs(server): atx-server README with first-run walkthrough and acceptance-gate results"
```

---

## Self-Review Notes (already applied)

1. **Spec coverage:** §4.2 protos → T2; §4.3 layout/encoders → T3/T9; §4.4 realm → T5/T11/T15; §5 catalog/schema/FTS/refresh/warm-start/tokens → T4/T5/T8/T14; §6 threading decision + sqlite.hpp amendment → T4; §7 all 13 RPCs → T11/T12/T13; §8 error table, incident ids, UNAUTHENTICATED split, TLS gate → T10/T6/T3; §9 build wiring → T1; §11 test table → config(T3), schema(T4), determinism(T8), blob fidelity(T13), in-process services(T11-13), entitlements(T12), concurrency(T14), non-loopback(T3+T15), cli smoke(T15). §10 deferrals honored: no streaming, no SQL gateway, no chunked blobs anywhere above.
2. **Known deviation from spec §7:** `ServerInfo` does not carry the manifest/ATXVSA schema hashes (no public compile-time constant exports them); the ATXVSA hash travels on every `SurfaceBlob`, where it actually gates. Recorded here deliberately.
3. **Type consistency check:** `TokenState` at namespace scope (T5 note); `ServerDeps{catalog, registry, config, stats}` consistent T11-T13; `dump_main_no_ts` moved to test_support in T14 (T8 has the original — T14 explicitly migrates it); `Entitlements.db_ids` sorted (T5 `ORDER BY`) matching `binary_search` (T6).
4. **OTM side convention** (`strike >= F` => call) appears in three places that must agree: the proto comment (T2), `encode_curve` (T9), and the T9 greeks test. All three are written consistently; if any is edited, edit all three.

---

## Execution

Plan complete. Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks (superpowers:subagent-driven-development)
2. **Inline** — execute in-session with checkpoints (superpowers:executing-plans)
