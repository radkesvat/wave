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

void validate_tls_settings(const TlsSettings& settings, int port);
std::unique_ptr<httplib::Server> make_tls_server(const TlsSettings& settings,
                                              const std::filesystem::path& site_dir, int port);

} // namespace wave
