#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace wave {

struct ConfigJsonImport {
    nlohmann::json document;
    bool needs_confirmation = false;
    std::string error;
};

ConfigJsonImport parse_config_json_import(std::string_view text);

} // namespace wave
