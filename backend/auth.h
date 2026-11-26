#pragma once

#include <filesystem>
#include <string>

namespace httplib {
class Server;
}

namespace wave {

void configure_authenticated_routes(httplib::Server& server, const std::filesystem::path& site_dir,
                                    const std::filesystem::path& state_dir, const std::string& authority);

} // namespace wave
