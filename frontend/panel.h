#pragma once

#include "auth_session.h"
#include "views/console_view.h"
#include "views/entry_view.h"
#include "views/logs_view.h"
#include "views/panel_controls_view.h"
#include "views/settings_view.h"
#include "views/topology_view.h"
#include "views/users_view.h"

#include <span>
#include <string>

namespace wave {

class PreviewSession;

enum class PanelPage {
    users = 100,
    server = 101,
    settings = 102,
    console = 103,
    topology = 104,
    about = 106,
};
enum class PanelDialog {
    none,
    sign_out,
    version,
    diagnostics,
    input
};

inline constexpr int panel_menu_control = 1;
inline constexpr int panel_tools_control = 2;

struct PanelNavigationItem {
    const char* label;
    PanelPage page;
};

struct PanelState {
    PanelPage page = PanelPage::users;
    AuthSession auth;
    EntryViewState entry;
    UsersViewState users;
    LogsViewState logs;
    TopologyViewState topology;
    SettingsViewState settings;
    CanvasViewport console{};
    PanelControlsState panel_controls;
    float stacked_server_status_height = 0;
    bool navigation_open = true;
    bool menu_open = false;
    bool tools_open = false;
    bool high_contrast = false;
    bool metrics_open = false;
    bool imgui_demo_open = false;
    bool implot_demo_open = false;
    int browser_control = 0;
    PanelDialog requested_dialog = PanelDialog::none;
    char input_sample[257]{};
};

const char* panel_page_name(PanelPage page);
std::span<const PanelNavigationItem> panel_navigation();
float panel_height(float viewport_width, float canvas_width, const PanelState& state);
void render_panel(PanelState& state, PreviewSession& session, float viewport_width, int browser_focus);
bool panel_demo_at(const PanelState& state, float x, float y);
std::string describe_panel(const PanelState& state, const PreviewSession& session);

} // namespace wave
