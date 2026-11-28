#pragma once

#include "panel_theme.h"
#include "tls.h"

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <optional>
#include <string>
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
    PanelSettings settings() const;
    const nlohmann::json& initial_contents() const;
    std::string panel_base_path() const;
    bool save_panel_base_path(std::string_view previous, std::string_view path);
    nlohmann::json save_tls_settings(const std::optional<TlsSettings>& tls,
                                     std::unique_ptr<httplib::Server>* activation = nullptr);
    void begin_listener_restart();
    void finish_listener_restart(std::optional<PanelSettings> settings);
    std::unique_ptr<httplib::Server> make_listener(const PanelSettings& settings) const;
    int session_expiry_days() const;
    void save_session_expiry_days(int days);
    void save_theme(theme::Preset preset);
    void save_donation(std::string_view answer, std::string_view answered_at);
    bool claim_waterwall_install();
    void save_waterwall_version(std::string_view version);
    void save_auth(const nlohmann::json& admins, const nlohmann::json& auth);
    void save_core_settings(const nlohmann::json& settings);
    void save_core_configs(const nlohmann::json& configs);
    nlohmann::json capture_panel_backup() const;
    void validate_panel_backup(const nlohmann::json& panel) const;
    PanelSettings panel_backup_listener_settings(const nlohmann::json& panel) const;
    std::string panel_backup_tls_error(const nlohmann::json& panel) const;
    void save_workspace_restore(const nlohmann::json& manifest, const nlohmann::json& restore_id,
                                const nlohmann::json& auth_state);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wave
