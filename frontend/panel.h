#pragma once

#include "entry_view.h"
#include "logs_view.h"
#include "secondary_views.h"
#include "users_view.h"

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
    SecondaryViewsState secondary;
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
