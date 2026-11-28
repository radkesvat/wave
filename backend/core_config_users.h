#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace wave {

class CoreConfigUsers {
  public:
    struct Result {
        bool success;
        std::string message;
        nlohmann::json result = nullptr;
        std::optional<nlohmann::json> roster_change;
        bool database_unavailable = false;
    };

    explicit CoreConfigUsers(const std::filesystem::path& executable_directory);

    static bool valid_filename(std::string_view db_name);
    static std::filesystem::path relative_path(std::string_view config_name, std::string_view db_name);
    static std::string validate_schematic_editor(const nlohmann::json& users);

    std::string prepare(std::string_view config_name, std::string_view db_name, bool& created) const;
    bool remove(std::string_view config_name, std::string_view db_name) const;
    Result load_roster(std::string_view config_name, std::string_view db_name) const;
    Result execute(std::string_view action, std::string_view config_name, std::string_view db_name,
                   const nlohmann::json& input, bool include_roster_change = false) const;

  private:
    std::filesystem::path root_;
};

} // namespace wave
