#include "local_cli.h"

#include "core_settings.h"
#include "panel_settings.h"
#include "private_temp_directory.h"
#include "waterwall_logs.h"
#include "waterwall_schematic_file.h"
#include "wave_version.h"

#include <httplib.h>

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

namespace wave {
namespace {

namespace fs = std::filesystem;

std::optional<int> positive_number(std::string_view text)
{
    int number = 0;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, number);
    return result.ec == std::errc{} && result.ptr == end && number > 0 ? std::optional(number) : std::nullopt;
}

std::string listener_url(const PanelSettings& settings, int port, std::string_view base_path)
{
    if (settings.tls) {
        return "https://" + tls_authority(settings.tls->domain, port) + std::string(base_path) + '/';
    }
    auto host = settings.listen_host;
    if (host == "0.0.0.0") {
        host = "127.0.0.1";
    } else if (host == "::") {
        host = "::1";
    }
    if (host.find(':') != std::string::npos) {
        host = '[' + host + ']';
    }
    return "http://" + host + ':' + std::to_string(port) + std::string(base_path) + '/';
}

void show_local_settings(const PanelDatabase& database)
{
    const auto settings = database.settings();
    const auto path = database.panel_base_path();
    const auto& contents = database.initial_contents();
    const auto& admins = contents.at("admin");
    const auto username = admins.empty() ? std::string_view{}
                                         : schematic_text(schematic_field(admins.front(), "username"));
    const auto core_version = schematic_text(schematic_field(contents, "waterwall_version"));
    std::cout << "Wave version: " << release_version << '\n'
              << "Install directory: " << database.directory() << '\n'
              << "Listen address: " << nlohmann::json(settings.listen_host).dump() << '\n'
              << "Port: " << settings.listen_port << (settings.listen_port ? "" : " (automatic)") << '\n'
              << "TLS: " << (settings.tls ? "enabled" : "disabled") << '\n';
    if (settings.tls) {
        std::cout << "TLS domain: " << nlohmann::json(settings.tls->domain).dump() << '\n';
    }
    std::cout << "Base path: " << (path.empty() ? "/ (setup pending)" : path) << '\n'
              << "Administrator: " << (username.empty() ? "setup pending" : nlohmann::json(username).dump()) << '\n'
              << "Session expiry: " << database.session_expiry_days() << " days\n"
              << "Selected WaterWall: " << (core_version.empty() ? bundled_waterwall_version : core_version) << '\n';
    if (settings.listen_port != 0) {
        std::cout << "Configured URL: " << nlohmann::json(listener_url(settings, settings.listen_port, path)).dump()
                  << '\n';
    }
}

void show_local_core_logs(PanelDatabase& database)
{
    select_waterwall_schematic(database, true);
    const auto settings = core_settings::normalize(
        database.initial_contents().value("core_settings", nlohmann::json::object()));
    if (!settings.issue.empty()) {
        throw std::runtime_error(settings.issue);
    }
    WaterWallLogs reader(database.directory());
    reader.start(settings.settings, true);
    const auto entries = reader.read();
    if (entries.empty()) {
        std::cout << "No recent WaterWall logs.\n";
    }
    for (const auto& entry : entries) {
        std::cout << entry.timestamp << " [" << entry.source << "] [" << entry.level << "] " << entry.message << '\n';
    }
}

void check_port(int port, const char* host)
{
    httplib::Server server;
    server.set_socket_options([](socket_t socket) { httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1); });
    if (!server.bind_to_port(host, port)) {
        throw std::runtime_error("Could not bind to " + std::string(host) + ':' + std::to_string(port) + '.');
    }
    server.stop();
}

} // namespace

std::optional<int> run_local_command(int argc, char* argv[])
{
    if (argc < 2) {
        return std::nullopt;
    }
    const std::string_view command = argv[1];
    try {
        if (command == "--version" && argc == 2) {
            std::cout << release_version << '\n';
#ifdef WAVE_ENABLE_AUTH
        } else if (command == "--supports-remote-setup" && argc == 2) {
            return EXIT_SUCCESS;
#endif
        } else if (command == "--check-port") {
            const auto port = argc == 3 || argc == 4 ? positive_number(argv[2]) : std::nullopt;
            if (!port || *port > 65535) {
                throw std::runtime_error("Usage: wave-backend --check-port PORT [HOST] (port 1 through 65535)");
            }
            check_port(*port, argc == 4 ? argv[3] : "127.0.0.1");
        } else if ((command == "--local-settings" || command == "--local-core-logs") && argc == 2) {
            PanelDatabase database(argv[0], {}, true);
            if (command == "--local-settings") {
                show_local_settings(database);
            } else {
                show_local_core_logs(database);
            }
        } else {
            return std::nullopt;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

void validate_ready_file(const fs::path& path, const fs::path& directory)
{
    struct stat info{};
    if (path != directory / ".wave-run" / "ready" || ::lstat(path.parent_path().c_str(), &info) != 0 ||
        !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0777) != 0700) {
        throw std::runtime_error("Managed readiness requires a mode-0700 .wave-run directory beside the executable.");
    }
}

void publish_listener_ready(const fs::path& path, const PanelSettings& settings, int port,
                            std::string_view base_path) noexcept
{
    try {
        const PrivateTempDirectory staging(path.parent_path(), ".ready-");
        if (staging.path().empty()) {
            throw std::runtime_error("Cannot stage the Wave readiness file.");
        }
        const auto candidate = staging.path() / "ready";
        std::ofstream output(candidate);
        output << ::getpid() << '\n' << listener_url(settings, port, base_path) << '\n';
        output.close();
        if (!output) {
            throw std::runtime_error("Cannot write the Wave readiness file.");
        }
        fs::rename(candidate, path);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
}

} // namespace wave
