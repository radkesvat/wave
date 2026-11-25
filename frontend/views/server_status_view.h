#pragma once

#include <nlohmann/json_fwd.hpp>

namespace wave {

void render_server_status(const nlohmann::json& server, bool narrow);

} // namespace wave
