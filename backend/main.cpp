#include "overview_data.h"
#include "panel_settings.h"
#include "preview.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

struct Options {
    std::filesystem::path site_dir = "build/web-release/site";
    std::filesystem::path auth_dir = "/var/lib/wave-auth";
    bool check_tls = false;
};

bool parse_options(int argc, char* argv[], Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == "--check-tls") {
            options.check_tls = true;
            continue;
        }
        if (option != "--site-dir" && option != "--auth-dir") {
            if (option.starts_with("--")) {
                std::cerr << "Unknown option: " << option.substr(0, option.find('=')) << '\n';
            } else {
                std::cerr << "Unexpected argument.\n";
            }
            return false;
        }

        const std::string_view value = ++i < argc ? argv[i] : "";
        if (value.empty() || value.starts_with("--")) {
            std::cerr << "Missing value for " << option << '\n';
            return false;
        }

        if (option == "--site-dir") {
            options.site_dir = value;
        } else {
            options.auth_dir = value;
        }
    }
    return true;
}

bool validate_site(const std::filesystem::path& site_dir, bool authenticated)
{
    std::error_code error;
    if (!std::filesystem::is_directory(site_dir, error)) {
        if (authenticated) {
            std::cerr << "Cannot access the authenticated site directory.\n";
        } else {
            std::cerr << "Cannot access site directory: " << site_dir << '\n';
        }
        return false;
    }

    for (const char* name : {"index.html", "index.js", "index.wasm"}) {
        const auto asset = site_dir / name;
        if (!std::filesystem::is_regular_file(asset, error)) {
            if (authenticated) {
                std::cerr << "Missing authenticated site asset " << name << '\n';
            } else {
                std::cerr << "Missing required site asset: " << asset << '\n';
            }
            return false;
        }
    }
    return true;
}

std::string make_overview_snapshot()
{
    using namespace std::chrono_literals;
    constexpr wave::OverviewData sample{
        .cpu_percent = 28.0f,
        .memory_used_gib = 3.2f,
        .memory_total_gib = 8.0f,
        .uptime = 103h + 23min,
        .download_mib_per_second = {2.1f, 3.0f, 2.8f, 5.5f, 4.6f, 7.9f, 6.8f,
                                    10.2f, 8.3f, 8.9f, 5.7f, 6.2f, 7.4f},
    };
    return nlohmann::json{
        {"cpu_percent", sample.cpu_percent},
        {"memory_used_gib", sample.memory_used_gib},
        {"memory_total_gib", sample.memory_total_gib},
        {"uptime_minutes", sample.uptime.count()},
        {"download_mib_per_second", sample.download_mib_per_second},
    }.dump();
}

bool configure_demo_routes(httplib::Server& server, const std::filesystem::path& site_dir)
{
    if (!server.set_mount_point("/", site_dir.string(), {{"Cache-Control", "no-store"}})) {
        return false;
    }

    server.WebSocket("/ws", [](const httplib::Request&, httplib::ws::WebSocket& socket) {
        std::string message;
        while (const auto result = socket.read(message)) {
            if (result == httplib::ws::Text && !socket.send(message)) {
                break;
            }
        }
    });
    server.WebSocket("/ws/overview",
        [snapshot = make_overview_snapshot()](const httplib::Request&, httplib::ws::WebSocket& socket) {
            if (socket.send(snapshot)) {
                socket.close();
            }
        });
    wave::configure_preview_route(server);
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr << "Usage: wave-backend [--site-dir PATH] [--auth-dir PATH] [--check-tls]\n";
        return EXIT_FAILURE;
    }

    bool authenticated = false;
    wave::PanelSettings settings{};
    std::unique_ptr<httplib::Server> server;
    try {
        settings = wave::load_panel_settings(argv[0], options.site_dir);
        authenticated = settings.tls.has_value();
        if (!options.check_tls && !validate_site(options.site_dir, authenticated)) {
            return EXIT_FAILURE;
        }
        if (options.check_tls && !authenticated) {
            throw std::runtime_error("TLS is disabled in wave-db.json.");
        }
        if (authenticated) {
#ifdef WAVE_ENABLE_AUTH
            if (options.check_tls) {
                wave::check_tls(*settings.tls, options.site_dir);
                std::cout << "TLS configuration is valid.\n";
                return EXIT_SUCCESS;
            }
            server = wave::make_authenticated_server(options.auth_dir, options.site_dir, settings.listen_port,
                                                     *settings.tls);
#else
            throw std::runtime_error("TLS is enabled in wave-db.json. Build with WAVE_ENABLE_AUTH=ON to use it.");
#endif
        } else {
            server = std::make_unique<httplib::Server>();
            if (!configure_demo_routes(*server, options.site_dir)) {
                throw std::runtime_error("Could not mount the demo site directory.");
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    // Override cpp-httplib's port-sharing defaults so an occupied port fails to bind.
    server->set_socket_options([](socket_t socket) {
#ifdef _WIN32
        httplib::set_socket_opt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });

    const auto& host = settings.listen_host;
    int port = settings.listen_port;
    const bool bound = port == 0 ? (port = server->bind_to_any_port(host)) > 0
                                 : server->bind_to_port(host, port);
    const auto url_host = host.find(':') == std::string::npos ? host : '[' + host + ']';
    const auto endpoint = authenticated ? "the authenticated HTTPS listener on port " : url_host + ':';
    if (!bound) {
        std::cerr << "Could not bind to " << endpoint << settings.listen_port << '\n';
        return EXIT_FAILURE;
    }

    if (authenticated) {
        std::cout << "Authenticated HTTPS listening on port " << port << std::endl;
    } else {
        std::cout << "Listening on http://" << url_host << ':' << port << '/' << std::endl;
    }
    if (!server->listen_after_bind()) {
        std::cerr << "Could not serve requests on " << endpoint << port << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
