#pragma once

#include "core_config_json.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace wave {

struct CoreConfigDocument {
    nlohmann::json config;
    std::string revision;
    std::string error;
};

class CoreConfigFiles {
  public:
    explicit CoreConfigFiles(const std::filesystem::path& directory);

    bool create(std::string_view name, bool reuse_existing, bool& created) const;
    bool remove(std::string_view name) const;
    bool available(std::string_view name) const;
    CoreConfigDocument read(std::string_view name) const;
    std::optional<std::string> save(std::string_view name, std::string_view serialized) const;
    static std::filesystem::path relative_path(std::string_view name);

  private:
    std::filesystem::path root_;
};

} // namespace wave
