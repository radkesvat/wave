#include "core_settings_view.h"

#include "core_settings.h"
#include "dialog.h"
#include "view_helpers.h"
#include "waterwall_schematic.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <string_view>
#include <utility>

namespace wave {
namespace {

namespace spec = core_settings;

using theme::Color;
using view::Json;

constexpr char help[] = "Changes apply on the next core start. Enable configs from their workspace headers.";
constexpr char save_label[] = "Save core settings";
constexpr char revert_label[] = "Revert edits";

const Json& current(const CoreSettingsDraft& draft, const Json::json_pointer& path)
{
    static const Json empty;
    return draft.settings.contains(path) ? draft.settings.at(path) : empty;
}

bool preserve_empty(const Json::json_pointer& path)
{
    const auto key = path.to_string();
    for (const auto& section : spec::sections()) {
        for (const auto& group : schematic_field(section, "groups")) {
            if (schematic_field(group, "path") == key && spec::enabled(group, "preserve_empty")) {
                return true;
            }
        }
    }
    return false;
}

void set_value(CoreSettingsDraft& draft, const Json::json_pointer& path, Json value)
{
    draft.errors.erase(path.to_string());
    draft.validation.clear();
    if (!value.is_null()) {
        draft.settings[path] = std::move(value);
        return;
    }

    auto group = path.parent_pointer();
    if (!draft.settings.contains(group)) {
        return;
    }
    draft.settings[group].erase(path.back());
    while (!group.empty() && draft.settings[group].empty() && !preserve_empty(group)) {
        const auto key = group.back();
        group.pop_back();
        draft.settings[group].erase(key);
    }
}

void reload(CoreSettingsDraft& draft, const Json& source)
{
    draft.source = source;
    draft.settings = source;
    draft.inputs.clear();
    draft.errors.clear();
}

std::string initial_text(std::string_view kind, const Json& value)
{
    if (kind == "string_list" && value.is_array()) {
        std::string text;
        for (const auto& item : value) {
            if (!text.empty()) {
                text += '\n';
            }
            text += view::string_value(item);
        }
        return text;
    }
    if (kind == "integer" && value.is_number_integer()) {
        return value.dump();
    }
    return std::string(view::string_value(value));
}

Json string_list(std::string_view text)
{
    auto values = Json::array();
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const auto line = text.substr(0, newline);
        const auto first = line.find_first_not_of(" \t\r");
        if (first != line.npos) {
            values.push_back(line.substr(first, line.find_last_not_of(" \t\r") - first + 1));
        }
        if (newline == text.npos) {
            break;
        }
        text.remove_prefix(newline + 1);
    }
    return values.empty() ? Json{} : std::move(values);
}

void edit_text(CoreSettingsDraft& draft, const Json& field, const Json::json_pointer& path,
               std::string_view kind)
{
    const auto key = path.to_string();
    const auto& value = current(draft, path);
    auto& text = draft.inputs.try_emplace(key, initial_text(kind, value)).first->second;
    if (kind == "string_list") {
        if (ImGui::InputTextMultiline("##value", &text, {-1, 84})) {
            set_value(draft, path, string_list(text));
        }
        return;
    }

    const auto flags = kind == "integer" ? ImGuiInputTextFlags_CharsDecimal : 0;
    const bool stored_empty = value.is_string() && value.get_ref<const std::string&>().empty();
    const auto hint = view::string_field(field, "hint");
    if (ImGui::InputTextWithHint("##value", stored_empty ? "Empty value" : hint.data(), &text, flags)) {
        if (text.empty()) {
            set_value(draft, path, {});
        } else if (kind == "text") {
            set_value(draft, path, text);
        } else {
            std::int64_t number = 0;
            const auto end = text.data() + text.size();
            const auto parsed = std::from_chars(text.data(), end, number);
            if (parsed.ec == std::errc{} && parsed.ptr == end && spec::valid_integer(field, number)) {
                set_value(draft, path, number);
            } else {
                draft.validation.clear();
                draft.errors[key] = std::format("{} requires a whole number {}from {} to {}.",
                                                view::string_field(field, "label"),
                                                spec::enabled(field, "allow_zero") ? "of 0 or " : "",
                                                field.at("min").dump(), field.at("max").dump());
            }
        }
    }
    if (stored_empty && theme::small_button("Use core default")) {
        text.clear();
        set_value(draft, path, {});
    }
    if (const auto error = draft.errors.find(key); error != draft.errors.end()) {
        view::wrapped(error->second, Color::error);
    }
}

void edit_choice(CoreSettingsDraft& draft, const Json& field, const Json::json_pointer& path, bool boolean)
{
    const auto value = current(draft, path);
    const auto& options = view::array_field(field, "choices");
    std::string_view label = "Core default";
    if (boolean && value.is_boolean()) {
        label = value == true ? "Enabled" : "Disabled";
    } else {
        for (const auto& option : options) {
            if (value == schematic_field(option, "value")) {
                label = view::string_field(option, "label");
                break;
            }
        }
    }
    if (!ImGui::BeginCombo("##value", label.data())) {
        return;
    }
    if (ImGui::Selectable("Core default", value.is_null())) {
        set_value(draft, path, {});
    }
    if (boolean) {
        if (ImGui::Selectable("Enabled", value == true)) {
            set_value(draft, path, true);
        }
        if (ImGui::Selectable("Disabled", value == false)) {
            set_value(draft, path, false);
        }
    } else {
        for (const auto& option : options) {
            const auto& selected = schematic_field(option, "value");
            if (ImGui::Selectable(view::string_field(option, "label").data(), value == selected)) {
                set_value(draft, path, selected);
            }
        }
    }
    ImGui::EndCombo();
}

void edit_flags(CoreSettingsDraft& draft, const Json& field, const Json::json_pointer& path)
{
    auto value = current(draft, path);
    bool override_flags = value.is_array();
    if (ImGui::Checkbox("Override core defaults", &override_flags)) {
        value = override_flags ? view::array_field(field, "default") : Json{};
        set_value(draft, path, value);
    }
    if (!override_flags) {
        return;
    }
    for (const auto& option : view::array_field(field, "choices")) {
        const auto& item = schematic_field(option, "value");
        auto selected = std::ranges::find(value, item);
        bool enabled = selected != value.end();
        if (ImGui::Checkbox(view::string_field(option, "label").data(), &enabled)) {
            if (enabled) {
                value.push_back(item);
            } else {
                value.erase(selected);
            }
            set_value(draft, path, value);
        }
        view::help_marker(view::string_field(option, "help"));
    }
}

void field_editor(CoreSettingsDraft& draft, const Json::json_pointer& group, const Json& field,
                  bool align_label = false)
{
    const auto path = group / std::string(view::string_field(field, "key"));
    const auto id = path.to_string();
    ImGui::PushID(id.c_str());
    if (align_label) {
        ImGui::AlignTextToFramePadding();
    }
    view::text(view::string_field(field, "label"));
    view::help_marker(view::string_field(field, "help", view::string_field(field, "hint")));
    ImGui::SetNextItemWidth(-1);
    const auto kind = view::string_field(field, "kind");
    if (kind == "integer" || kind == "text" || kind == "string_list") {
        edit_text(draft, field, path, kind);
    } else if (kind == "flags") {
        edit_flags(draft, field, path);
    } else {
        edit_choice(draft, field, path, kind == "boolean");
    }
    ImGui::PopID();
}

void fields(CoreSettingsDraft& draft, const Json::json_pointer& path, const Json& rows, int max_columns)
{
    const float column_width = 16 * ImGui::GetFontSize();
    const int columns =
        std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / column_width), 1, max_columns);
    ImGui::PushID(path.to_string().c_str());
    if (ImGui::BeginTable("Fields", columns,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        for (const auto& field : rows) {
            ImGui::TableNextColumn();
            field_editor(draft, path, field, columns > 1);
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

bool show_group(CoreSettingsDraft& draft, const Json& group, const Json::json_pointer& path)
{
    const auto label = view::string_field(group, "toggle");
    if (label.empty()) {
        return true;
    }
    bool configured = current(draft, path).is_object();
    if (ImGui::Checkbox(label.data(), &configured)) {
        set_value(draft, path, configured ? Json::object() : Json{});
        if (!configured) {
            const auto prefix = path.to_string() + '/';
            const auto within_group = [&](const auto& entry) { return entry.first.starts_with(prefix); };
            std::erase_if(draft.inputs, within_group);
            std::erase_if(draft.errors, within_group);
        }
    }
    view::help_marker(view::string_field(group, "help"));
    return configured;
}

void render_groups(CoreSettingsDraft& draft, const Json& groups)
{
    bool previous_collapsible = false;
    for (const auto& group : groups) {
        const auto heading = view::string_field(group, "heading");
        const bool collapsible = view::bool_field(group, "collapsible");
        if (collapsible && !previous_collapsible) {
            ImGui::Spacing();
        }
        previous_collapsible = collapsible;
        if (collapsible) {
            if (!ImGui::CollapsingHeader(heading.data())) {
                continue;
            }
        } else if (!heading.empty()) {
            ImGui::Spacing();
            ImGui::SeparatorText(heading.data());
        }

        const auto path = Json::json_pointer(std::string(view::string_field(group, "path")));
        if (!show_group(draft, group, path)) {
            continue;
        }
        const auto& rows = view::array_field(group, "fields");
        const bool compact = view::bool_field(group, "compact");
        int max_columns = view::integer_field(group, "columns", 2);
        if (compact) {
            ImGui::PushStyleVarY(ImGuiStyleVar_CellPadding, 2);
            const auto& style = ImGui::GetStyle();
            const auto row_count = (rows.size() + 1) / 2;
            const float row_height =
                2 * ImGui::GetFrameHeight() + style.ItemSpacing.y + 2 * style.CellPadding.y;
            const float collapsed_height =
                row_count * row_height + style.ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
            if (max_columns == 2 && ImGui::GetContentRegionAvail().y < collapsed_height) {
                max_columns = 3;
            }
        }
        if (rows.size() == 1 && max_columns == 1) {
            field_editor(draft, path, rows.front());
        } else {
            fields(draft, path, rows, max_columns);
        }
        if (compact) {
            ImGui::PopStyleVar();
        }
    }
}

void render_sections(CoreSettingsDraft& draft, float footer_height)
{
    if (!ImGui::BeginTabBar("Core sections")) {
        return;
    }
    for (const auto& section : spec::sections()) {
        const auto label = view::string_field(section, "label");
        if (!ImGui::BeginTabItem(label.data())) {
            continue;
        }
        draft.section = label;
        if (ImGui::BeginChild("Core options", {0, -footer_height}, ImGuiChildFlags_Borders)) {
            if (ImGui::GetScrollMaxY() > 0) {
                const auto position = ImGui::GetWindowPos();
                const auto size = ImGui::GetWindowSize();
                draft.scroll_bounds = {position.x, position.y, size.x, size.y};
            }
            render_groups(draft, view::array_field(section, "groups"));
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void save(CoreSettingsDraft& draft, PreviewSession& session)
{
    auto normalized = spec::normalize(draft.settings);
    if (!normalized.issue.empty()) {
        draft.validation = std::move(normalized.issue);
        return;
    }
    draft.validation.clear();
    draft.request = session.submit(
        {{"action", "apply_core_settings"}, {"settings", std::move(normalized.settings)}}, 5000);
}

} // namespace

void update_core_settings(CoreSettingsDraft& draft, PreviewSession& session)
{
    const auto& snapshot = view::field(session.data(), "core_settings");
    draft.notice = view::string_field(session.data(), "core_settings_notice");
    if (auto reply = session.take_reply(draft.request); reply && reply->ok()) {
        reload(draft, snapshot);
    }
    if (!draft.request && !draft.dirty() && snapshot.is_object() && snapshot != draft.source) {
        reload(draft, snapshot);
    }
}

void render_core_settings(CoreSettingsDraft& draft, PreviewSession& session)
{
    if (!draft.source.is_object() && session.status() == PreviewStatus::loading) {
        return;
    }
    const auto version = waterwall_version();
    if (!version.empty()) {
        view::text(std::format("WaterWall {}", version));
    }
    view::wrapped(help, Color::secondary_text);
    if (!draft.notice.empty()) {
        view::wrapped(draft.notice, Color::warning);
    }
    const auto& snapshot = view::field(session.data(), "core_settings");
    if (session.status() == PreviewStatus::unavailable) {
        view::wrapped("Settings connection unavailable.", Color::warning);
        if (theme::button("Reconnect")) {
            session.start(true);
        }
    }
    if (!draft.source.is_object() || !spec::sections().is_array()) {
        if (session.ready()) {
            view::wrapped("Core settings unavailable.", Color::warning);
        }
        return;
    }
    ImGui::BeginDisabled(draft.request.has_value());
    const auto& message = draft.errors.empty() ? draft.validation : draft.errors.begin()->second;
    const float width = ImGui::GetContentRegionAvail().x;
    const auto spacing = ImGui::GetStyle().ItemSpacing;
    const float controls_width =
        view::button_width(save_label) + view::button_width(revert_label) + 2 * spacing.x;
    const auto message_size = ImGui::CalcTextSize(message.c_str());
    const bool inline_message =
        controls_width + message_size.x <= width && message_size.y <= ImGui::GetTextLineHeight();
    float footer_height = ImGui::GetFrameHeightWithSpacing();
    if (!message.empty() && !inline_message) {
        footer_height += ImGui::CalcTextSize(message.c_str(), nullptr, false, width).y + spacing.y;
    }
    render_sections(draft, footer_height);
    const bool save_needed = draft.dirty() || !draft.notice.empty();
    ImGui::BeginDisabled(!save_needed || !draft.errors.empty() || !session.ready() || !snapshot.is_object());
    if (theme::button(save_label)) {
        save(draft, session);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!draft.dirty() || !snapshot.is_object());
    if (theme::button(revert_label)) {
        reload(draft, snapshot);
        draft.validation.clear();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (draft.request) {
        ImGui::SameLine();
        draw_busy_spinner(ImGui::GetFrameHeight());
    }
    if (!message.empty()) {
        if (inline_message) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
        }
        view::wrapped(message, Color::error);
    }
}

std::string describe_core_settings(const CoreSettingsDraft& draft)
{
    std::string description = "WaterWall core settings.";
    if (!draft.section.empty()) {
        description += " " + draft.section + '.';
    }
    if (const auto version = waterwall_version(); !version.empty()) {
        description += std::format(" Version {}.", version);
    }
    if (!draft.notice.empty()) {
        description += ' ' + draft.notice;
    }
    if (!draft.errors.empty()) {
        description += ' ' + draft.errors.begin()->second;
    } else if (!draft.validation.empty()) {
        description += ' ' + draft.validation;
    } else if (draft.request) {
        description += std::string(" ") + busy_description;
    }
    return description;
}

} // namespace wave
