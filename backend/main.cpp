#include "auth.h"
#include "auth_store.h"
#include "console.h"
#include "panel_settings.h"
#include "preview.h"
#include "waterwall_schematic_file.h"
#include "workspace_backup.h"

#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

void wave::ClosePanelListener::operator()(httplib::Server* listener) const noexcept
{
    listener->stop();
    delete listener;
}

namespace {

struct Options {
    std::filesystem::path site_dir = "build/web-release/site";
    bool check_tls = false;
    std::filesystem::path check_schematic;
};

bool parse_options(int argc, char* argv[], Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == "--check-tls") {
            options.check_tls = true;
            continue;
        }
        if (option != "--site-dir" && option != "--check-schematic") {
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

        if (option == "--check-schematic") {
            options.check_schematic = value;
        } else {
            options.site_dir = value;
        }
    }
    if (options.check_tls && !options.check_schematic.empty()) {
        std::cerr << "Choose either --check-tls or --check-schematic.\n";
        return false;
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
                                const std::shared_ptr<wave::PanelDatabase>& database,
                                const wave::PreviewRoutes& preview)
{
    server.WebSocket(wave::panel_route("/ws"), [store, database](const httplib::Request& req, httplib::ws::WebSocket& socket) {
        using namespace std::chrono_literals;
        const auto session = wave::websocket_session(store, database, req);
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
    wave::configure_console_route(server, store, database);
    preview.configure(server);
}

void configure_listener_socket(httplib::Server& server)
{
    // Override cpp-httplib's port-sharing defaults so an occupied port fails to bind.
    server.set_socket_options([](socket_t socket) {
#ifdef _WIN32
        httplib::set_socket_opt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });
}

} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr << "Usage: wave-backend [--site-dir PATH] [--check-tls | --check-schematic PATH]\n";
        return EXIT_FAILURE;
    }

    bool secure = false;
    wave::PanelSettings settings{};
    std::shared_ptr<wave::PanelDatabase> database;
    std::shared_ptr<wave::AuthStore> store;
    std::unique_ptr<httplib::Server> server;
    try {
        if (!options.check_schematic.empty()) {
            wave::load_waterwall_schematic_file(options.check_schematic);
            std::cout << "WaterWall schematic is valid.\n";
            return EXIT_SUCCESS;
        }
        database =
            std::make_shared<wave::PanelDatabase>(argv[0], options.site_dir, options.check_tls);
        if (!options.check_tls) {
            wave::recover_workspace_restore(*database);
        }
        settings = database->settings();
        secure = settings.tls.has_value();
        if (!options.check_tls && !validate_site(options.site_dir, secure)) {
            return EXIT_FAILURE;
        }
        if (options.check_tls && !secure) {
            throw std::runtime_error("TLS is disabled in wave-db.json.");
        }
        server = database->make_listener(settings);
        if (options.check_tls) {
            std::cout << "TLS configuration is valid.\n";
            return EXIT_SUCCESS;
        }
        const auto authority = secure ? wave::tls_authority(settings.tls->domain, settings.listen_port) : "";
        wave::select_waterwall_schematic(*database);
        store = std::make_shared<wave::AuthStore>(database);
        wave::configure_authenticated_routes(*server, options.site_dir, store, database, secure, authority);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    configure_listener_socket(*server);
    const auto url_host = [&] {
        const auto& host = settings.listen_host;
        return host.find(':') == std::string::npos ? host : '[' + host + ']';
    };
    const auto endpoint = [&]() -> std::string {
        return secure ? "the authenticated HTTPS listener on port " : url_host() + ':';
    };
    int port = settings.listen_port;
    const bool bound = port == 0 ? (port = server->bind_to_any_port(settings.listen_host)) > 0
                                 : server->bind_to_port(settings.listen_host, port);
    if (!bound) {
        std::cerr << "Could not bind to " << endpoint() << settings.listen_port << '\n';
        return EXIT_FAILURE;
    }

    std::optional<wave::PanelListenerRestart> pending_restart;
    wave::PreviewRoutes preview;
    try {
        const auto prepare_restart = [&](std::unique_ptr<httplib::Server> listener, wave::PanelSettings next) {
            wave::PanelListenerRestart restart{{}, std::move(next), 0, false};
            restart.listener.reset(listener.release());
            const auto& target = restart.settings;
            const auto authority = target.tls ? wave::tls_authority(target.tls->domain, target.listen_port) : "";
            wave::configure_authenticated_routes(*restart.listener, options.site_dir, store, database,
                                                 target.tls.has_value(), authority);
            configure_workspace_routes(*restart.listener, store, database, preview);
            configure_listener_socket(*restart.listener);
            restart.port = target.listen_port;
            restart.bind_after_stop = restart.port == port;
            // Reserve a different port before telling the browser where to reconnect.
            if (restart.port == 0) {
                restart.port = restart.listener->bind_to_any_port(target.listen_host);
                if (restart.port <= 0) {
                    throw std::runtime_error("Could not allocate a port for the restored panel listener.");
                }
            } else if (!restart.bind_after_stop && !restart.listener->bind_to_port(target.listen_host, restart.port)) {
                throw std::runtime_error("Could not bind the panel listener to port " + std::to_string(restart.port) + '.');
            }
            return restart;
        };
        preview = wave::make_preview_routes(store, database, prepare_restart, [&](wave::PanelListenerRestart restart) {
            // Preview accepts one restart at a time. The main thread joins the request workers
            // before consuming the replacement listener.
            pending_restart.emplace(std::move(restart));
            server->stop();
            auto& next = *pending_restart;
            if (next.bind_after_stop) {
                // Open the accept queue before acknowledging the switch. If the old connections
                // still hold the port, let the main thread retry after those workers have drained.
                if (next.listener->bind_to_port(next.settings.listen_host, next.port)) {
                    next.bind_after_stop = false;
                } else {
                    next.listener->stop();
                }
            }
        });
        configure_workspace_routes(*server, store, database, preview);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    for (;;) {
        if (secure) {
            std::cout << "Authenticated HTTPS listening on port " << port << std::endl;
        } else {
            std::cout << "Listening on http://" << url_host() << ':' << port << '/' << std::endl;
        }
        if (!server->listen_after_bind()) {
            std::cerr << "Could not serve requests on " << endpoint() << port << '\n';
            return EXIT_FAILURE;
        }
        if (!pending_restart) {
            return EXIT_SUCCESS;
        }

        auto restart = std::move(*pending_restart);
        pending_restart.reset();
        if (restart.bind_after_stop && !restart.listener->bind_to_port(restart.settings.listen_host, restart.port)) {
            std::cerr << "Could not bind the panel listener to port " << restart.port << '\n';
            restart.listener.reset();
            if (!server->bind_to_port(settings.listen_host, port)) {
                std::cerr << "Could not resume " << endpoint() << port << '\n';
                return EXIT_FAILURE;
            }
            database->finish_listener_restart(std::nullopt);
            preview.finish_listener_restart(false);
            continue;
        }
        settings = std::move(restart.settings);
        port = restart.port;
        secure = settings.tls.has_value();
        database->finish_listener_restart(settings);
        server.reset(restart.listener.release());
        preview.finish_listener_restart(true);
    }
}
