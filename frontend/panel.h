#pragma once

#include "views/bot_view.h"
#include "views/console_view.h"
#include "views/entry_view.h"
#include "views/logs_view.h"
#include "views/settings_view.h"
#include "views/topology_view.h"
#include "views/users_view.h"

#include <string>

namespace wave {

struct OverviewData;
class PreviewSession;

enum class PanelPage { users, server, settings, console, topology, bot, about, overview };
enum class PanelDialog { none, sign_out, version, diagnostics, input };

struct PanelState {
    PanelPage page = PanelPage::users;
    EntryViewState entry;
    UsersViewState users;
    LogsViewState logs;
    TopologyViewState topology;
    SettingsViewState settings;
    ConsoleViewState console;
    BotViewState bot;
    bool navigation_open = true;
    bool menu_open = false;
    bool tools_open = false;
    bool high_contrast = false;
    bool metrics_open = false;
    int browser_control = 0;
    PanelDialog requested_dialog = PanelDialog::none;
    char input_sample[257]{};
};

const char* panel_page_name(PanelPage page);
int panel_navigation_index(PanelPage page);
float panel_height(float viewport_width, const PanelState& state);
void configure_panel_style(bool high_contrast = false);
void render_panel(PanelState& state, PreviewSession& session, const OverviewData* overview, bool overview_loading,
                  int browser_focus);
std::string describe_panel(const PanelState& state, const PreviewSession& session,
                           const OverviewData* overview, bool overview_loading);

} // namespace wave
