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
#include <limits>

namespace wave {
namespace {

using theme::Color;
using theme::packed;

constexpr std::array navigation{
    PanelNavigationItem{"Server Status", PanelPage::server},
    PanelNavigationItem{"Console", PanelPage::console},
    PanelNavigationItem{"Settings", PanelPage::settings},
    PanelNavigationItem{"About", PanelPage::about},
};
constexpr auto footer_item_count = navigation.size() - 1;

constexpr float margin = 10;
constexpr float sidebar_width = 178;
constexpr float wide_sidebar_width = 224;
constexpr float desktop_header = 48;
constexpr float narrow_header = 78;
constexpr float breakpoint = 760;
constexpr float nav_row = 34;
constexpr float wide_nav_row = 38;
constexpr ImVec2 nav_inset{7, 5};
constexpr float nav_status_gap = 4;
constexpr float header_button_height = 34;
constexpr std::size_t narrow_config_rows = 3;
constexpr float narrow_work_height = 660;
constexpr float narrow_logs_height = 232;
constexpr float desktop_logs_min_height = 195;
constexpr float server_min_work_height = 120;

struct PanelLayout {
    bool narrow;
    bool sidebar;
    bool wide;
    float navigation_width;
    float narrow_navigation_height;
    float header_height;
    ImVec2 workspace;

    PanelLayout(float width, const PanelState& state)
        : narrow(width < breakpoint),
          sidebar(state.navigation_open && state.entry.page == EntryPage::workspace),
          wide(width >= theme::workspace_font_breakpoint),
          navigation_width(wide ? wide_sidebar_width : sidebar_width),
          narrow_navigation_height((2 + std::min(state.config_items.size(), narrow_config_rows)) * nav_row +
                                   2 * margin + nav_status_gap),
          header_height(narrow ? narrow_header : desktop_header),
          workspace{margin + (sidebar && !narrow ? navigation_width : 0),
                    header_height + margin + (sidebar && narrow ? narrow_navigation_height : 0)}
    {
    }

    float work_width(float canvas_width) const
    {
        return std::max(1.0f, canvas_width - workspace.x - margin);
    }

    bool stack_server_status(const PanelState& state, float viewport_width) const
    {
        const float font = wide ? theme::workspace_font_size : theme::secondary_font_size;
        // Choose columns independently of document scrolling. Allow for the pane's own scrollbar.
        const float content_width =
            work_width(viewport_width) - 2 * theme::window_padding.x - theme::scrollbar_size;
        return state.page == PanelPage::server && !server_status_has_columns(content_width, font);
    }
};

float compact_work_height(const PanelState& state, bool stacked_status)
{
    return stacked_status ? std::max(narrow_work_height, state.stacked_server_status_height)
                          : narrow_work_height;
}

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

std::string_view reported_core_state(const PreviewSession& session)
{
    return view::string_field(view::field(view::field(session.data(), "server"), "core"), "state");
}

float draw_header(const PanelLayout& layout, ImVec2 viewport, bool in_workspace,
                  const PreviewSession& session)
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
    const auto core_state = reported_core_state(session);
    const auto status = session.ready() && !core_state.empty()       ? std::format("WaterWall {}", core_state)
                        : session.status() == PreviewStatus::loading ? "Connecting to Wave…"
                                                                     : "Wave disconnected";
    const float status_size = theme::secondary_font_size;
    ImGui::PushFont(nullptr, status_size);
    const float status_width = ImGui::CalcTextSize(status.c_str()).x;
    const float status_x = layout.narrow ? 23 : viewport.x - 24 - status_width;
    const float center_y = layout.narrow ? 64 : desktop_header / 2;
    const float text_y = std::floor(center_y - status_size / 2);
    draw.AddCircleFilled(
        {status_x - 9, center_y}, 3,
        packed(session.ready() && core_state == "running" ? Color::success : Color::warning));
    draw.AddText({status_x, text_y}, packed(Color::secondary_text), status.c_str());
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
        render_create_config_menu(state.panel_controls, session);
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

const view::Json& config_registry(const PreviewSession& session)
{
    const auto& server = view::field(session.data(), "server");
    return view::array_field(view::field(server, "core"), "configs");
}

void sync_configs(PanelState& state, PreviewSession& session)
{
    if (!session.ready()) {
        return;
    }
    std::vector<PanelConfigItem> items;
    for (const auto& config : config_registry(session)) {
        const auto& key = view::field(config, "id");
        const auto name = view::string_field(config, "name");
        if (key.is_number_integer() && !name.empty()) {
            const auto id = key.get<std::uint64_t>();
            if (id > 0 && id <= std::numeric_limits<int>::max()) {
                items.push_back({static_cast<int>(id), std::string(name)});
            }
        }
    }
    if (state.config_items == items) {
        return;
    }
    state.config_items = std::move(items);
    const auto exists = [&](int id) {
        return std::ranges::find(state.config_items, id, &PanelConfigItem::id) != state.config_items.end();
    };
    for (auto it = state.workspaces.begin(); it != state.workspaces.end();) {
        if (exists(it->first)) {
            ++it;
        } else {
            abandon_config_workspace(it->second, session);
            it = state.workspaces.erase(it);
        }
    }
    if (state.page == PanelPage::config && !exists(state.config_id)) {
        state.page = PanelPage::server;
        state.config_id = 0;
    }
}

void select_page(PanelState& state, int control)
{
    if (control < 0) {
        if (std::ranges::any_of(state.config_items,
                                [control](const auto& item) { return -item.id == control; })) {
            state.page = PanelPage::config;
            state.config_id = -control;
        }
        return;
    }
    const auto item =
        std::ranges::find(navigation, static_cast<PanelPage>(control), &PanelNavigationItem::page);
    if (item != navigation.end()) {
        state.page = item->page;
    }
}

void navigation_link(PanelState& state, int control, std::string_view label, float left, float top,
                     float width, float item_height, bool narrow)
{
    const float item_width = width - 2 * nav_inset.x;
    ImGui::SetCursorPos({left + nav_inset.x, top});
    const auto position = ImGui::GetCursorScreenPos();
    const bool selected = state.page_control() == control;
    const auto flags = ImGuiSelectableFlags_NoAutoClosePopups;
    ImGui::PushID(control);
    if (ImGui::Selectable("##link", selected, flags, {item_width, item_height})) {
        select_page(state, control);
    }
    const bool smaller_label =
        narrow && ImGui::CalcTextSize(label.data(), label.data() + label.size()).x > item_width;
    if (smaller_label) {
        ImGui::PushFont(nullptr, theme::annotation_font_size);
    }
    auto& draw = *ImGui::GetWindowDrawList();
    const ImVec2 start{position.x, position.y + std::floor((item_height - ImGui::GetFontSize()) / 2)};
    const float right = position.x + item_width;
    ImGui::RenderTextEllipsis(&draw, start, {right, position.y + item_height}, right, label.data(),
                              label.data() + label.size(), nullptr);
    if (smaller_label) {
        ImGui::PopFont();
    }
    if (selected) {
        const auto min = ImGui::GetItemRectMin();
        const auto max = ImGui::GetItemRectMax();
        const float edge = std::min(max.x, draw.GetClipRectMax().x);
        draw.AddRectFilled({edge - theme::selection_width, min.y}, {edge, max.y}, packed(Color::accent));
    }
    ImGui::PopID();
}

void navigation_divider(float width, float top, float thickness = theme::border_width)
{
    const auto origin = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLineH(origin.x, origin.x + width, origin.y + top, packed(Color::divider),
                                         thickness);
}

void render_config_links(PanelState& state, float width, float top, float height, float row_height,
                         bool narrow)
{
    if (height <= 0 || state.config_items.empty()) {
        return;
    }
    ImGui::SetCursorPos({0, top});
    if (ImGui::BeginChild("Config links", {width, height}, ImGuiChildFlags_NavFlattened)) {
        const auto position = ImGui::GetWindowPos();
        const auto size = ImGui::GetWindowSize();
        if (ImGui::GetScrollMaxY() > 0) {
            state.config_nav_scroll_bounds = {position.x, position.y, size.x, size.y};
        }
        const float content_width = ImGui::GetContentRegionAvail().x;
        for (std::size_t index = 0; index < state.config_items.size(); ++index) {
            const auto& item = state.config_items[index];
            const float y = index * row_height;
            navigation_link(state, -item.id, item.name, 0, y, content_width, row_height - 4, narrow);
            navigation_divider(content_width, y + row_height - 2 - ImGui::GetScrollY());
        }
    }
    ImGui::EndChild();
}

void render_navigation(PanelState& state, const PanelLayout& layout, float canvas_width,
                       float viewport_height)
{
    if (!layout.sidebar) {
        return;
    }
    const float width = layout.narrow ? canvas_width - 2 * margin : layout.navigation_width - margin;
    const float row_height = layout.wide ? wide_nav_row : nav_row;
    const float item_height = row_height - 4;
    const float min_height =
        2 * nav_inset.y + (navigation.size() + !state.config_items.empty()) * row_height + nav_status_gap;
    const float height = layout.narrow
                             ? layout.narrow_navigation_height - margin
                             : std::max(min_height, viewport_height - layout.header_height - 2 * margin);
    const float footer_y = height - nav_inset.y - (layout.narrow ? 1 : footer_item_count) * row_height;
    const float config_y = nav_inset.y + row_height + nav_status_gap;
    const float config_height = std::min(state.config_items.size() * row_height, footer_y - config_y);
    ImGui::SetCursorScreenPos({margin, layout.header_height + margin});

    if (begin_pane("Navigation", {width, height}, Color::pane,
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
                   ImGuiChildFlags_NavFlattened)) {
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        // Selectable expands its hitbox by half the item spacing around the label area.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {2 * nav_inset.x, 0});
        navigation_link(state, static_cast<int>(navigation.front().page), navigation.front().label, 0,
                        nav_inset.y, width, item_height, layout.narrow);
        navigation_divider(width, nav_inset.y + row_height, 2 * theme::border_width);

        render_config_links(state, width, config_y, config_height, row_height, layout.narrow);
        const auto origin = ImGui::GetWindowPos();
        auto& draw = *ImGui::GetWindowDrawList();
        for (std::size_t row = 0; row < footer_item_count; ++row) {
            const auto& item = navigation[row + 1];
            if (layout.narrow) {
                const float left = std::floor(row * width / footer_item_count);
                const float right = std::floor((row + 1) * width / footer_item_count);
                navigation_link(state, static_cast<int>(item.page), item.label, left, footer_y, right - left,
                                item_height, true);
                if (row != 0) {
                    draw.AddLineV(origin.x + left, origin.y + footer_y, origin.y + footer_y + item_height,
                                  packed(Color::divider), theme::border_width);
                }
            } else {
                const float y = footer_y + row * row_height;
                navigation_link(state, static_cast<int>(item.page), item.label, 0, y, width, item_height,
                                false);
                if (row + 1 < footer_item_count) {
                    navigation_divider(width, y + item_height + 1);
                }
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
    session.take_reply(state.refresh_request);
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
        const bool pending =
            (sign_out && state.auth.pending()) ||
            (diagnostics && (state.refresh_request || session.status() == PreviewStatus::loading));
        const std::array actions = {DialogAction{sign_out ? "Cancel" : "Close", pending},
                                    DialogAction{sign_out ? "Sign out" : "Refresh workspace", pending}};
        const auto count = sign_out || diagnostics ? 2 : 1;
        Dialog frame(title, width, std::span(actions).first(count),
                     {.busy = sign_out || diagnostics ? std::optional(pending) : std::nullopt});
        if (!frame) {
            continue;
        }
        switch (dialog) {
        case PanelDialog::sign_out:
            ImGui::TextWrapped("End this session and return to sign-in?");
            if (state.auth.action() == AuthSession::Action::sign_out && !state.auth.error().empty()) {
                session.add_local_log("error", state.auth.error());
                state.auth.clear_error();
                ImGui::CloseCurrentPopup();
            }
            break;
        case PanelDialog::version: {
            const auto version = displayed_panel_version(session);
            ImGui::Text("Wave %.*s", static_cast<int>(version.size()), version.data());
            ImGui::TextWrapped("Dear ImGui %s / WebAssembly / WebGL 2", IMGUI_VERSION);
            ImGui::TextLinkOpenURL("Project releases", "https://github.com/radkesvat/wave/releases");
            break;
        }
        case PanelDialog::diagnostics: {
            ImGui::TextWrapped("Preview socket: %s", session.status() == PreviewStatus::ready ? "Connected"
                                                     : session.status() == PreviewStatus::loading
                                                         ? "Connecting"
                                                         : "Unavailable");
            const auto core_state = reported_core_state(session);
            if (core_state.empty()) {
                ImGui::TextWrapped("WaterWall state: Unavailable");
            } else {
                ImGui::TextWrapped("%s: %.*s", session.ready() ? "WaterWall state" : "Last WaterWall state",
                                   static_cast<int>(core_state.size()), core_state.data());
            }
            ImGui::TextWrapped("Workspace transport: JSON WebSocket");
            ImGui::TextWrapped("Renderer: %.0f frames/s, %.2f× scale", ImGui::GetIO().Framerate,
                               ImGui::GetIO().DisplayFramebufferScale.x);
            break;
        }
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
        } else if (action == 1 && diagnostics) {
            if (session.ready()) {
                state.refresh_request = session.submit({{"action", "refresh"}}, 5000);
            } else {
                session.start(true);
            }
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
    switch (state.page) {
    case PanelPage::server: {
        const auto& server = view::field(session.data(), "server");
        if (server.is_null()) {
            if (session.status() == PreviewStatus::loading) {
                draw_busy_spinner(ImGui::GetFrameHeight());
            } else {
                view::wrapped("Panel connection unavailable.", Color::warning);
                if (theme::button("Reconnect")) {
                    session.start();
                }
            }
            break;
        }
        const float height = render_server_status(server, state.panel_controls, session, stacked_status);
        if (narrow && stacked_status) {
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
    case PanelPage::config: {
        const auto& configs = config_registry(session);
        const auto config = std::ranges::find_if(
            configs, [&](const view::Json& item) { return view::field(item, "id") == state.config_id; });
        if (config != configs.end()) {
            render_config_workspace(state.workspaces[state.config_id], state.panel_controls, session, *config,
                                    state.config_id, narrow);
        } else {
            view::wrapped("Config unavailable.", Color::warning);
            if (!session.ready() && session.status() != PreviewStatus::loading &&
                theme::button("Reconnect")) {
                session.start(true);
            }
        }
        break;
    }
    case PanelPage::about:
        render_about(displayed_panel_version(session));
        break;
    }
}

} // namespace

const char* panel_page_name(PanelPage page)
{
    const auto item = std::ranges::find(navigation, page, &PanelNavigationItem::page);
    return item == navigation.end() ? "Config" : item->label;
}

std::span<const PanelNavigationItem> panel_navigation()
{
    return navigation;
}

int PanelState::page_control() const
{
    return page == PanelPage::config ? -config_id : static_cast<int>(page);
}

ConfigWorkspaceState* PanelState::config_workspace()
{
    const auto config = workspaces.find(config_id);
    return page == PanelPage::config && config != workspaces.end() ? &config->second : nullptr;
}

bool panel_tool_window_at(const PanelState& state, float x, float y)
{
    const auto contains = [x, y](const char* title) {
        const auto* window = ImGui::FindWindowByName(title);
        return window && window->Active && window->Rect().Contains({x, y});
    };
    return (state.metrics_open && contains("Dear ImGui Metrics/Debugger")) ||
           (state.imgui_demo_open && contains(imgui_demo_title)) ||
           (state.implot_demo_open && (contains(implot_demo_title) || contains(imgui_demo_title)));
}

float panel_height(float viewport_width, const PanelState& state)
{
    const PanelLayout layout(viewport_width, state);
    if (state.entry.page != EntryPage::workspace) {
        return layout.header_height;
    }
    if (layout.narrow) {
        const float work_height =
            compact_work_height(state, layout.stack_server_status(state, viewport_width));
        return layout.workspace.y + work_height + narrow_logs_height + 2 * margin;
    }
    return state.page == PanelPage::server
               ? layout.workspace.y + server_min_work_height + desktop_logs_min_height + 2 * margin
               : 666;
}

void render_panel(PanelState& state, PreviewSession& session, ImVec2 browser_viewport, int browser_focus)
{
    state.console = {};
    state.config_nav_scroll_bounds = {};
    state.settings.core.scroll_bounds = {};
    const bool in_workspace = state.entry.page == EntryPage::workspace;
    if (in_workspace) {
        sync_configs(state, session);
        update_panel_controls(state.panel_controls, session);
        update_logs_view(state.logs, session);
    }
    const ImVec2 viewport = ImGui::GetIO().DisplaySize;
    if (state.browser_control && state.navigation_open && in_workspace) {
        select_page(state, state.browser_control);
    }

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(viewport);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("Wave workspace", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    PanelLayout layout(browser_viewport.x, state);
    const float menu_x = draw_header(layout, viewport, in_workspace, session);
    if (in_workspace) {
        render_menus(state, menu_x, browser_focus, session);
    }
    state.browser_control = 0;
    layout = PanelLayout(browser_viewport.x, state);
    if (!in_workspace) {
        render_entry(state.entry, state.auth, layout.header_height);
    } else {
        render_navigation(state, layout, viewport.x, browser_viewport.y);
        const float width = layout.work_width(viewport.x);
        const bool stacked_status = layout.stack_server_status(state, browser_viewport.x);
        const float logs_height =
            layout.narrow ? narrow_logs_height
                          : std::floor(std::clamp(viewport.y * 0.25f, desktop_logs_min_height, 244.0f));
        const float work_height =
            layout.narrow ? compact_work_height(state, stacked_status)
                          : std::max(1.0f, viewport.y - layout.workspace.y - logs_height - 2 * margin);
        ImGui::PushID(state.page_control());
        ImGui::SetCursorScreenPos(layout.workspace);
        const auto view_flags = layout.narrow && stacked_status
                                    ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                                    : 0;
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
        if (auto* workspace = state.config_workspace()) {
            workspace->nodes.viewport = {};
        }
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
    auto description = std::format("Wave {}.", panel_page_name(state.page));
    if (state.page == PanelPage::settings) {
        description += ' ' + describe_settings(state.settings, state.auth);
    } else if (state.page == PanelPage::config) {
        const auto item = std::ranges::find(state.config_items, state.config_id, &PanelConfigItem::id);
        const auto workspace = state.workspaces.find(state.config_id);
        if (item != state.config_items.end() && workspace != state.workspaces.end()) {
            description =
                std::format("Wave Config {}. {}", item->name, describe_config_workspace(workspace->second));
            if (config_action_pending(state.panel_controls, state.config_id)) {
                description += std::string(" ") + busy_description;
            }
        }
    }
    const bool controls_dialog = state.panel_controls.dialog != PanelControlsState::Dialog::none;
    const bool has_server = state.page == PanelPage::server && session.data().contains("server");
    if (!session.ready() && !has_server) {
        auto result =
            description + (session.status() == PreviewStatus::loading ? " Loading panel data."
                                                                      : " Panel connection unavailable.");
        if (controls_dialog) {
            result += ' ' + describe_panel_controls(state.panel_controls, session, false);
        }
        if (!session.logs().empty()) {
            result += ' ' + describe_logs(state.logs, session.logs());
        }
        return result;
    }

    std::string result = description + ' ' + describe_logs(state.logs, session.logs());
    if (state.refresh_request || state.logs.clear_request ||
        (state.auth.pending() && state.auth.action() == AuthSession::Action::sign_out)) {
        result += std::string(" ") + busy_description;
    }
    if (state.page == PanelPage::server) {
        result += ' ' + describe_server_status(session.data()["server"]);
    }
    if (has_server || controls_dialog) {
        result += ' ' + describe_panel_controls(state.panel_controls, session, has_server);
    }
    return result;
}

} // namespace wave
