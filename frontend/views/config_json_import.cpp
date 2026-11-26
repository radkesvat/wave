#include "config_json_import.h"

#include "core_config_json.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace wave {
namespace {

using Json = nlohmann::json;
constexpr std::size_t max_import_bytes = 4 * max_core_config_bytes;
constexpr std::string_view json_whitespace = " \t\r\n";

std::size_t string_end(std::string_view text, std::size_t start)
{
    for (auto i = start + 1; i < text.size(); ++i) {
        if (text[i] == '"') {
            return i + 1;
        }
        if (text[i] == '\\') {
            ++i;
        }
    }
    return text.npos;
}

std::string strip_line_comments(std::string_view text, bool& removed, std::string& error)
{
    std::string source;
    source.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '"') {
            const auto end = string_end(text, i);
            if (end == text.npos) {
                error = "Enter valid JSON.";
                return {};
            }
            source.append(text.substr(i, end - i));
            i = end;
        } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            removed = true;
            i = text.find_first_of("\r\n", i + 2);
        } else {
            source.push_back(text[i++]);
        }
    }
    return source;
}

std::size_t object_end(std::string_view text, std::size_t start)
{
    int depth = 0;
    for (auto i = start; i < text.size();) {
        if (text[i] == '"') {
            i = string_end(text, i);
            continue;
        }
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}' && --depth == 0) {
            return i + 1;
        }
        ++i;
    }
    return text.npos;
}

std::string_view variable_declaration(std::string_view source, std::string& error)
{
    constexpr std::string_view bom = "\xef\xbb\xbf";
    const auto root = source.find_first_not_of(json_whitespace, source.starts_with(bom) ? bom.size() : 0);
    if (root == source.npos || source[root] != '{') {
        return {};
    }
    int depth = 0;
    for (auto i = root; i < source.size();) {
        if (source[i] == '"') {
            const auto end = string_end(source, i);
            if (depth == 1 && end != source.npos && source.substr(i, end - i) == "\"variables\"") {
                const auto colon = source.find_first_not_of(json_whitespace, end);
                if (colon != source.npos && source[colon] == ':') {
                    const auto start = source.find_first_not_of(json_whitespace, colon + 1);
                    if (start == source.npos || source[start] != '{') {
                        error = "The top-level variables block must be a JSON object.";
                        return {};
                    }
                    const auto stop = object_end(source, start);
                    if (stop == source.npos) {
                        error = "The top-level variables block is incomplete.";
                        return {};
                    }
                    return source.substr(start, stop - start);
                }
            }
            i = end;
            continue;
        }
        if (source[i] == '$') {
            const auto end = source.find_first_of("$\r\n", i + 1);
            if (end == source.npos || source[end] != '$') {
                return {};
            }
            i = end + 1;
            continue;
        }
        if (source[i] == '{' || source[i] == '[') {
            ++depth;
        } else if (source[i] == '}' || source[i] == ']') {
            --depth;
        }
        ++i;
    }
    return {};
}

Json parse_json(std::string_view source, std::string_view invalid, std::string& error)
{
    std::vector<std::set<std::string>> object_keys;
    auto check = [&](int depth, Json::parse_event_t event, Json& value) {
        if (!error.empty()) {
            return false;
        }
        if (depth > max_core_config_depth) {
            error = "The config exceeds Wave's 64-level nesting limit.";
            return false;
        }
        if (event == Json::parse_event_t::object_start) {
            object_keys.emplace_back();
        } else if (event == Json::parse_event_t::object_end) {
            object_keys.pop_back();
        } else if (event == Json::parse_event_t::key || event == Json::parse_event_t::value) {
            if (const auto* string = value.get_ptr<const std::string*>();
                string && string->find('\0') != string->npos) {
                error = "Config strings cannot contain a NUL character.";
                return false;
            }
            if (event == Json::parse_event_t::key &&
                !object_keys.back().insert(value.get_ref<const std::string&>()).second) {
                error = "The config contains a duplicate JSON object key.";
                return false;
            }
        }
        return true;
    };
    auto document = Json::parse(source, check, false);
    if (error.empty() && document.is_discarded()) {
        error = invalid;
    }
    return document;
}

std::string substitute_variables(std::string_view source, const Json& variables, std::string& error)
{
    std::string expanded;
    expanded.reserve(source.size());
    std::map<std::string_view, std::string> replacements;
    for (std::size_t i = 0; i < source.size();) {
        const auto start = i;
        std::string_view chunk;
        if (source[i] == '"') {
            i = string_end(source, i);
            chunk = source.substr(start, i - start);
        } else if (source[i] == '$') {
            const auto end = source.find_first_of("$\r\n", start + 1);
            if (end == source.npos || source[end] != '$') {
                error = "The config contains an unterminated variable placeholder.";
                return {};
            }
            const auto name = source.substr(start + 1, end - start - 1);
            if (name.empty()) {
                error = "The config contains an empty variable placeholder.";
                return {};
            }
            const auto variable = variables.find(name);
            if (variable == variables.end()) {
                error = "The config references an undefined variable.";
                return {};
            }
            const auto [replacement, inserted] = replacements.try_emplace(name);
            if (inserted) {
                replacement->second = variable->dump();
            }
            chunk = replacement->second;
            i = end + 1;
        } else {
            i = source.find_first_of("\"$", i + 1);
            chunk = source.substr(start, i - start);
        }
        if (chunk.size() > max_import_bytes - expanded.size()) {
            error = "The expanded config exceeds Wave's 1 MiB import limit.";
            return {};
        }
        expanded.append(chunk);
    }
    return expanded;
}

} // namespace

ConfigJsonImport parse_config_json_import(std::string_view text)
{
    ConfigJsonImport result;
    if (text.size() > max_import_bytes) {
        result.error = "The pasted config exceeds Wave's 1 MiB import limit.";
        return result;
    }
    if (text.find('\0') != text.npos) {
        result.error = "The pasted config contains a NUL byte.";
        return result;
    }

    bool removed_comments = false;
    const auto source = strip_line_comments(text, removed_comments, result.error);
    if (!result.error.empty()) {
        return result;
    }
    const auto declaration = variable_declaration(source, result.error);
    if (!result.error.empty()) {
        return result;
    }
    Json variables = Json::object();
    if (!declaration.empty()) {
        variables =
            parse_json(declaration, "The variables block must contain valid JSON values.", result.error);
        if (!result.error.empty()) {
            return result;
        }
    }
    const auto expanded = substitute_variables(source, variables, result.error);
    if (!result.error.empty()) {
        return result;
    }
    auto document = parse_json(expanded, "Enter valid JSON.", result.error);
    if (!result.error.empty()) {
        return result;
    }
    result.needs_confirmation = removed_comments;
    if (document.is_object()) {
        const bool removed_variables = document.erase("variables") != 0;
        const bool removed_commits = document.erase("commits") != 0;
        result.needs_confirmation |= removed_variables || removed_commits;
    }
    result.document = std::move(document);
    return result;
}

} // namespace wave
