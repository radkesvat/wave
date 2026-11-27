#pragma once

#include "panel_theme.h"
#include "tls.h"

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <optional>
#include <string_view>

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

    const std::filesystem::path& directory() const;
    const PanelSettings& settings() const;
    const nlohmann::json& initial_contents() const;
    std::string panel_base_path() const;
    bool save_panel_base_path(std::string_view path);
    nlohmann::json save_tls_settings(const std::optional<TlsSettings>& tls);
    int session_expiry_days() const;
    void save_session_expiry_days(int days);
    void save_theme(theme::Preset preset);
    void save_donation(std::string_view answer, std::string_view answered_at);
    bool claim_waterwall_install();
    void save_auth(const nlohmann::json& admins, const nlohmann::json& auth);
    void save_core_settings(const nlohmann::json& settings);
    void save_core_configs(const nlohmann::json& configs);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wave
