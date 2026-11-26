#pragma once

#include <filesystem>
#include <string_view>

namespace wave {

class CoreConfigFiles {
public:
    explicit CoreConfigFiles(const std::filesystem::path& directory);

    bool create(std::string_view name, bool reuse_existing, bool& created) const;
    bool remove(std::string_view name) const;
    bool available(std::string_view name) const;
    static std::filesystem::path relative_path(std::string_view name);

private:
    std::filesystem::path root_;
};

} // namespace wave
