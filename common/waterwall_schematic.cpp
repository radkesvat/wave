#include "waterwall_schematic.h"

#include "core_settings.h"
#include "schematic_rules.h"
#include "schematic_users_ui.h"
#include "waterwall_node_links.h"
#include "waterwall_node_settings.h"

#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

namespace wave {

struct WaterWallSchematic {
    struct Node {
        std::vector<node_settings::Field> fields;
        std::vector<NodeLinkField> links;
    };

    explicit WaterWallSchematic(nlohmann::json document) : data(std::move(document))
    {
    }

    nlohmann::json data;
    std::vector<std::string_view> types;
    std::map<std::string_view, Node> nodes;
    // Field choices must outlive the forms that read spans into them.
    std::deque<std::vector<const char*>> choice_lists;
};

namespace {

using Json = nlohmann::json;
using node_settings::Field;
using node_settings::Kind;

std::shared_ptr<const WaterWallSchematic> active;

void require(bool valid, std::string_view message)
{
    if (!valid) {
        throw std::runtime_error(std::string(message));
    }
}

const char* text(const Json& item)
{
    const auto* value = item.get_ptr<const std::string*>();
    require(value && value->find('\0') == std::string::npos, "Expected a string without NUL characters.");
    return value->c_str();
}

const char* optional_text(const Json& object, const char* key, const char* fallback = nullptr)
{
    const auto value = object.find(key);
    return value == object.end() ? fallback : text(*value);
}

std::array<const char*, 2> aliases(const Json& item)
{
    std::array<const char*, 2> result{};
    if (const auto list = item.find("aliases"); list != item.end()) {
        require(list->is_array() && list->size() <= result.size(), "Expected up to two aliases.");
        std::ranges::transform(*list, result.begin(), text);
    }
    return result;
}

Kind field_kind(std::string_view name)
{
    constexpr std::array kinds{
        "text",
        "boolean",
        "integer",
        "number",
        "choice",
        "string_list",
        "object",
        "object_list",
        "array",
        "text_or_list",
        "integer_or_boolean",
        "port",
        "port_list",
        "port_range",
        "json",
        "node_target",
    };
    const auto found = std::ranges::find(kinds, name);
    require(found != kinds.end(), "Unsupported node field kind.");
    return static_cast<Kind>(found - kinds.begin());
}

std::vector<Field> fields(WaterWallSchematic& source, const Json& definitions, unsigned depth = 0)
{
    require(definitions.is_array() && depth < 16, "Invalid node field list.");
    std::vector<Field> result;
    for (const auto& item : definitions) {
        Field field{
            .key = text(item.at("key")),
            .label = text(item.at("label")),
            .kind = field_kind(text(item.at("kind"))),
            .hint = optional_text(item, "hint", ""),
            .required = item.value("required", false),
            .aliases = aliases(item),
            .nullable = item.value("nullable", false),
            .case_insensitive = item.value("case_insensitive", false),
            .allow_random_port = item.value("allow_random_port", false),
            .string_alias_fallback = item.value("string_alias_fallback", false),
        };
        require(*field.key && *field.label, "Node field keys and labels must be nonempty.");
        field.minimum = item.value("minimum", field.minimum);
        field.maximum = item.value("maximum", field.maximum);
        require(std::isfinite(field.minimum) && std::isfinite(field.maximum) &&
                    field.minimum <= field.maximum,
                "Invalid node field bounds.");
        if (const auto choices = item.find("choices"); choices != item.end()) {
            require(choices->is_array(), "Expected an array of field choices.");
            auto& values = source.choice_lists.emplace_back();
            std::ranges::transform(*choices, std::back_inserter(values), text);
            field.choices = values;
        }
        require(field.kind != Kind::choice || !field.choices.empty(),
                "A choice field must define at least one string option.");
        if (const auto children = item.find("children"); children != item.end()) {
            field.children = fields(source, *children, depth + 1);
        }
        result.push_back(std::move(field));
    }
    return result;
}

std::vector<NodeLinkField> links(const Json& definitions, const Json& nodes)
{
    require(definitions.is_array(), "Invalid node link list.");
    std::vector<NodeLinkField> result;
    for (const auto& item : definitions) {
        const auto kind = schematic_text(item.at("kind"));
        const auto activation = schematic_text(schematic_field(item, "activation_kind"));
        require((kind == "branch" || kind == "reference") &&
                    (activation.empty() || activation == "true" || activation == "string"),
                "Unsupported node link kind or activation.");
        const auto* target = optional_text(item, "target_type");
        require(!target || nodes.contains(target), "A link references an unregistered node type.");
        result.push_back({
            .key = text(item.at("key")),
            .label = text(item.at("label")),
            .kind = kind == "branch" ? NodeLinkKind::branch : NodeLinkKind::reference,
            .target_type = target,
            .collection = optional_text(item, "collection"),
            .object = optional_text(item, "object"),
            .aliases = aliases(item),
            .string_alias_fallback = item.value("string_alias_fallback", false),
            .target_missing_setting = optional_text(item, "target_missing_setting"),
            .activation_setting = optional_text(item, "activation_setting"),
            .activation_is_boolean = activation == "true",
            .hint = optional_text(item, "hint", ""),
        });
    }
    return result;
}

void validate_ports(const Json& ports)
{
    for (const char* key : {"previous", "next", "dual_ended"}) {
        require(ports.at(key).is_boolean(), "Expected a Boolean node port capability.");
    }
    if (const auto excluded = ports.find("next_excluded_by"); excluded != ports.end()) {
        require(excluded->is_array() &&
                    std::ranges::all_of(*excluded,
                                        [](const Json& value) { return value.is_string() && *text(value); }),
                "Invalid next exclusion settings.");
    }
}

bool string_list(const Json& items)
{
    return items.is_array() &&
           std::ranges::all_of(items, [](const Json& value) { return value.is_string() && *text(value); });
}

void validate_metadata(const Json& document)
{
    const auto& ui = document.at("ui");
    require(ui.is_object(), "Invalid user editor metadata.");
    if (auto error = validate_schematic_users_ui(schematic_field(ui, "users")); !error.empty()) {
        throw std::runtime_error(std::move(error));
    }
    const auto& config = document.at("config");
    require(config.is_object() && string_list(config.at("version_keys")), "Invalid config version keys.");
    const auto& importer = config.at("import");
    require(importer.at("line_comments").is_boolean(), "Invalid config import metadata.");
    optional_text(importer, "variables_key");
    optional_text(importer, "commit_key");
    optional_text(importer, "confirm_notice");
    const auto& logs = document.at("runtime").at("logs");
    const auto& sources = logs.at("sources");
    require(sources.is_array(), "Expected a list of log sources.");
    std::set<std::string_view> names{"panel"};
    for (const auto& source : sources) {
        const auto* name = text(source.at("name"));
        require(*name && *text(source.at("label")) && names.insert(name).second, "Invalid log source.");
    }
}

} // namespace

bool valid_waterwall_version(std::string_view version)
{
    for (int component = 0; component < 3; ++component) {
        const auto dot = version.find('.');
        const auto number = version.substr(0, dot);
        if (number.empty() || number.size() > 6 || (number.size() > 1 && number.front() == '0') ||
            !std::ranges::all_of(number, [](char ch) { return ch >= '0' && ch <= '9'; })) {
            return false;
        }
        if (dot == std::string_view::npos) {
            return component == 2;
        }
        version.remove_prefix(dot + 1);
    }
    return false;
}

std::shared_ptr<const WaterWallSchematic> prepare_waterwall_schematic(Json document)
{
    require(document.at("schema_version").is_number_integer() && document.at("schema_version") == 1,
            "Unsupported schematic format.");
    require(valid_waterwall_version(text(document.at("version"))), "Invalid WaterWall release version.");
    require(core_settings::valid_schematic(document.at("core_settings")),
            "Invalid core settings schematic.");
    validate_metadata(document);
    auto next = std::make_shared<WaterWallSchematic>(std::move(document));
    const auto& definitions = next->data.at("nodes");
    require(definitions.is_object() && !definitions.empty(), "Expected a nonempty object of node types.");
    for (const auto& [name, definition] : definitions.items()) {
        require(!name.empty() && name.size() <= 128 && name.find('\0') == std::string::npos,
                "Invalid node type name.");
        try {
            validate_ports(definition.at("ports"));
            require(schematic_rules::valid(definition.at("rules")), "Invalid node setting rules.");
            require(schematic_rules::valid(definition.at("graph_rules")), "Invalid graph rules.");
            if (definition.value("user_database", false)) {
                require(*text(definition.at("database_key")), "Missing user database key.");
                const auto& controls =
                    schematic_field(schematic_field(next->data.at("ui"), "users"), "fields");
                require(controls.contains("id") && controls.contains("password"),
                        "The user database editor requires ID and password controls.");
                if (const auto defaults = definition.find("database_defaults");
                    defaults != definition.end()) {
                    require(defaults->is_object(), "Expected an object of user database defaults.");
                }
            }
            if (const auto next_field = definition.find("next_field"); next_field != definition.end()) {
                require(next_field->is_object(), "Invalid next field definition.");
                optional_text(*next_field, "label");
                optional_text(*next_field, "hint");
            }
            next->types.push_back(name);
            next->nodes.emplace(name, WaterWallSchematic::Node{fields(*next, definition.at("fields")),
                                                               links(definition.at("links"), definitions)});
        } catch (const std::exception& error) {
            throw std::runtime_error(name + ": " + error.what());
        }
    }
    return next;
}

void activate_waterwall_schematic(std::shared_ptr<const WaterWallSchematic> schematic) noexcept
{
    active = std::move(schematic);
}

const Json& waterwall_schematic(const WaterWallSchematic& schematic)
{
    return schematic.data;
}

std::string set_waterwall_schematic(Json document)
{
    try {
        activate_waterwall_schematic(prepare_waterwall_schematic(std::move(document)));
        return {};
    } catch (const std::exception& error) {
        return error.what();
    }
}

void clear_waterwall_schematic()
{
    active.reset();
}

const Json& waterwall_schematic()
{
    static const Json empty = Json::object();
    return active ? active->data : empty;
}

std::string_view waterwall_version()
{
    return schematic_text(schematic_field(waterwall_schematic(), "version"));
}

std::span<const std::string_view> waterwall_node_types()
{
    return active ? std::span<const std::string_view>{active->types} : std::span<const std::string_view>{};
}

const Json& waterwall_node(std::string_view type)
{
    return schematic_field(schematic_field(waterwall_schematic(), "nodes"), type);
}

std::span<const NodeLinkField> node_link_fields(std::string_view type)
{
    if (active) {
        if (const auto node = active->nodes.find(type); node != active->nodes.end()) {
            return node->second.links;
        }
    }
    return {};
}

bool allows_node_previous(std::string_view type)
{
    return schematic_field(schematic_field(waterwall_node(type), "ports"), "previous") != false;
}

bool allows_node_next(std::string_view type, const Json& settings)
{
    const auto& ports = schematic_field(waterwall_node(type), "ports");
    if (schematic_field(ports, "next") == false) {
        return false;
    }
    return std::ranges::none_of(schematic_field(ports, "next_excluded_by"),
                                [&](const Json& key) { return settings.contains(schematic_text(key)); });
}

namespace node_settings {

std::span<const Field> fields(std::string_view type)
{
    if (active) {
        if (const auto node = active->nodes.find(type); node != active->nodes.end()) {
            return node->second.fields;
        }
    }
    return {};
}

std::string validate(std::string_view type, const Json& settings)
{
    if (!settings.is_object()) {
        return "Node settings must be a JSON object.";
    }
    if (auto issue = detail::validate_fields(fields(type), settings); !issue.empty()) {
        return issue;
    }
    return schematic_rules::validate(schematic_field(waterwall_node(type), "rules"), settings);
}

} // namespace node_settings
} // namespace wave
