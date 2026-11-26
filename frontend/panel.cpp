#include "panel.h"

#include "browser_ui.h"
#include "dialog.h"
#include "preview_session.h"
#include "theme.h"
#include "views/about_view.h"
#include "views/server_status_view.h"
#include "views/view_helpers.h"
#include "wave_version.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace wave {
namespace {

using theme::Color;
using theme::packed;

constexpr std::array navigation{
    PanelNavigationItem{"Server Status", PanelPage::server},
    PanelNavigationItem{"Users", PanelPage::users},
    PanelNavigationItem{"Settings", PanelPage::settings},
    PanelNavigationItem{"Console", PanelPage::console},
    PanelNavigationItem{"Nodes & Routers", PanelPage::topology},
    PanelNavigationItem{"About", PanelPage::about},
};

constexpr float margin = 10;
constexpr float sidebar_width = 178;
constexpr float wide_sidebar_width = 224;
constexpr float desktop_header = 48;
constexpr float narrow_header = 78;
constexpr float breakpoint = 760;
constexpr float nav_row = 34;
constexpr float wide_nav_row = 38;
constexpr float header_button_height = 34;
constexpr int narrow_navigation_columns = 2;
constexpr auto narrow_navigation_rows =
    (navigation.size() + narrow_navigation_columns - 1) / narrow_navigation_columns;
constexpr float narrow_navigation_height = narrow_navigation_rows * nav_row + 2 * margin;
constexpr float narrow_work_height = 660;
constexpr float narrow_logs_height = 232;
constexpr float desktop_logs_min_height = 195;
constexpr float server_min_work_height = 120;

struct PanelLayout {
    bool narrow;
    bool sidebar;
    bool wide;
    float navigation_width;
    float header_height;
    ImVec2 workspace;

    PanelLayout(float width, const PanelState& state)
        : narrow(width < breakpoint),
          sidebar(state.navigation_open && state.entry.page == EntryPage::workspace),
          wide(width >= theme::workspace_font_breakpoint),
          navigation_width(wide ? wide_sidebar_width : sidebar_width),
          header_height(narrow ? narrow_header : desktop_header),
          workspace{margin + (sidebar && !narrow ? navigation_width : 0),
                    header_height + margin + (sidebar && narrow ? narrow_navigation_height : 0)}
    {
    }

    float work_width(float canvas_width) const
    {
        return std::max(1.0f, canvas_width - workspace.x - margin);
    }

    bool stack_server_status(const PanelState& state, float canvas_width) const
    {
        const float font = wide ? theme::workspace_font_size : theme::secondary_font_size;
        // Reserve the pane scrollbar when testing fit so short desktops keep the same columns.
        const float content_width =
            work_width(canvas_width) - 2 * theme::window_padding.x - theme::scrollbar_size;
        return state.page == PanelPage::server && !server_status_has_columns(content_width, font);
    }
};

bool begin_pane(const char* name, ImVec2 size, Color surface, ImGuiWindowFlags flags = 0,
                ImGuiChildFlags child_flags = 0)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::rgba(surface));
    ImGui::PushStyleColor(ImGuiCol_Border, theme::rgba(Color::divider));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::pane_radius);
    const bool visible = ImGui::BeginChild(name, size, ImGuiChildFlags_Borders | child_flags, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    return visible;
}

float draw_header(const PanelLayout& layout, ImVec2 viewport, bool in_workspace)
{
    auto& draw = *ImGui::GetWindowDrawList();
    draw.AddRectFilled({0, 0}, {viewport.x, layout.header_height}, packed(Color::header));
    draw.AddRectFilled({0, 0}, {viewport.x, theme::selection_width}, packed(Color::accent));
    draw.AddRectFilled({0, layout.header_height - theme::border_width}, {viewport.x, layout.header_height},
                       packed(Color::divider));
    draw.AddRectFilled({12, 10}, {40, 38}, packed(Color::raised), theme::control_radius);
    constexpr ImVec2 wave[] = {{17, 26}, {22, 21}, {25, 21}, {29, 27}, {31, 27}, {35, 22}};
    draw.AddPolyline(wave, IM_ARRAYSIZE(wave), packed(Color::accent), 2);
    const float title_size = layout.wide ? theme::title_font_size : theme::workspace_font_size;
    constexpr float title_x = 47;
    ImGui::PushFont(nullptr, title_size);
    draw.AddText({title_x, std::floor((desktop_header - title_size) / 2)}, packed(Color::primary_text),
                 "Wave");
    const float menu_x = title_x + ImGui::CalcTextSize("Wave").x + 8;
    ImGui::PopFont();

    if (!in_workspace) {
        return menu_x;
    }
    constexpr char status[] = "WaterWall disconnected";
    constexpr char preview[] = "Preview";
    float status_size = theme::secondary_font_size;
    ImGui::PushFont(nullptr, status_size);
    float preview_width = ImGui::CalcTextSize(preview).x;
    const float status_width = ImGui::CalcTextSize(status).x;
    if (layout.narrow && preview_width + status_width + 46 > viewport.x) {
        status_size = theme::annotation_font_size;
        ImGui::PopFont();
        ImGui::PushFont(nullptr, status_size);
        preview_width = ImGui::CalcTextSize(preview).x;
    }
    const float status_x = layout.narrow ? 12 + preview_width + 22 : viewport.x - 24 - status_width;
    const float preview_x = layout.narrow ? 12 : status_x - 22 - preview_width;
    const float center_y = layout.narrow ? 64 : desktop_header / 2;
    const float text_y = std::floor(center_y - status_size / 2);
    draw.AddCircleFilled({status_x - 9, center_y}, 3, packed(Color::warning));
    draw.AddText({status_x, text_y}, packed(Color::secondary_text), status);
    draw.AddText({preview_x, text_y}, packed(Color::accent), preview);
    ImGui::PopFont();
    return menu_x;
}

struct HeaderPopup {
    const char* label;
    const char* name;
    const char* longest_item;
    int control;
};

constexpr HeaderPopup menu{"Menu", "Wave menu", "Version information", panel_menu_control};
constexpr HeaderPopup tools{"Tools", "Wave tools", "Connection diagnostics", panel_tools_control};
constexpr char imgui_demo_title[] = "Dear ImGui Demo";
constexpr char implot_demo_title[] = "ImPlot Demo";

struct HeaderTrigger {
    ImVec2 anchor;
    bool close;
};

HeaderTrigger draw_header_trigger(const HeaderPopup& popup, const PanelState& state, int focus,
                                  bool header_popup_open)
{
    const bool open = ImGui::IsPopupOpen(popup.name);
    ImGui::PushStyleColor(ImGuiCol_Button, theme::rgba(open ? Color::selected : Color::header));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool pressed = ImGui::Button(popup.label, {0, header_button_height});
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (open) {
        const float inset = ImGui::GetStyle().FramePadding.x;
        ImGui::GetWindowDrawList()->AddLineH(min.x + inset, max.x - inset, max.y - 3, packed(Color::accent),
                                             theme::selection_width);
    }
    if (focus == popup.control) {
        ImGui::GetWindowDrawList()->AddRect({min.x - 1, min.y - 1}, {max.x + 1, max.y + 1},
                                            packed(Color::focus), theme::control_radius, theme::focus_width);
    }

    // When a header popup owns hover, pressing either trigger should still toggle or switch it.
    const bool popup_click =
        header_popup_open && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_NoNavOverride);
    const bool activate = pressed || popup_click || state.browser_control == popup.control;
    if (activate && !open) {
        ImGui::OpenPopup(popup.name);
    }
    return {{min.x, max.y + 1}, activate && open};
}

bool begin_header_popup(const HeaderPopup& popup, HeaderTrigger trigger)
{
    if (!ImGui::IsPopupOpen(popup.name)) {
        return false;
    }
    const auto& style = ImGui::GetStyle();
    const float display_width = ImGui::GetIO().DisplaySize.x;
    const float safe_x = style.DisplaySafeAreaPadding.x;
    const float content_width = ImGui::CalcTextSize(popup.longest_item).x + ImGui::GetFontSize() * 1.2f +
                                style.ItemInnerSpacing.x + 2 * style.WindowPadding.x + 2;
    const float width = std::min(std::ceil(content_width), std::max(1.0f, display_width - 2 * safe_x));
    trigger.anchor.x = std::clamp(trigger.anchor.x, safe_x, std::max(safe_x, display_width - width - safe_x));
    ImGui::SetNextWindowPos(trigger.anchor);
    ImGui::SetNextWindowSize({width, 0});
    if (!ImGui::BeginPopup(popup.name)) {
        return false;
    }
    if (trigger.close) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return false;
    }
    return true;
}

void render_menus(PanelState& state, float x, int focus, PreviewSession& session)
{
    ImGui::PushFont(nullptr, theme::secondary_font_size);
    ImGui::SetCursorScreenPos({x, (desktop_header - header_button_height) / 2});
    const bool header_popup_open = ImGui::IsPopupOpen(menu.name) || ImGui::IsPopupOpen(tools.name);
    const auto menu_trigger = draw_header_trigger(menu, state, focus, header_popup_open);
    ImGui::SameLine(0, 2);
    const auto tools_trigger = draw_header_trigger(tools, state, focus, header_popup_open);
    if (begin_header_popup(menu, menu_trigger)) {
        if (ImGui::MenuItem("Show sidebar", nullptr, state.navigation_open)) {
            state.navigation_open = !state.navigation_open;
        }
        if (ImGui::MenuItem("High contrast", nullptr, state.high_contrast)) {
            state.high_contrast = !state.high_contrast;
        }
        if (ImGui::MenuItem("Version information")) {
            state.requested_dialog = PanelDialog::version;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Sign out")) {
            state.auth.clear_error();
            state.requested_dialog = PanelDialog::sign_out;
        }
        ImGui::Separator();
        render_panel_config_menu(state.panel_controls, session);
        ImGui::EndPopup();
    }

    if (begin_header_popup(tools, tools_trigger)) {
        if (ImGui::MenuItem("Connection diagnostics")) {
            state.requested_dialog = PanelDialog::diagnostics;
        }
        ImGui::MenuItem("Renderer metrics", nullptr, &state.metrics_open);
        if (ImGui::MenuItem("Mini keyboard")) {
            state.requested_dialog = PanelDialog::input;
        }
        ImGui::Separator();
        ImGui::MenuItem("ImGui Demo", nullptr, &state.imgui_demo_open);
        ImGui::MenuItem("ImPlot Demo", nullptr, &state.implot_demo_open);
        ImGui::EndPopup();
    }
    state.menu_open = ImGui::IsPopupOpen(menu.name);
    state.tools_open = ImGui::IsPopupOpen(tools.name);
    ImGui::PopFont();
}

void render_navigation(PanelState& state, const PanelLayout& layout, ImVec2 viewport)
{
    if (!layout.sidebar) {
        return;
    }
    const int columns = layout.narrow ? narrow_navigation_columns : 1;
    const float width = layout.narrow ? viewport.x - 2 * margin : layout.navigation_width - margin;
    const float row_height = layout.wide ? wide_nav_row : nav_row;
    const float item_width = std::floor((width - (columns + 1) * 6) / columns);
    ImGui::SetCursorScreenPos({margin, layout.header_height + margin});
    const float height =
        layout.narrow ? narrow_navigation_height - margin : viewport.y - layout.header_height - 2 * margin;
    // Share the workspace focus scope so Tab can reach its controls after a sidebar click.
    if (begin_pane("Navigation", {width, height}, Color::pane,
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
                   ImGuiChildFlags_NavFlattened)) {
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0.0f, 0.5f});
        for (std::size_t i = 0; i < navigation.size(); ++i) {
            const int row = static_cast<int>(i) / columns;
            const int column = static_cast<int>(i) % columns;
            ImGui::SetCursorPos({7 + column * (item_width + 5), 5 + row * row_height});
            const auto& item = navigation[i];
            const bool selected = state.page == item.page;
            const bool smaller_label = layout.narrow && ImGui::CalcTextSize(item.label).x > item_width - 4;
            if (smaller_label) {
                ImGui::PushFont(nullptr, theme::annotation_font_size);
            }
            const bool activate =
                ImGui::Selectable(item.label, selected, ImGuiSelectableFlags_NoAutoClosePopups,
                                  {item_width - 4, row_height - 5});
            if (smaller_label) {
                ImGui::PopFont();
            }
            if (activate) {
                state.page = item.page;
            }
            if (selected) {
                const auto min = ImGui::GetItemRectMin();
                const auto max = ImGui::GetItemRectMax();
                const float right = std::ceil(max.x) + 1;
                ImGui::GetWindowDrawList()->AddRectFilled(
                    {right - theme::selection_width, std::ceil(min.y) + 2}, {right, std::floor(max.y) - 2},
                    packed(Color::accent));
            }
        }
        ImGui::PopStyleVar();
        ImGui::PopItemFlag();
    }
    ImGui::EndChild();
}

std::string_view displayed_panel_version(const PreviewSession& session)
{
    return view::string_field(view::field(session.data(), "server"), "version", wave::version);
}

const char* dialog_title(PanelDialog dialog)
{
    switch (dialog) {
    case PanelDialog::sign_out:
        return "Sign out of Wave";
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
    constexpr float key_gap = 4;
    const float key_width = std::max(1.0f, std::floor((ImGui::GetContentRegionAvail().x - 9 * key_gap) / 10));
    const auto append = [&state](char c) {
        const auto used = std::strlen(state.input_sample);
        if (used + 1 < sizeof(state.input_sample)) {
            state.input_sample[used] = c;
            state.input_sample[used + 1] = 0;
        }
    };
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {3, ImGui::GetStyle().FramePadding.y});
    for (const std::string_view row : {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"}) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i != 0) {
                ImGui::SameLine(0, key_gap);
            }
            const char label[]{row[i], 0};
            if (ImGui::Button(label, {key_width, 29})) {
                append(row[i]);
            }
        }
    }
    ImGui::PopStyleVar();
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

void render_demo(const char* title, void (*show)(bool*), bool& open, const CanvasViewport& viewport)
{
    if (!open) {
        return;
    }
    const auto* window = ImGui::FindWindowByName(title);
    // The ImPlot demo can also open Dear ImGui's demo through its own Tools menu.
    if (!window || !window->Active) {
        show(&open);
        window = ImGui::FindWindowByName(title);
    }
    if (!window || !window->Appearing) {
        return;
    }

    const ImVec2 min{viewport.left + margin, std::max(viewport.top, desktop_header) + margin};
    const ImVec2 max{viewport.left + viewport.width - margin, viewport.top + viewport.height - margin};
    const ImVec2 size{std::min(window->SizeFull.x, std::max(1.0f, max.x - min.x)),
                      std::min(window->SizeFull.y, std::max(1.0f, max.y - min.y))};
    ImGui::SetWindowSize(title, size);
    ImGui::SetWindowPos(title, {std::clamp(window->Pos.x, min.x, std::max(min.x, max.x - size.x)),
                                std::clamp(window->Pos.y, min.y, std::max(min.y, max.y - size.y))});
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
        const bool sign_out = dialog == PanelDialog::sign_out;
        const bool diagnostics = dialog == PanelDialog::diagnostics;
        const float width = sign_out ? 350 : (diagnostics || dialog == PanelDialog::input ? 440 : 365);
        const bool pending = sign_out && state.auth.pending();
        const std::array actions = {DialogAction{sign_out ? "Cancel" : "Close", pending},
                                    DialogAction{sign_out ? "Sign out" : "Refresh sample data", pending}};
        const auto count = sign_out || diagnostics ? 2 : 1;
        Dialog frame(title, width, std::span(actions).first(count));
        if (!frame) {
            continue;
        }
        switch (dialog) {
        case PanelDialog::sign_out:
            ImGui::TextWrapped(pending ? "Ending this session…" : "End this session and return to sign-in?");
            if (!state.auth.error().empty()) {
                view::wrapped(state.auth.error(), Color::error);
            }
            break;
        case PanelDialog::version: {
            const auto version = displayed_panel_version(session);
            ImGui::Text("Wave %.*s", static_cast<int>(version.size()), version.data());
            ImGui::TextWrapped("Dear ImGui %s / WebAssembly / WebGL 2", IMGUI_VERSION);
            ImGui::TextLinkOpenURL("Project releases", "https://github.com/radkesvat/wave/releases");
            break;
        }
        case PanelDialog::diagnostics:
            ImGui::TextWrapped("Preview socket: %s", session.status() == PreviewStatus::ready ? "Connected"
                                                     : session.status() == PreviewStatus::loading
                                                         ? "Connecting"
                                                         : "Unavailable");
            ImGui::TextWrapped("WaterWall: Disconnected");
            ImGui::TextWrapped("Transport: HTTP and JSON WebSocket");
            ImGui::TextWrapped("Renderer: %.0f frames/s, %.2f× scale", ImGui::GetIO().Framerate,
                               ImGui::GetIO().DisplayFramebufferScale.x);
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
        const int action = frame.footer();
        if (action == 0) {
            ImGui::CloseCurrentPopup();
        } else if (action == 1 && sign_out) {
            state.auth.submit(AuthSession::Action::sign_out, nlohmann::json::object());
        } else if (action == 1 && diagnostics && !session.send({{"action", "refresh"}})) {
            session.start();
        }
    }
    if (state.metrics_open) {
        ImGui::SetNextWindowSize({std::min(520.0f, ImGui::GetIO().DisplaySize.x - 24), 350},
                                 ImGuiCond_Appearing);
        ImGui::ShowMetricsWindow(&state.metrics_open);
    }
    if (state.imgui_demo_open || state.implot_demo_open) {
        const auto viewport = visible_canvas();
        render_demo(implot_demo_title, ImPlot::ShowDemoWindow, state.implot_demo_open, viewport);
        render_demo(imgui_demo_title, ImGui::ShowDemoWindow, state.imgui_demo_open, viewport);
    }
}

void render_view(PanelState& state, PreviewSession& session, bool narrow, bool stacked_status)
{
    const bool has_server = state.page == PanelPage::server && session.data().contains("server");
    if (!session.ready() && !has_server && state.page != PanelPage::about &&
        state.page != PanelPage::settings && state.page != PanelPage::console) {
        ImGui::TextUnformatted(panel_page_name(state.page));
        ImGui::Separator();
        if (session.status() == PreviewStatus::loading) {
            ImGui::TextColored(theme::rgba(Color::secondary_text), "Loading sample data…");
        } else {
            ImGui::TextColored(theme::rgba(Color::warning), "Sample data unavailable.");
            if (theme::button("Reconnect")) {
                session.start();
            }
        }
        if (state.page == PanelPage::users && state.users.dialog) {
            render_users_dialog(state.users, session);
        } else if (state.page == PanelPage::topology) {
            render_topology_dialogs(state.topology, session);
        }
        return;
    }
    switch (state.page) {
    case PanelPage::users:
        render_users(state.users, session);
        break;
    case PanelPage::server: {
        const float height =
            render_server_status(session.data()["server"], state.panel_controls, session, stacked_status);
        if (stacked_status) {
            state.stacked_server_status_height = height + 2 * theme::window_padding.y;
        }
        break;
    }
    case PanelPage::settings:
        render_settings(state.settings, state.auth, session, narrow);
        break;
    case PanelPage::console:
        state.console = render_console();
        break;
    case PanelPage::topology:
        render_topology(state.topology, session, narrow);
        break;
    case PanelPage::about:
        render_about(displayed_panel_version(session));
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

bool panel_demo_at(const PanelState& state, float x, float y)
{
    const auto contains = [x, y](const char* title) {
        const auto* window = ImGui::FindWindowByName(title);
        return window && window->Active && window->Rect().Contains({x, y});
    };
    return (state.imgui_demo_open && contains(imgui_demo_title)) ||
           (state.implot_demo_open && (contains(implot_demo_title) || contains(imgui_demo_title)));
}

float panel_height(float viewport_width, float canvas_width, const PanelState& state)
{
    const PanelLayout layout(viewport_width, state);
    if (state.entry.page != EntryPage::workspace) {
        return layout.header_height;
    }
    if (layout.stack_server_status(state, canvas_width)) {
        return layout.workspace.y + std::max(narrow_work_height, state.stacked_server_status_height) +
               narrow_logs_height + 2 * margin;
    }
    if (layout.narrow) {
        return layout.workspace.y + narrow_work_height + narrow_logs_height + 2 * margin;
    }
    return state.page == PanelPage::server
               ? layout.workspace.y + server_min_work_height + desktop_logs_min_height + 2 * margin
               : 666;
}

void render_panel(PanelState& state, PreviewSession& session, float viewport_width, int browser_focus)
{
    state.console = {};
    state.topology.visible_bounds = {};
    state.settings.core.scroll_bounds = {};
    const bool in_workspace = state.entry.page == EntryPage::workspace;
    if (in_workspace) {
        update_panel_controls(state.panel_controls, session);
    }
    const ImVec2 viewport = ImGui::GetIO().DisplaySize;
    if (state.browser_control && state.navigation_open && in_workspace) {
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

    PanelLayout layout(viewport_width, state);
    const float menu_x = draw_header(layout, viewport, in_workspace);
    if (in_workspace) {
        render_menus(state, menu_x, browser_focus, session);
    }
    state.browser_control = 0;
    layout = PanelLayout(viewport_width, state);
    if (!in_workspace) {
        render_entry(state.entry, state.auth, layout.header_height);
    } else {
        render_navigation(state, layout, viewport);
        const float width = layout.work_width(viewport.x);
        const bool stacked_status = layout.stack_server_status(state, viewport.x);
        const float logs_height =
            layout.narrow || stacked_status
                ? narrow_logs_height
                : std::floor(std::clamp(viewport.y * 0.25f, desktop_logs_min_height, 244.0f));
        const float work_height =
            stacked_status  ? std::max(narrow_work_height, state.stacked_server_status_height)
            : layout.narrow ? narrow_work_height
                            : std::max(1.0f, viewport.y - layout.workspace.y - logs_height - 2 * margin);
        ImGui::PushID(static_cast<int>(state.page));
        ImGui::SetCursorScreenPos(layout.workspace);
        const auto view_flags =
            stacked_status ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0;
        if (begin_pane("View", {width, work_height}, Color::canvas, view_flags)) {
            render_view(state, session, layout.narrow, stacked_status);
        }
        ImGui::EndChild();
        ImGui::PopID();

        ImGui::SetCursorScreenPos({layout.workspace.x, layout.workspace.y + work_height + margin});
        if (begin_pane("Logs pane", {width, logs_height}, Color::pane)) {
            render_logs(state.logs, session, layout.narrow);
        }
        ImGui::EndChild();
        render_panel_control_dialog(state.panel_controls, session);
        render_dialogs(state, session);
    }
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        state.console = {};
        state.topology.visible_bounds = {};
    }
    if (state.metrics_open || state.imgui_demo_open || state.implot_demo_open) {
        state.console = {};
    }
    ImGui::End();
}

std::string describe_panel(const PanelState& state, const PreviewSession& session)
{
    if (state.entry.page != EntryPage::workspace) {
        return describe_entry(state.entry);
    }
    if (state.page == PanelPage::console) {
        return "Wave Console. WaterWall terminal.";
    }
    auto description = std::format("Wave {}. Preview. WaterWall disconnected.", panel_page_name(state.page));
    if (state.page == PanelPage::settings) {
        description += ' ' + describe_settings(state.settings, state.auth);
    }
    const bool controls_dialog = state.panel_controls.dialog != PanelControlsState::Dialog::none;
    const bool has_server = state.page == PanelPage::server && session.data().contains("server");
    if (!session.ready() && !has_server) {
        auto result =
            description + (session.status() == PreviewStatus::loading ? " Loading sample data."
                                                                      : " Sample data unavailable.");
        if (controls_dialog) {
            result += ' ' + describe_panel_controls(state.panel_controls, session, false);
        }
        return result;
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
    const auto action = session.feedback_action();
    const bool core_settings_feedback = state.page == PanelPage::settings && action == "apply_core_settings";
    if (!session.feedback().empty() && !core_settings_feedback &&
        ((!has_server && !controls_dialog) ||
         (action != "update_panel" && action != "restore_preview_backup" &&
          action.find("_core") == std::string_view::npos))) {
        result += " ";
        result += session.feedback();
    }
    if (state.page == PanelPage::server) {
        result += ' ' + describe_server_status(session.data()["server"]);
    } else if (state.page == PanelPage::topology) {
        result += ' ' + describe_topology(state.topology, session.data()["topology"]);
    }
    if (has_server || controls_dialog) {
        result += ' ' + describe_panel_controls(state.panel_controls, session, has_server);
    }
    return result;
}

} // namespace wave
