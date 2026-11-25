#include <httplib.h>

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

struct Options {
    std::filesystem::path site_dir = "build/web-release/site";
    std::string host = "127.0.0.1";
    int port = 2030;
};

bool parse_options(int argc, char* argv[], Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option != "--site-dir" && option != "--host" && option != "--port") {
            std::cerr << "Unknown option: " << option << '\n';
            return false;
        }

        const std::string_view value = ++i < argc ? argv[i] : "";
        if (value.empty() || value.starts_with("--")) {
            std::cerr << "Missing value for " << option << '\n';
            return false;
        }

        if (option == "--site-dir") {
            options.site_dir = value;
        } else if (option == "--host") {
            options.host = value;
        } else {
            const auto end = value.data() + value.size();
            const auto [ptr, error] = std::from_chars(value.data(), end, options.port);
            if (error != std::errc{} || ptr != end || options.port < 0 || options.port > 65535) {
                std::cerr << "Invalid port: " << value << " (expected an integer from 0 to 65535)\n";
                return false;
            }
        }
    }
    return true;
}

bool validate_site(const std::filesystem::path& site_dir)
{
    std::error_code error;
    if (!std::filesystem::is_directory(site_dir, error)) {
        std::cerr << "Cannot access site directory: " << site_dir << '\n';
        return false;
    }

    for (const char* name : {"index.html", "index.js", "index.wasm"}) {
        const auto asset = site_dir / name;
        if (!std::filesystem::is_regular_file(asset, error)) {
            std::cerr << "Missing required site asset: " << asset << '\n';
            return false;
        }
    }
    return true;
}

bool configure_routes(httplib::Server& server, const std::filesystem::path& site_dir)
{
    if (!server.set_mount_point("/", site_dir.string())) {
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
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr << "Usage: wave-backend [--site-dir PATH] [--host HOST] [--port PORT]\n";
        return EXIT_FAILURE;
    }
    if (!validate_site(options.site_dir)) {
        return EXIT_FAILURE;
    }

    httplib::Server server;
    if (!configure_routes(server, options.site_dir)) {
        std::cerr << "Could not mount site directory: " << options.site_dir << '\n';
        return EXIT_FAILURE;
    }

    // Override cpp-httplib's port-sharing defaults so an occupied port fails to bind.
    server.set_socket_options([](socket_t socket) {
#ifdef _WIN32
        httplib::set_socket_opt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });

    int port = options.port;
    const bool bound = port == 0 ? (port = server.bind_to_any_port(options.host)) > 0
                                 : server.bind_to_port(options.host, port);
    const auto url_host = options.host.find(':') == std::string::npos
        ? options.host
        : '[' + options.host + ']';
    if (!bound) {
        std::cerr << "Could not bind to " << url_host << ':' << options.port << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Listening on http://" << url_host << ':' << port << '/' << std::endl;
    if (!server.listen_after_bind()) {
        std::cerr << "Could not serve requests on " << url_host << ':' << port << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
