#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace wave {

class PrivateTempDirectory {
  public:
    PrivateTempDirectory(const std::filesystem::path& parent, std::string_view prefix)
        : name_((parent / (std::string(prefix) + "XXXXXX")).string())
    {
        if (!::mkdtemp(name_.data())) {
            name_.clear();
        }
    }

    PrivateTempDirectory(const PrivateTempDirectory&) = delete;
    PrivateTempDirectory& operator=(const PrivateTempDirectory&) = delete;

    ~PrivateTempDirectory()
    {
        if (!name_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(name_, ignored);
        }
    }

    std::filesystem::path path() const
    {
        return name_;
    }

  private:
    std::string name_;
};

} // namespace wave
