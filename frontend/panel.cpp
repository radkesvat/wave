#include "panel.h"

#include "browser_ui.h"
#include "preview_session.h"
#include "views/about_view.h"
#include "views/overview.h"
#include "views/server_status_view.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace wave {
namespace {

constexpr std::array navigation{
    PanelNavigationItem{"Server Status", PanelPage::server},
    PanelNavigationItem{"Users", PanelPage::users},
    PanelNavigationItem{"Panel Settings", PanelPage::settings},
    PanelNavigationItem{"Console", PanelPage::console},
    PanelNavigationItem{"Nodes & Routers", PanelPage::topology},
    PanelNavigationItem{"Telegram Bot", PanelPage::bot},
    PanelNavigationItem{"About", PanelPage::about},
    PanelNavigationItem{"Overview", PanelPage::overview},
};

constexpr ImU32 background = IM_COL32(11, 20, 30, 255);
constexpr ImU32 chrome = IM_COL32(17, 31, 43, 255);
constexpr ImU32 border = IM_COL32(41, 62, 76, 255);
constexpr ImU32 accent = IM_COL32(104, 229, 195, 255);
constexpr ImU32 accent_surface = IM_COL32(23, 53, 56, 255);
constexpr ImU32 foreground = IM_COL32(234, 243, 247, 255);
constexpr ImU32 muted = IM_COL32(173, 197, 211, 255);
constexpr ImU32 amber = IM_COL32(246, 192, 119, 255);
constexpr float margin = 10;
constexpr float sidebar_width = 178;
constexpr float desktop_header = 48;
constexpr float narrow_header = 78;
constexpr float breakpoint = 760;
constexpr float nav_row = 34;
constexpr int narrow_navigation_columns = 2;
constexpr auto narrow_navigation_rows =
    (navigation.size() + narrow_navigation_columns - 1) / narrow_navigation_columns;
constexpr float narrow_navigation_height = narrow_navigation_rows * nav_row + 2 * margin;
constexpr float narrow_work_height = 660;
constexpr float narrow_logs_height = 232;

struct PanelLayout {
    bool narrow;
    bool sidebar;
    float header_height;
    ImVec2 workspace;

    PanelLayout(float width, const PanelState& state)
        : narrow(width < breakpoint),
          sidebar(state.navigation_open && state.entry.page == EntryPage::workspace),
          header_height(narrow ? narrow_header : desktop_header),
          workspace{margin + (sidebar && !narrow ? sidebar_width : 0),
                    header_height + margin + (sidebar && narrow ? narrow_navigation_height : 0)}
    {
    }
};

void focus_outline()
{
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect({min.x - 1, min.y - 1}, {max.x + 1, max.y + 1}, accent, 6, 2);
}

void draw_header(const PanelLayout& layout, ImVec2 viewport)
{
    auto& draw = *ImGui::GetWindowDrawList();
    draw.AddRectFilled({0, 0}, {viewport.x, layout.header_height}, chrome);
    draw.AddLineH(0, viewport.x, layout.header_height - 1, border);
    draw.AddRectFilled({12, 10}, {40, 38}, accent_surface, 7);
    constexpr ImVec2 wave[] = {{17, 26}, {22, 21}, {25, 21}, {29, 27}, {31, 27}, {35, 22}};
    draw.AddPolyline(wave, IM_ARRAYSIZE(wave), accent, 2);
    draw.AddText(ImGui::GetFont(), 21, {47, 13}, foreground, "Wave");

    constexpr char status[] = "WaterWall disconnected";
    const float x = layout.narrow ? 78 : viewport.x - 176;
    const float y = layout.narrow ? 55 : 16;
    draw.AddCircleFilled({x, y + 8}, 3, amber);
    draw.AddText(ImGui::GetFont(), 15, {x + 8, y}, muted, status);
    const float preview_x = layout.narrow ? 12 : x - 75;
    draw.AddText(ImGui::GetFont(), 15, {preview_x, y}, accent, "Preview");
}

void open_workspace_entry(PanelState& state, EntryPage page, PreviewSession& session)
{
    open_entry(state.entry, page, session);
    if (page == EntryPage::workspace) {
        state.page = PanelPage::users;
    }
}

void render_menus(PanelState& state, int focus, PreviewSession& session)
{
    ImGui::SetCursorScreenPos({104, 9});
    if (ImGui::Button("Menu", {55, 30}) || state.browser_control == panel_menu_control) {
        ImGui::OpenPopup("Wave menu");
    }
    if (focus == panel_menu_control) {
        focus_outline();
    }
    ImGui::SetNextWindowPos({104, 42}, ImGuiCond_Appearing);
    state.menu_open = ImGui::BeginPopup("Wave menu");
    if (state.menu_open) {
        if (ImGui::MenuItem("Show sidebar", nullptr, state.navigation_open,
                            state.entry.page == EntryPage::workspace)) {
            state.navigation_open = !state.navigation_open;
        }
        if (ImGui::MenuItem("High contrast", nullptr, state.high_contrast)) {
            state.high_contrast = !state.high_contrast;
            configure_panel_style(state.high_contrast);
        }
        if (ImGui::MenuItem("Version information")) {
            state.requested_dialog = PanelDialog::version;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Demo sign-in")) {
            open_workspace_entry(state, EntryPage::sign_in, session);
        }
        if (ImGui::MenuItem("First-run setup")) {
            open_workspace_entry(state, EntryPage::setup, session);
        }
        if (ImGui::MenuItem("Demo sign-out")) {
            state.requested_dialog = PanelDialog::sign_out;
        }
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos({165, 9});
    if (ImGui::Button("Tools", {55, 30}) || state.browser_control == panel_tools_control) {
        ImGui::OpenPopup("Wave tools");
    }
    if (focus == panel_tools_control) {
        focus_outline();
    }
    ImGui::SetNextWindowPos({std::min(165.0f, ImGui::GetIO().DisplaySize.x - 195), 42}, ImGuiCond_Appearing);
    state.tools_open = ImGui::BeginPopup("Wave tools");
    if (state.tools_open) {
        if (ImGui::MenuItem("Connection diagnostics")) {
            state.requested_dialog = PanelDialog::diagnostics;
        }
        ImGui::MenuItem("Renderer metrics", nullptr, &state.metrics_open);
        if (ImGui::MenuItem("Mini keyboard")) {
            state.requested_dialog = PanelDialog::input;
        }
        ImGui::EndPopup();
    }
}

void render_navigation(PanelState& state, const PanelLayout& layout, ImVec2 viewport, int focus)
{
    if (!layout.sidebar) {
        return;
    }
    const int columns = layout.narrow ? narrow_navigation_columns : 1;
    const float width = layout.narrow ? viewport.x - 2 * margin : sidebar_width - margin;
    const float item_width = (width - (columns + 1) * 6) / columns;
    ImGui::SetCursorScreenPos({margin, layout.header_height + margin});
    const float height =
        layout.narrow ? narrow_navigation_height - margin : viewport.y - layout.header_height - 2 * margin;
    if (ImGui::BeginChild("Navigation", {width, height}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::SetCursorPosY(5);
        for (std::size_t i = 0; i < navigation.size(); ++i) {
            const int row = static_cast<int>(i) / columns;
            const int column = static_cast<int>(i) % columns;
            ImGui::SetCursorPos({7 + column * (item_width + 5), 5 + row * nav_row});
            const auto& item = navigation[i];
            if (ImGui::Selectable(item.label, state.page == item.page, ImGuiSelectableFlags_NoAutoClosePopups,
                                  {item_width - 4, 29})) {
                state.page = item.page;
            }
            if (focus == static_cast<int>(item.page)) {
                focus_outline();
            }
        }
    }
    ImGui::EndChild();
}

const char* dialog_title(PanelDialog dialog)
{
    switch (dialog) {
    case PanelDialog::sign_out:
        return "Demo sign-out";
    case PanelDialog::version:
        return "Wave version";
    case PanelDialog::diagnostics:
        return "Connection diagnostics";
    case PanelDialog::input:
        return "Mini keyboard";
    case PanelDialog::none:
        return "";
    }
    return "";
}

void mini_keyboard(PanelState& state)
{
    ImGui::TextWrapped("Compose text with the keys below, then copy it into a preview field.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##scratch", state.input_sample, sizeof(state.input_sample));
    const float key_width = (ImGui::GetContentRegionAvail().x - 9 * ImGui::GetStyle().ItemSpacing.x) / 10;
    const auto append = [&state](char c) {
        const auto used = std::strlen(state.input_sample);
        if (used + 1 < sizeof(state.input_sample)) {
            state.input_sample[used] = c;
            state.input_sample[used + 1] = 0;
        }
    };
    for (const std::string_view row : {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"}) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i != 0) {
                ImGui::SameLine();
            }
            const char label[]{row[i], 0};
            if (ImGui::Button(label, {key_width, 29})) {
                append(row[i]);
            }
        }
    }
    if (ImGui::Button("Space")) {
        append(' ');
    }
    ImGui::SameLine();
    if (ImGui::Button("Backspace")) {
        const auto used = std::strlen(state.input_sample);
        if (used > 0) {
            std::size_t start = used - 1;
            while (start > 0 && (static_cast<unsigned char>(state.input_sample[start]) & 0xc0) == 0x80) {
                --start;
            }
            state.input_sample[start] = 0;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        ImGui::SetClipboardText(state.input_sample);
    }
}

void render_dialogs(PanelState& state, PreviewSession& session)
{
    if (state.requested_dialog != PanelDialog::none) {
        ImGui::OpenPopup(dialog_title(state.requested_dialog));
        state.requested_dialog = PanelDialog::none;
    }
    for (const auto dialog :
         {PanelDialog::sign_out, PanelDialog::version, PanelDialog::diagnostics, PanelDialog::input}) {
        const char* title = dialog_title(dialog);
        if (!ImGui::IsPopupOpen(title)) {
            continue;
        }
        fit_preview_dialog(dialog == PanelDialog::input ? 415 : 410);
        if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            continue;
        }
        switch (dialog) {
        case PanelDialog::sign_out:
            ImGui::TextWrapped("Return to the demo sign-in screen?");
            if (ImGui::Button("Sign out")) {
                session.send({{"action", "sign_out"}});
                open_workspace_entry(state, EntryPage::sign_in, session);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            break;
        case PanelDialog::version:
            ImGui::TextUnformatted("Wave 0.1.0 preview");
            ImGui::TextWrapped("Dear ImGui %s / WebAssembly / WebGL 2", IMGUI_VERSION);
            ImGui::TextLinkOpenURL("Project releases", "https://github.com/radkesvat/wave/releases");
            break;
        case PanelDialog::diagnostics:
            ImGui::Text("Preview socket: %s", session.status() == PreviewStatus::ready     ? "Connected"
                                              : session.status() == PreviewStatus::loading ? "Connecting"
                                                                                           : "Unavailable");
            ImGui::TextUnformatted("WaterWall: Disconnected");
            ImGui::TextUnformatted("Transport: HTTP and JSON WebSocket");
            ImGui::Text("Renderer: %.0f frames/s, %.2f× scale", ImGui::GetIO().Framerate,
                        ImGui::GetIO().DisplayFramebufferScale.x);
            if (ImGui::Button("Refresh sample data")) {
                if (!session.send({{"action", "refresh"}})) {
                    session.start();
                }
            }
            if (session.feedback_action() == "refresh") {
                ImGui::TextWrapped("%.*s", static_cast<int>(session.feedback().size()),
                                   session.feedback().data());
            }
            break;
        case PanelDialog::input:
            mini_keyboard(state);
            break;
        case PanelDialog::none:
            break;
        }
        if (ImGui::Button(dialog == PanelDialog::sign_out ? "Cancel" : "Close")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (state.metrics_open) {
        ImGui::SetNextWindowSize({std::min(520.0f, ImGui::GetIO().DisplaySize.x - 24), 350},
                                 ImGuiCond_Appearing);
        ImGui::ShowMetricsWindow(&state.metrics_open);
    }
}

void render_view(PanelState& state, PreviewSession& session, bool narrow, const OverviewData* overview,
                 bool overview_loading)
{
    if (!session.ready() && state.page != PanelPage::about && state.page != PanelPage::overview) {
        ImGui::TextUnformatted(panel_page_name(state.page));
        ImGui::Separator();
        if (session.status() == PreviewStatus::loading) {
            ImGui::TextUnformatted("Loading sample data…");
        } else {
            ImGui::TextUnformatted("Sample data unavailable.");
            if (ImGui::Button("Reconnect")) {
                session.start();
            }
        }
        return;
    }
    switch (state.page) {
    case PanelPage::users:
        render_users(state.users, session, narrow);
        break;
    case PanelPage::server:
        render_server_status(session.data()["server"], narrow);
        break;
    case PanelPage::settings:
        render_settings(state.settings, session, narrow);
        break;
    case PanelPage::console:
        render_console(state.console, session, narrow);
        break;
    case PanelPage::topology:
        render_topology(state.topology, session.data()["topology"], narrow);
        break;
    case PanelPage::bot:
        render_bot(state.bot, session, narrow);
        break;
    case PanelPage::about:
        render_about();
        break;
    case PanelPage::overview:
        render_overview(overview, overview_loading);
        break;
    }
}

} // namespace

const char* panel_page_name(PanelPage page)
{
    return std::ranges::find(navigation, page, &PanelNavigationItem::page)->label;
}

std::span<const PanelNavigationItem> panel_navigation()
{
    return navigation;
}

float panel_height(float viewport_width, const PanelState& state)
{
    const PanelLayout layout(viewport_width, state);
    if (state.entry.page != EntryPage::workspace) {
        return layout.header_height + 470;
    }
    if (layout.narrow) {
        return layout.workspace.y + narrow_work_height + narrow_logs_height + 2 * margin;
    }
    return 666;
}

void configure_panel_style(bool high_contrast)
{
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {11, 9};
    style.FramePadding = {8, 5};
    style.ItemSpacing = {7, 7};
    style.CellPadding = {7, 5};
    style.FrameRounding = 5;
    style.ChildRounding = 7;
    style.PopupRounding = 7;
    style.ScrollbarRounding = 6;
    style.ScrollbarSize = 13;
    auto* colors = style.Colors;
    const auto color = [](ImU32 value) { return ImGui::ColorConvertU32ToFloat4(value); };
    colors[ImGuiCol_Text] = color(foreground);
    colors[ImGuiCol_TextDisabled] = color(high_contrast ? foreground : muted);
    colors[ImGuiCol_WindowBg] = color(background);
    colors[ImGuiCol_ChildBg] = color(IM_COL32(17, 31, 43, 255));
    colors[ImGuiCol_PopupBg] = color(IM_COL32(18, 33, 46, 255));
    colors[ImGuiCol_Border] = color(high_contrast ? muted : border);
    colors[ImGuiCol_FrameBg] = color(IM_COL32(26, 47, 62, 255));
    colors[ImGuiCol_FrameBgHovered] = color(IM_COL32(37, 65, 77, 255));
    colors[ImGuiCol_FrameBgActive] = colors[ImGuiCol_FrameBgHovered];
    colors[ImGuiCol_Button] = color(IM_COL32(33, 55, 70, 255));
    colors[ImGuiCol_ButtonHovered] = color(IM_COL32(39, 79, 81, 255));
    colors[ImGuiCol_ButtonActive] = colors[ImGuiCol_ButtonHovered];
    colors[ImGuiCol_Header] = color(IM_COL32(30, 86, 79, 255));
    colors[ImGuiCol_HeaderHovered] = color(IM_COL32(37, 72, 75, 255));
    colors[ImGuiCol_HeaderActive] = colors[ImGuiCol_Header];
    colors[ImGuiCol_Tab] = colors[ImGuiCol_FrameBg];
    colors[ImGuiCol_TabHovered] = colors[ImGuiCol_ButtonHovered];
    colors[ImGuiCol_TabSelected] = colors[ImGuiCol_Header];
    colors[ImGuiCol_TabSelectedOverline] = color(accent);
    colors[ImGuiCol_CheckMark] = color(accent);
    colors[ImGuiCol_NavCursor] = color(accent);
    colors[ImGuiCol_PlotHistogram] = color(accent);
    colors[ImGuiCol_ScrollbarBg] = color(background);
    colors[ImGuiCol_ScrollbarGrab] = color(IM_COL32(53, 78, 93, 255));
    colors[ImGuiCol_TableHeaderBg] = color(IM_COL32(25, 47, 62, 255));
    colors[ImGuiCol_TableBorderStrong] = color(border);
    colors[ImGuiCol_TableBorderLight] = color(IM_COL32(28, 48, 62, 255));
    colors[ImGuiCol_TableRowBg] = color(IM_COL32(17, 32, 44, 255));
    colors[ImGuiCol_TableRowBgAlt] = color(IM_COL32(22, 40, 54, 255));
}

void render_panel(PanelState& state, PreviewSession& session, const OverviewData* overview,
                  bool overview_loading, int browser_focus)
{
    state.topology.visible_map = TopologyViewState::Tab::none;
    const ImVec2 viewport = ImGui::GetIO().DisplaySize;
    if (state.browser_control && state.navigation_open && state.entry.page == EntryPage::workspace) {
        const auto selected = std::ranges::find(navigation, static_cast<PanelPage>(state.browser_control),
                                                &PanelNavigationItem::page);
        if (selected != navigation.end()) {
            state.page = selected->page;
        }
    }

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(viewport);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("Wave workspace", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    PanelLayout layout(viewport.x, state);
    draw_header(layout, viewport);
    render_menus(state, browser_focus, session);
    state.browser_control = 0;
    layout = PanelLayout(viewport.x, state);
    if (state.entry.page != EntryPage::workspace) {
        const float width = std::max(1.0f, std::min(460.0f, viewport.x - 2 * margin));
        ImGui::SetCursorScreenPos({(viewport.x - width) * 0.5f, layout.workspace.y + 18});
        if (ImGui::BeginChild("Entry", {width, 408}, ImGuiChildFlags_Borders)) {
            render_entry(state.entry, session);
        }
        ImGui::EndChild();
    } else {
        render_navigation(state, layout, viewport, browser_focus);
        const float width = std::max(1.0f, viewport.x - layout.workspace.x - margin);
        const float logs_height =
            layout.narrow ? narrow_logs_height : std::clamp(viewport.y * 0.25f, 195.0f, 244.0f);
        const float work_height =
            layout.narrow ? narrow_work_height
                          : std::max(1.0f, viewport.y - layout.workspace.y - logs_height - 2 * margin);
        ImGui::PushID(static_cast<int>(state.page));
        ImGui::SetCursorScreenPos(layout.workspace);
        if (ImGui::BeginChild("View", {width, work_height}, ImGuiChildFlags_Borders)) {
            render_view(state, session, layout.narrow, overview, overview_loading);
        }
        ImGui::EndChild();
        ImGui::PopID();

        ImGui::SetCursorScreenPos({layout.workspace.x, layout.workspace.y + work_height + margin});
        if (ImGui::BeginChild("Logs pane", {width, logs_height}, ImGuiChildFlags_Borders)) {
            render_logs(state.logs, session, layout.narrow);
        }
        ImGui::EndChild();
    }
    render_dialogs(state, session);
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        state.topology.visible_map = TopologyViewState::Tab::none;
    }
    ImGui::End();
}

std::string describe_panel(const PanelState& state, const PreviewSession& session,
                           const OverviewData* overview, bool overview_loading)
{
    if (state.entry.page == EntryPage::sign_in) {
        return std::format("Wave demo sign-in. {}", state.entry.message);
    }
    if (state.entry.page == EntryPage::setup) {
        return std::format("Wave first-run setup. Step {} of 3. {}", state.entry.step + 1,
                           state.entry.message);
    }
    if (state.page == PanelPage::overview) {
        return describe_overview(overview, overview_loading);
    }
    const auto description =
        std::format("Wave {}. Preview. WaterWall disconnected.", panel_page_name(state.page));
    if (!session.ready()) {
        return description + (session.status() == PreviewStatus::loading ? " Loading sample data."
                                                                         : " Sample data unavailable.");
    }

    const auto& logs = session.data()["logs"];
    std::string result = std::format("{} Sample data. {} users. {} log entries.", description,
                                     session.data()["users"].size(), logs.size());
    if (!logs.empty() && logs.back().is_object()) {
        const auto message = logs.back().find("message");
        if (message != logs.back().end() && message->is_string()) {
            result += " Latest log: " + message->get_ref<const std::string&>();
        }
    }
    if (!session.feedback().empty()) {
        result += " ";
        result += session.feedback();
    }
    return result;
}

} // namespace wave
