#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace httplib {
class Server;
}

namespace wave {

struct TlsSettings {
    std::string domain;
    std::filesystem::path certificate_chain;
    std::filesystem::path private_key;
};

void check_tls(const TlsSettings& settings, const std::filesystem::path& site_dir);
std::unique_ptr<httplib::Server> make_authenticated_server(const std::filesystem::path& state_dir,
    const std::filesystem::path& site_dir, int port, const TlsSettings& settings);

} // namespace wave
