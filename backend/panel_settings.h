#pragma once

#include "tls.h"

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <optional>

namespace wave {

struct PanelSettings {
    std::string listen_host;
    int listen_port;
    std::optional<TlsSettings> tls;
};

class PanelDatabase {
  public:
    PanelDatabase(const char* executable, const std::filesystem::path& site_dir, bool check_only);
    ~PanelDatabase();

    const PanelSettings& settings() const;
    const nlohmann::json& initial_contents() const;
    void save_auth(const nlohmann::json& admins, const nlohmann::json& auth);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wave
