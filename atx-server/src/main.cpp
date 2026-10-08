// SAFETY: getenv is read once before worker startup and immediately copied into an owned
// string. This TU-local suppression retains portable C++ without manual CRT buffer ownership.
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#if defined(_WIN32) && !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif

#include <atx/server/core.hpp>
#include <atx/server/access.hpp>
#include <atx/server/research.hpp>
#include <atx/server/runtime_identity.hpp>
#include <atx/server/service.hpp>

#include <grpcpp/grpcpp.h>
#include <grpcpp/resource_quota.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
struct Options {
    std::string listen{"127.0.0.1:50051"};
    std::filesystem::path db{"data/workspace.db"};
    std::filesystem::path repo_root{std::filesystem::current_path()};
    std::vector<std::filesystem::path> artifact_roots;
    std::filesystem::path tls_certificate;
    std::filesystem::path tls_key;
    std::filesystem::path team_config;
    std::vector<std::pair<std::string, std::filesystem::path>> data_sources;
    std::vector<std::filesystem::path> data_roots;
    std::vector<atx::server::ResearchDataset> research_datasets;
    std::uint32_t workers{2};
    std::uint32_t capacity{1024};
};
std::uint32_t positive_integer(std::string_view text, std::uint32_t maximum) {
    std::uint32_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value == 0 || value > maximum) throw std::invalid_argument("invalid numeric option");
    return value;
}
Options arguments(int argc, char* argv[]) {
    Options options;
    // Allow bounded source/root/research registrations plus ordinary service options.
    if (argc > 256) throw std::invalid_argument("too many arguments");
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--help") {
            std::cout << "atx-server [--listen 127.0.0.1:50051] [--db data/workspace.db]\n"
                "  [--workers 2] [--capacity 1024] [--repo-root PATH] [--artifact-root PATH]\n"
                "  [--tls-cert PATH --tls-key PATH]\n"
                "  [--team-config PATH] (operator-managed shared-team credentials and roles)\n"
                "  [--data-source ID=PATH] [--data-root PATH] (repeatable)\n"
                "  [--research-dataset ID=SHA256=MANIFEST] (repeatable, pinned TRAIN data)\n"
                "ATX_SERVER_TOKEN supplies bearer authorization; "
                "external binds require TLS + token.\n";
            std::exit(0);
        }
        if (i + 1 >= argc) throw std::invalid_argument("missing value for " + std::string(flag));
        const std::string value(argv[++i]);
        if (flag == "--listen") options.listen = value;
        else if (flag == "--db") options.db = value;
        else if (flag == "--workers") options.workers = positive_integer(value, 64);
        else if (flag == "--capacity") options.capacity = positive_integer(value, 100000);
        else if (flag == "--repo-root") options.repo_root = value;
        else if (flag == "--artifact-root") options.artifact_roots.emplace_back(value);
        else if (flag == "--tls-cert") options.tls_certificate = value;
        else if (flag == "--tls-key") options.tls_key = value;
        else if (flag == "--team-config") options.team_config = value;
        else if (flag == "--data-root") options.data_roots.emplace_back(value);
        else if (flag == "--data-source") {
            const auto equal = value.find('=');
            if (equal == std::string::npos || equal == 0 || equal + 1 == value.size()) {
                throw std::invalid_argument("data source must have ID=PATH form");
            }
            options.data_sources.emplace_back(value.substr(0, equal), value.substr(equal + 1));
        }
        else if (flag == "--research-dataset") {
            const auto first = value.find('=');
            const auto second = first == std::string::npos ? first : value.find('=', first + 1);
            if (first == std::string::npos || second == std::string::npos || first == 0 ||
                second - first != 65 || second + 1 == value.size() ||
                options.research_datasets.size() >= 32) {
                throw std::invalid_argument("research dataset must have ID=SHA256=MANIFEST form");
            }
            const auto id = value.substr(0, first);
            options.research_datasets.push_back({id, id, value.substr(second + 1),
                                                 value.substr(first + 1, 64)});
        }
        else throw std::invalid_argument("unknown option " + std::string(flag));
    }
    if (options.db.empty() || options.db == ":memory:") {
        throw std::invalid_argument("server requires a persistent database path");
    }
    if (options.artifact_roots.empty()) options.artifact_roots.push_back(options.repo_root);
    return options;
}
bool loopback_listener(std::string_view listen) {
    const auto colon = listen.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || listen.size() > 300) {
        throw std::invalid_argument("listen must have host:port form");
    }
    static_cast<void>(positive_integer(listen.substr(colon + 1), 65535));
    const auto host = listen.substr(0, colon);
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}
std::string read_pem(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 1024 * 1024) {
        throw std::invalid_argument("TLS file exceeds 1 MiB");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read TLS file");
    std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad() || contents.empty()) throw std::runtime_error("invalid TLS file");
    return contents;
}
std::string token_from_environment() {
    const auto* value = std::getenv("ATX_SERVER_TOKEN");
    const std::string token(value == nullptr ? "" : value);
    if (!token.empty() && (token.size() < 32 || token.size() > 256)) {
        throw std::invalid_argument("ATX_SERVER_TOKEN must contain 32 to 256 characters");
    }
    for (const auto ch : token) {
        if (ch < '!' || ch > '~') throw std::invalid_argument("token must contain printable ASCII");
    }
    return token;
}

std::shared_ptr<const atx::server::AccessPolicy> read_team_policy(
    const std::filesystem::path& path, std::string_view service_token) {
    if (path.empty()) return {};
    if (service_token.empty())
        throw std::invalid_argument(
            "team mode requires a separate ATX_SERVER_TOKEN for the gateway");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::invalid_argument("cannot read team configuration");
    std::string document(65537, '\0');
    stream.read(document.data(), static_cast<std::streamsize>(document.size()));
    const auto count = stream.gcount();
    if (stream.bad() || count <= 0 || count > 65536)
        throw std::invalid_argument("team configuration is empty, unreadable or exceeds 64 KiB");
    document.resize(static_cast<std::size_t>(count));
    const auto now = atx::server::unix_ms();
    auto policy = std::make_shared<const atx::server::AccessPolicy>(document, now);
    if (policy->contains_credential("Bearer " + std::string(service_token)))
        throw std::invalid_argument("gateway credential must be separate from team credentials");
    return policy;
}

// A process lease protects restart recovery from marking another server's live work FAILED.
class DatabaseLease final {
public:
    explicit DatabaseLease(const std::filesystem::path& database) {
        auto path = database;
        path += ".lock";
#ifdef _WIN32
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("database is already leased");
        handle_.reset(handle);
#else
        descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (descriptor_ < 0) throw std::runtime_error("cannot create database lease");
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("database is already leased");
        }
#endif
    }
    ~DatabaseLease() {
#ifndef _WIN32
        if (descriptor_ >= 0) ::close(descriptor_);
#endif
    }
    DatabaseLease(const DatabaseLease&) = delete;
    DatabaseLease& operator=(const DatabaseLease&) = delete;
    DatabaseLease(DatabaseLease&&) = delete;
    DatabaseLease& operator=(DatabaseLease&&) = delete;
private:
#ifdef _WIN32
    struct Closer { void operator()(void* handle) const noexcept { CloseHandle(handle); } };
    std::unique_ptr<void, Closer> handle_;
#else
    int descriptor_{-1};
#endif
};

int run(const Options& options) {
    const auto runtime = atx::server::identify_runtime(atx::server::current_executable_path());
    const auto token = token_from_environment();
    const auto team = read_team_policy(options.team_config, token);
    const auto local = loopback_listener(options.listen);
    const auto tls = !options.tls_certificate.empty() && !options.tls_key.empty();
    if (options.tls_certificate.empty() != options.tls_key.empty()) {
        throw std::invalid_argument("TLS requires both --tls-cert and --tls-key");
    }
    if (!local && (token.empty() || !tls)) {
        throw std::invalid_argument("external listen requires ATX_SERVER_TOKEN and TLS");
    }
    std::shared_ptr<grpc::ServerCredentials> credentials;
    if (tls) {
        grpc::SslServerCredentialsOptions security;
        security.pem_key_cert_pairs.push_back({read_pem(options.tls_key),
                                               read_pem(options.tls_certificate)});
        credentials = grpc::SslServerCredentials(security);
    } else credentials = grpc::InsecureServerCredentials();
    if (!credentials) throw std::runtime_error("cannot initialize server credentials");
    std::vector<atx::server::DataSource> configured_sources;
    for (const auto& [id, path] : options.data_sources) {
        configured_sources.push_back({id, id, atx::server::make_duckdb_provider(path,
            {options.data_roots})});
    }
    const atx::server::DatabaseRegistry databases(std::move(configured_sources));
    const auto database = std::filesystem::weakly_canonical(std::filesystem::absolute(options.db));
    std::filesystem::create_directories(database.parent_path());
    DatabaseLease lease(database);
    atx::server::ResearchOptions research_options;
    research_options.repo_root = options.repo_root;
    research_options.artifact_roots = options.artifact_roots;
    research_options.datasets = options.research_datasets;
    research_options.runtime_build_sha256 = runtime.sha256;
    research_options.output_root = database.parent_path() / "research";
    std::filesystem::create_directories(research_options.output_root);
    const atx::server::ResearchBackend backend(std::move(research_options));
    atx::server::Store store(database.string());
    atx::server::EventBus events(2048, &store);
    const auto workflow_authorize = [team_mode = static_cast<bool>(team),
        principals = team ? team->principals() : std::vector<atx::server::Principal>{}]
        (std::string_view actor_id, std::int64_t now_ms) {
        if (!team_mode) return actor_id == "local";
        return std::any_of(principals.begin(), principals.end(), [&](const auto& principal) {
            return principal.id == actor_id &&
                atx::server::permits(principal.role, atx::server::Permission::research) &&
                (principal.expires_at_ms == 0 || principal.expires_at_ms > now_ms);
        });
    };
    atx::server::Scheduler scheduler(store, events,
        [&](std::string_view request, std::stop_token stop, const atx::server::Progress& progress) {
            return backend.execute(request, stop, progress);
        }, options.workers, options.capacity, false, workflow_authorize);
    atx::server::WorkspaceService service(store, scheduler, events,
        [&] { return backend.strategies_json(); }, {token, 32, team}, &databases, &backend);
    // MAX_POLLERS covers waiting pollers; active synchronous RPCs consume this shared quota.
    grpc::ResourceQuota transport_quota("atx-server");
    transport_quota.Resize(128U * 1024U * 1024U).SetMaxThreads(96);
    grpc::ServerBuilder builder;
    builder.SetResourceQuota(transport_quota);
    builder.SetMaxReceiveMessageSize(128 * 1024);
    builder.SetMaxSendMessageSize(20 * 1024 * 1024);
    builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::NUM_CQS, 1);
    builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MIN_POLLERS, 2);
    builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MAX_POLLERS, 64);
    builder.AddListeningPort(options.listen, credentials);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server) throw std::runtime_error("cannot bind gRPC server listener");
    try {
        scheduler.start();
    } catch (...) {
        scheduler.shutdown();
        events.close();
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
        server->Wait();
        throw;
    }
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#ifdef SIGBREAK
    std::signal(SIGBREAK, signal_handler);
#endif
    events.publish("system", "server.ready", "", "{}");
    std::cout << "atx-server listening on " << options.listen << ", database " << database << '\n';
    for (; interrupted == 0;) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    events.close();
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
    scheduler.shutdown();
    server->Wait();
    return 0;
}
} // namespace

int main(int argc, char* argv[]) {
    try { return run(arguments(argc, argv)); }
    catch (const std::exception& error) {
        std::cerr << "atx-server: " << error.what() << '\n';
        return 1;
    }
}
