#include "core_settings_view.h"

#include "core_settings.h"
#include "dialog.h"
#include "view_helpers.h"
#include "wave_version.h"

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace wave {
namespace {

namespace spec = core_settings;

using Section = CoreSettingsDraft::Section;
using theme::Color;
using view::Json;

constexpr char help[] = "Changes apply on the next core start. Enable configs from their workspace headers.";
constexpr char save_label[] = "Save core settings";
constexpr char revert_label[] = "Revert edits";

struct HelpText {
    std::string_view key;
    std::string_view description;
};

// WaterWall v1.6.0 core settings and c-ares options.
constexpr HelpText field_help[] = {
    {"workers",
     "If omitted or set to 0 or less, WaterWall uses all online logical CPUs. It caps the count at 254 "
     "workers."},
    {"ram-profile",
     "Sizes memory pools and sets defaults for memory-sensitive node settings. Defaults to Server (L2)."},
    {"mtu", "Default MTU for nodes that inherit the core setting. A node can override it. Defaults to 1500 "
            "bytes."},
    {"libs-path", "Directory used to load external tunnel libraries. Defaults to libs/."},
    {"splice",
     "Allows eligible stream chains to move data through kernel pipes. Defaults to enabled. Every node in "
     "the chain and the platform must support splice."},
    {"tcp-tune",
     "Lets WaterWall try to raise Linux kernel TCP memory limits at startup when splice is enabled. "
     "Defaults to enabled on Linux."},
    {"try-enabling-bbr",
     "Lets WaterWall try to enable the BBR congestion controller at startup on Linux. Defaults to enabled "
     "on Linux."},
    {"tcp-stack-receive-budget",
     "Per-worker target for advertised userspace TCP receive windows. Defaults to 32 MiB. Enter 0 to "
     "disable adaptation."},
    {"lwip-memory-budget",
     "Total memory limit for growable lwIP pools and retained packet data across the process. Defaults to "
     "512 MiB."},
    {"lwip-max-tcp-flows",
     "Maximum number of TCP states across the process, including opening and closing connections. "
     "Defaults to 65,536."},
    {"lwip-max-udp-flows", "Maximum number of UDP states across the process. Defaults to 65,536."},
    {"lwip-max-tcp-listeners",
     "Maximum number of userspace TCP listeners across the process. Defaults to 1,024."},
    {"lwip-max-tcp-segments",
     "Maximum number of TCP segments held in send, receive, and retransmit queues. Defaults to 1,048,576."},
    {"lwip-max-rx-wrappers",
     "Maximum number of objects retaining incoming packets in lwIP. Defaults to 262,144."},
    {"path",
     "WaterWall appends each log file name directly to this directory. End it with /. Defaults to log/."},
    {"loglevel",
     "Sets the minimum severity written by this logger. Defaults to Info. Silent disables output from "
     "this logger."},
    {"file",
     "File name under the log directory. Use a distinct file name for each logger so Wave can identify "
     "its source."},
    {"console",
     "Writes this logger's messages to the core console as well as its configured file. Defaults to "
     "enabled."},
    {"domain-strategy",
     "Default address selection for nodes that inherit the core DNS preference. Prefer IPv4 falls back to "
     "IPv6. IPv4 only and IPv6 only require the selected family."},
    {"servers",
     "Enter DNS servers separated by commas. Use [IPv6]:port when specifying an IPv6 port. Leave blank to "
     "use system resolvers."},
    {"domains",
     "Suffixes the resolver can append to relative names. Enter one search domain per line. Leave blank "
     "to use the system search list."},
    {"lookups", "Choose whether to consult DNS, the hosts file, or both in order. The core default uses the "
                "resolver's system lookup order."},
    {"timeout-ms",
     "Initial time allowed for one DNS attempt before the resolver has response history for the server. "
     "Defaults to 1000 ms."},
    {"max-timeout-ms",
     "Upper limit for a single DNS attempt, including retry attempts. A full lookup can take longer. "
     "Defaults to 5000 ms."},
    {"tries", "Number of query attempts allowed per DNS server. Defaults to 5."},
    {"query-cache-max-ttl",
     "Maximum time DNS results stay in the query cache. Defaults to 1800 seconds. Enter 0 to disable the "
     "cache."},
    {"ndots",
     "Minimum dots in a name before the resolver tries it as written ahead of the search domains. Accepts "
     "0 to 15."},
    {"udp-port", "Default destination port for DNS over UDP."},
    {"tcp-port", "Default destination port for DNS over TCP."},
    {"socket-send-buffer-size",
     "Requested socket send buffer size for the resolver, in bytes. Leave blank to use the system socket "
     "default."},
    {"socket-receive-buffer-size",
     "Requested socket receive buffer size for the resolver, in bytes. Leave blank to use the system "
     "socket default."},
    {"edns-packet-size", "Maximum UDP payload size advertised to DNS servers when EDNS is enabled."},
    {"udp-max-queries", "Maximum queries a resolver sends through one UDP socket before rotating it."},
    {"rotate",
     "Rotates the starting DNS server across queries. Core default follows the resolver's system setting."},
    {"resolvconf-path",
     "File the resolver reads for system DNS configuration. Defaults to /etc/resolv.conf on Linux."},
    {"hosts-path",
     "File used for local host-name lookups when the lookup order includes hosts. Defaults to /etc/hosts "
     "on Linux."},
    {"sortlist", "Orders DNS address results by preferred networks. Use c-ares sortlist syntax."},
    {"flags",
     "The defaults enable EDNS and reuse idle DNS connections. Override core defaults replaces the entire "
     "flag set with the checked options."},
    {"retry-chance",
     "Controls how often c-ares retries failed DNS servers after the retry delay. Defaults to 10."},
    {"retry-delay-ms", "Wait before a failed DNS server is eligible to be retried. Defaults to 5000 ms."},
    {"use-vc", "Send DNS queries over TCP."},
    {"primary", "Ask the resolver to use only its first DNS server."},
    {"ignore-truncated", "Avoid switching to TCP when a server returns a truncated UDP reply."},
    {"no-recurse", "Request that DNS servers skip recursion for these queries."},
    {"stayopen", "Keep idle DNS connections available for later queries."},
    {"no-search", "Skip search-domain expansion when looking up a name."},
    {"no-aliases", "Skip the host alias file named by HOSTALIASES."},
    {"no-check-response", "Skip the resolver's usual checks for certain DNS response errors."},
    {"edns", "Use Extended DNS to advertise features such as a larger UDP reply size."},
    {"no-default-server", "Skip the resolver's fallback server when no DNS servers are configured."},
    {"dns0x20", "Randomize letter case in outgoing DNS query names and check that replies match it."},
};

std::string_view describe_field(std::string_view key, std::string_view fallback = {})
{
    const auto found = std::ranges::find(field_help, key, &HelpText::key);
    return found == std::end(field_help) ? fallback : found->description;
}

const Json& current(const CoreSettingsDraft& draft, const Json::json_pointer& path)
{
    static const Json empty;
    return draft.settings.contains(path) ? draft.settings.at(path) : empty;
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
    while (!group.empty() && draft.settings[group].empty() && group.to_string() != "/dns/server-failover") {
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

std::string initial_text(spec::Kind kind, const Json& value)
{
    if (kind == spec::Kind::domains && value.is_array()) {
        std::string text;
        for (const auto& item : value) {
            if (!text.empty()) {
                text += '\n';
            }
            text += view::string_value(item);
        }
        return text;
    }
    if (kind == spec::Kind::integer && value.is_number_integer()) {
        return value.dump();
    }
    return std::string(view::string_value(value));
}

Json domain_list(std::string_view text)
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

void edit_text(CoreSettingsDraft& draft, const spec::Field& field, const Json::json_pointer& path,
               const char* hint)
{
    const auto key = path.to_string();
    const auto& value = current(draft, path);
    auto& text = draft.inputs.try_emplace(key, initial_text(field.kind, value)).first->second;
    if (field.kind == spec::Kind::domains) {
        if (ImGui::InputTextMultiline("##value", &text, {-1, 84})) {
            set_value(draft, path, domain_list(text));
        }
        return;
    }

    const auto flags = field.kind == spec::Kind::integer ? ImGuiInputTextFlags_CharsDecimal : 0;
    const bool stored_empty = value.is_string() && value.get_ref<const std::string&>().empty();
    if (ImGui::InputTextWithHint("##value", stored_empty ? "Empty value" : hint, &text, flags)) {
        if (text.empty()) {
            set_value(draft, path, {});
        } else if (field.kind == spec::Kind::text) {
            set_value(draft, path, text);
        } else {
            std::int64_t number = 0;
            const auto end = text.data() + text.size();
            const auto parsed = std::from_chars(text.data(), end, number);
            if (parsed.ec == std::errc{} && parsed.ptr == end && spec::valid_integer(field, number)) {
                set_value(draft, path, number);
            } else {
                draft.validation.clear();
                draft.errors[key] =
                    std::format("{} requires a whole number {}from {} to {}.", field.label,
                                field.allow_zero ? "of 0 or " : "", field.minimum, field.maximum);
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

std::span<const spec::Choice> choices(spec::Kind kind)
{
    switch (kind) {
    case spec::Kind::log_level:
        return spec::log_levels;
    case spec::Kind::ram_profile:
        return spec::ram_profiles;
    case spec::Kind::domain_strategy:
        return spec::domain_strategies;
    case spec::Kind::lookups:
        return spec::lookup_orders;
    default:
        return {};
    }
}

Json choice_value(spec::Kind kind, std::string_view value)
{
    if (kind == spec::Kind::ram_profile && (value == "2" || value == "5")) {
        return value.front() - '0';
    }
    return value;
}

void edit_choice(CoreSettingsDraft& draft, spec::Kind kind, const Json::json_pointer& path)
{
    const auto value = current(draft, path);
    const auto options = choices(kind);
    const char* label = "Core default";
    if (kind == spec::Kind::boolean && value.is_boolean()) {
        label = value == true ? "Enabled" : "Disabled";
    } else {
        for (const auto& option : options) {
            if (value == choice_value(kind, option.value)) {
                label = option.label;
                break;
            }
        }
    }
    if (!ImGui::BeginCombo("##value", label)) {
        return;
    }
    if (ImGui::Selectable("Core default", value.is_null())) {
        set_value(draft, path, {});
    }
    if (kind == spec::Kind::boolean) {
        if (ImGui::Selectable("Enabled", value == true)) {
            set_value(draft, path, true);
        }
        if (ImGui::Selectable("Disabled", value == false)) {
            set_value(draft, path, false);
        }
    } else {
        for (const auto& option : options) {
            auto selected = choice_value(kind, option.value);
            if (ImGui::Selectable(option.label, value == selected)) {
                set_value(draft, path, std::move(selected));
            }
        }
    }
    ImGui::EndCombo();
}

void edit_flags(CoreSettingsDraft& draft, const Json::json_pointer& path)
{
    auto value = current(draft, path);
    bool override_flags = value.is_array();
    if (ImGui::Checkbox("Override core defaults", &override_flags)) {
        value = override_flags ? Json::array({"stayopen", "edns"}) : Json{};
        set_value(draft, path, value);
    }
    if (!override_flags) {
        return;
    }
    for (const auto& option : spec::dns_flags) {
        auto selected = std::ranges::find_if(value, [&](const Json& item) { return item == option.value; });
        bool enabled = selected != value.end();
        if (ImGui::Checkbox(option.label, &enabled)) {
            if (enabled) {
                value.push_back(option.value);
            } else {
                value.erase(selected);
            }
            set_value(draft, path, value);
        }
        view::help_marker(describe_field(option.value));
    }
}

void field_editor(CoreSettingsDraft& draft, std::string_view group, const spec::Field& field,
                  const char* hint = nullptr, bool align_label = false)
{
    const auto id = std::format("{}/{}", group, field.key);
    const Json::json_pointer path(id);
    ImGui::PushID(id.c_str());
    if (align_label) {
        ImGui::AlignTextToFramePadding();
    }
    ImGui::TextUnformatted(field.label);
    view::help_marker(describe_field(field.key, field.hint));
    ImGui::SetNextItemWidth(-1);
    switch (field.kind) {
    case spec::Kind::integer:
    case spec::Kind::text:
    case spec::Kind::domains:
        edit_text(draft, field, path, hint ? hint : field.hint);
        break;
    case spec::Kind::flags:
        edit_flags(draft, path);
        break;
    default:
        edit_choice(draft, field.kind, path);
        break;
    }
    ImGui::PopID();
}

void fields(CoreSettingsDraft& draft, const char* group, std::span<const spec::Field> rows,
            const char* logger_file = nullptr, int max_columns = 2)
{
    const float column_width = 16 * ImGui::GetFontSize();
    const int columns =
        std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / column_width), 1, max_columns);
    ImGui::PushID(group);
    if (ImGui::BeginTable("Fields", columns,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        for (const auto& field : rows) {
            ImGui::TableNextColumn();
            const auto hint = std::string_view(field.key) == "file" ? logger_file : nullptr;
            field_editor(draft, group, field, hint, columns > 1);
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void runtime(CoreSettingsDraft& draft)
{
    ImGui::PushStyleVarY(ImGuiStyleVar_CellPadding, 2);
    const auto& style = ImGui::GetStyle();
    constexpr auto rows_at_two_columns = (std::size(spec::runtime_fields) + 1) / 2;
    const float row_height = 2 * ImGui::GetFrameHeight() + style.ItemSpacing.y + 2 * style.CellPadding.y;
    const float collapsed_height =
        rows_at_two_columns * row_height + style.ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    const int max_columns = ImGui::GetContentRegionAvail().y < collapsed_height ? 3 : 2;
    fields(draft, "/misc", spec::runtime_fields, nullptr, max_columns);
    ImGui::PopStyleVar();
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("TCP and lwIP tuning")) {
        fields(draft, "/misc", spec::stack_fields);
    }
}

void logging(CoreSettingsDraft& draft)
{
    field_editor(draft, "/log", spec::log_path);
    for (const auto& logger : spec::loggers) {
        const auto group = std::format("/log/{}", logger.value);
        const auto file = std::format("{}.log", logger.value);
        ImGui::Spacing();
        ImGui::SeparatorText(logger.label);
        fields(draft, group.c_str(), spec::logger_fields, file.c_str());
    }
}

void dns(CoreSettingsDraft& draft)
{
    fields(draft, "/dns", spec::dns_fields);
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Resolver tuning")) {
        fields(draft, "/dns", spec::dns_advanced_fields);
    }
    if (ImGui::CollapsingHeader("Server failover")) {
        constexpr char group[] = "/dns/server-failover";
        const Json::json_pointer path(group);
        bool configured = current(draft, path).is_object();
        if (ImGui::Checkbox("Configure server failover", &configured)) {
            set_value(draft, path, configured ? Json::object() : Json{});
            if (!configured) {
                const auto within_group = [](const auto& entry) {
                    return entry.first.starts_with("/dns/server-failover/");
                };
                std::erase_if(draft.inputs, within_group);
                std::erase_if(draft.errors, within_group);
            }
        }
        view::help_marker(
            "Set the retry behavior for failed DNS servers. Leave unchecked to use c-ares defaults.");
        if (configured) {
            fields(draft, group, spec::failover_fields);
        }
    }
}

const char* section_name(Section section)
{
    switch (section) {
    case Section::runtime:
        return "Runtime";
    case Section::logging:
        return "Logging";
    case Section::dns:
        return "DNS";
    }
    return "";
}

void render_sections(CoreSettingsDraft& draft, float footer_height)
{
    if (!ImGui::BeginTabBar("Core sections")) {
        return;
    }
    for (const auto section : {Section::runtime, Section::logging, Section::dns}) {
        if (!ImGui::BeginTabItem(section_name(section))) {
            continue;
        }
        draft.section = section;
        if (ImGui::BeginChild("Core options", {0, -footer_height}, ImGuiChildFlags_Borders)) {
            if (ImGui::GetScrollMaxY() > 0) {
                const auto position = ImGui::GetWindowPos();
                const auto size = ImGui::GetWindowSize();
                draft.scroll_bounds = {position.x, position.y, size.x, size.y};
            }
            switch (section) {
            case Section::runtime:
                runtime(draft);
                break;
            case Section::logging:
                logging(draft);
                break;
            case Section::dns:
                dns(draft);
                break;
            }
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
    ImGui::Text("WaterWall %s", wave::version);
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
    if (!draft.source.is_object()) {
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
    auto description =
        std::format("WaterWall core settings. {}. Version {}.", section_name(draft.section), wave::version);
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
