#include "auth.h"
#include "auth_store.h"
#include "panel_settings.h"
#include "preview.h"

#include <httplib.h>

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
        if (option != "--site-dir") {
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

        options.site_dir = value;
    }
    return true;
}

bool validate_site(const std::filesystem::path& site_dir, bool secure)
{
    std::error_code error;
    if (!std::filesystem::is_directory(site_dir, error)) {
        if (secure) {
            std::cerr << "Cannot access the authenticated site directory.\n";
        } else {
            std::cerr << "Cannot access site directory: " << site_dir << '\n';
        }
        return false;
    }

    for (const char* name : {"index.html", "index.js", "index.wasm"}) {
        const auto asset = site_dir / name;
        if (!std::filesystem::is_regular_file(asset, error)) {
            if (secure) {
                std::cerr << "Missing authenticated site asset " << name << '\n';
            } else {
                std::cerr << "Missing required site asset: " << asset << '\n';
            }
            return false;
        }
    }
    return true;
}

void configure_workspace_routes(httplib::Server& server, const std::shared_ptr<wave::AuthStore>& store,
                                const std::shared_ptr<wave::PanelDatabase>& database, bool first_install)
{
    server.WebSocket("/ws", [store](const httplib::Request& req, httplib::ws::WebSocket& socket) {
        using namespace std::chrono_literals;
        const auto session = wave::websocket_session(store, req);
        socket.set_read_timeout(1s);
        std::string message;
        while (const auto result = socket.read(message)) {
            if (!session()) {
                socket.close(httplib::ws::CloseStatus::PolicyViolation, "Session ended.");
                break;
            }
            if (result == httplib::ws::Text && !socket.send(message)) {
                break;
            }
        }
    });
    wave::configure_preview_routes(server, store, database, first_install);
}

} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr << "Usage: wave-backend [--site-dir PATH] [--check-tls]\n";
        return EXIT_FAILURE;
    }

    bool secure = false;
    wave::PanelSettings settings{};
    std::shared_ptr<wave::PanelDatabase> database;
    std::shared_ptr<wave::AuthStore> store;
    std::unique_ptr<httplib::Server> server;
    try {
        database =
            std::make_shared<wave::PanelDatabase>(argv[0], options.site_dir, options.check_tls);
        settings = database->settings();
        secure = settings.tls.has_value();
        if (!options.check_tls && !validate_site(options.site_dir, secure)) {
            return EXIT_FAILURE;
        }
        if (options.check_tls && !secure) {
            throw std::runtime_error("TLS is disabled in wave-db.json.");
        }
        std::string authority;
        if (secure) {
#ifdef WAVE_ENABLE_AUTH
            if (options.check_tls) {
                wave::check_tls(*settings.tls, options.site_dir);
                std::cout << "TLS configuration is valid.\n";
                return EXIT_SUCCESS;
            }
            authority = settings.tls->domain;
            if (settings.listen_port != 443) {
                authority += ':' + std::to_string(settings.listen_port);
            }
            server = wave::make_tls_server(*settings.tls, options.site_dir, settings.listen_port);
#else
            throw std::runtime_error("TLS is enabled in wave-db.json. Build with "
                                     "WAVE_ENABLE_AUTH=ON to use it.");
#endif
        } else {
            server = std::make_unique<httplib::Server>();
        }
        store = std::make_shared<wave::AuthStore>(database);
        wave::configure_authenticated_routes(*server, options.site_dir, store, secure, authority);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    // Override cpp-httplib's port-sharing defaults so an occupied port fails to
    // bind.
    server->set_socket_options([](socket_t socket) {
#ifdef _WIN32
        httplib::set_socket_opt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });

    const auto& host = settings.listen_host;
    int port = settings.listen_port;
    const bool bound =
        port == 0 ? (port = server->bind_to_any_port(host)) > 0 : server->bind_to_port(host, port);
    const auto url_host = host.find(':') == std::string::npos ? host : '[' + host + ']';
    const auto endpoint = secure ? "the authenticated HTTPS listener on port " : url_host + ':';
    if (!bound) {
        std::cerr << "Could not bind to " << endpoint << settings.listen_port << '\n';
        return EXIT_FAILURE;
    }
    try {
        const bool first_install = database->claim_waterwall_install();
        configure_workspace_routes(*server, store, database, first_install);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    if (secure) {
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
