#pragma once

#include <nlohmann/json.hpp>

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace wave::schematic_rules {

namespace detail {

using Json = nlohmann::json;

enum class Op {
    get,
    present,
    type,
    eq,
    ne,
    lt,
    le,
    gt,
    ge,
    in,
    all,
    any,
    neg,
    choose,
    size,
    lower,
    list,
    item,
    mapping,
    add,
    sub,
    mul,
    mod,
    decimal_places,
    every,
    some,
    distinct,
    increasing,
    sum,
    format,
    starts_with,
    substring
};

struct Operation {
    std::string_view name;
    Op operation;
    std::size_t minimum;
    std::size_t maximum;
};

inline const Operation* operation(const Json& expression)
{
    using enum Op;
    constexpr std::size_t many = 256;
    static constexpr Operation operations[] = {
        {"get", get, 1, 2},
        {"present", present, 1, 1},
        {"type", type, 2, 2},
        {"eq", eq, 2, 2},
        {"ne", ne, 2, 2},
        {"lt", lt, 2, 2},
        {"le", le, 2, 2},
        {"gt", gt, 2, 2},
        {"ge", ge, 2, 2},
        {"in", in, 2, many},
        {"all", all, 1, many},
        {"any", any, 1, many},
        {"not", neg, 1, 1},
        {"if", choose, 3, 3},
        {"size", size, 1, 1},
        {"lower", lower, 1, 1},
        {"list", list, 1, 1},
        {"item", item, 2, 2},
        {"map", mapping, 3, 3},
        {"add", add, 2, many},
        {"sub", sub, 2, 2},
        {"mul", mul, 2, 2},
        {"mod", mod, 2, 2},
        {"decimal-places", decimal_places, 2, 2},
        {"every", every, 2, 2},
        {"some", some, 2, 2},
        {"distinct", distinct, 2, 2},
        {"increasing", increasing, 2, 2},
        {"sum", sum, 2, 2},
        {"format", format, 2, 3},
        {"starts-with", starts_with, 2, 2},
        {"substring", substring, 2, 2},
    };
    if (!expression.is_array() || expression.empty() || !expression.front().is_string()) {
        return nullptr;
    }
    const auto found =
        std::ranges::find(operations, expression.front().get_ref<const std::string&>(), &Operation::name);
    return found != std::end(operations) && expression.size() > found->minimum &&
                   expression.size() - 1 <= found->maximum
               ? found
               : nullptr;
}

enum class Format {
    ipv4,
    ipv6,
    ipv4_cidr,
    ip_endpoint,
    hex,
    base64,
    nonnegative_int32
};

inline const Format* format(const Json& name)
{
    using enum Format;
    static constexpr std::pair<std::string_view, Format> formats[] = {
        {"ipv4", ipv4},
        {"ipv6", ipv6},
        {"ipv4-cidr", ipv4_cidr},
        {"ip-endpoint", ip_endpoint},
        {"hex", hex},
        {"base64", base64},
        {"nonnegative-int32", nonnegative_int32},
    };
    if (!name.is_string()) {
        return nullptr;
    }
    const auto found = std::ranges::find(formats, name.get_ref<const std::string&>(),
                                         &std::pair<std::string_view, Format>::first);
    return found == std::end(formats) ? nullptr : &found->second;
}

inline bool valid_pointer(const Json& value)
{
    if (!value.is_string()) {
        return false;
    }
    const auto& path = value.get_ref<const std::string&>();
    if (!path.empty() && path.front() != '/') {
        return false;
    }
    for (auto i = path.find('~'); i != std::string::npos; i = path.find('~', i + 2)) {
        if (i + 1 == path.size() || (path[i + 1] != '0' && path[i + 1] != '1')) {
            return false;
        }
    }
    return path.find('\0') == std::string::npos;
}

inline bool valid_expression(const Json& expression, unsigned depth)
{
    if (depth > 48) {
        return false;
    }
    if (expression.is_primitive()) {
        return true;
    }
    const auto* op = operation(expression);
    if (!op) {
        return false;
    }
    std::size_t first_expression = 1;
    std::size_t end_expression = expression.size();
    using enum Op;
    switch (op->operation) {
    case get:
    case present:
        if (!valid_pointer(expression[1])) {
            return false;
        }
        first_expression = 2;
        break;
    case type:
        if (expression[2] != "null" && expression[2] != "string" && expression[2] != "boolean" &&
            expression[2] != "number" && expression[2] != "integer" && expression[2] != "integral" &&
            expression[2] != "array" && expression[2] != "object") {
            return false;
        }
        end_expression = 2;
        break;
    case mapping:
        return expression[2].is_object() && valid_expression(expression[1], depth + 1) &&
               valid_expression(expression[3], depth + 1);
    case format: {
        const auto* kind = detail::format(expression[2]);
        if (!kind || (*kind == Format::base64) != (expression.size() == 4) ||
            (expression.size() == 4 &&
             (!expression[3].is_number_integer() || expression[3] < 0 || expression[3] > 65536))) {
            return false;
        }
        end_expression = 2;
        break;
    }
    default:
        break;
    }
    for (auto index = first_expression; index < end_expression; ++index) {
        if (!valid_expression(expression[index], depth + 1)) {
            return false;
        }
    }
    return true;
}

inline bool valid_rules(const Json& rules, unsigned depth)
{
    if (!rules.is_array() || depth > 48) {
        return false;
    }
    for (const auto& rule : rules) {
        if (!rule.is_object()) {
            return false;
        }
        const bool group = rule.contains("rules");
        const bool at = rule.contains("at");
        const bool each = rule.contains("each");
        const bool when = rule.contains("when");
        if ((when && !valid_expression(rule["when"], depth + 1)) ||
            (at && !valid_expression(rule["at"], depth + 1)) ||
            (each && !valid_expression(rule["each"], depth + 1))) {
            return false;
        }
        if (group) {
            if ((at && each) || rule.size() != 1U + when + at + each ||
                !valid_rules(rule["rules"], depth + 1)) {
                return false;
            }
        } else if (rule.size() != 2U + when || !rule.contains("assert") ||
                   !valid_expression(rule["assert"], depth + 1) || !rule.contains("message") ||
                   !rule["message"].is_string() || rule["message"].get_ref<const std::string&>().empty() ||
                   rule["message"].get_ref<const std::string&>().find('\0') != std::string::npos) {
            return false;
        }
    }
    return true;
}

inline bool ip(int family, std::string_view address)
{
    if (address.size() >= INET6_ADDRSTRLEN || address.find('\0') != std::string_view::npos) {
        return false;
    }
    std::array<unsigned char, 16> bytes{};
    return ::inet_pton(family, std::string(address).c_str(), bytes.data()) == 1;
}

inline bool integer_text(std::string_view source, int minimum, int maximum)
{
    if (source.empty()) {
        return false;
    }
    int number = 0;
    const auto end = source.data() + source.size();
    const auto result = std::from_chars(source.data(), end, number);
    return result.ec == std::errc{} && result.ptr == end && number >= minimum && number <= maximum;
}

inline bool ip_endpoint(std::string_view address)
{
    if (ip(AF_INET, address) || ip(AF_INET6, address)) {
        return true;
    }
    if (address.starts_with('[')) {
        const auto end = address.find(']');
        return end != std::string_view::npos && ip(AF_INET6, address.substr(1, end - 1)) &&
               (end + 1 == address.size() ||
                (address[end + 1] == ':' && integer_text(address.substr(end + 2), 1, 65535)));
    }
    const auto colon = address.find(':');
    return colon != std::string_view::npos && ip(AF_INET, address.substr(0, colon)) &&
           integer_text(address.substr(colon + 1), 1, 65535);
}

inline bool canonical_base64(std::string_view input, std::size_t decoded_size)
{
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned padding = (3 - decoded_size % 3) % 3;
    if (input.size() != ((decoded_size + 2) / 3) * 4 ||
        input.substr(input.size() - padding).find_first_not_of('=') != std::string_view::npos) {
        return false;
    }
    input.remove_suffix(padding);
    return input.find_first_not_of(alphabet) == std::string_view::npos &&
           (padding == 0 || alphabet.find(input.back()) % (1U << (2 * padding)) == 0);
}

inline bool format_matches(Format kind, const Json& value, std::size_t size)
{
    if (!value.is_string()) {
        return false;
    }
    const std::string_view input = value.get_ref<const std::string&>();
    using enum Format;
    switch (kind) {
    case ipv4:
        return ip(AF_INET, input);
    case ipv6:
        return ip(AF_INET6, input);
    case ipv4_cidr: {
        const auto slash = input.find('/');
        return slash != input.npos && ip(AF_INET, input.substr(0, slash)) &&
               integer_text(input.substr(slash + 1), 0, 32);
    }
    case ip_endpoint:
        return detail::ip_endpoint(input);
    case hex:
        return input.find_first_not_of("0123456789abcdefABCDEF") == input.npos;
    case base64:
        return canonical_base64(input, size);
    case nonnegative_int32:
        return integer_text(input, 0, 2147483647);
    }
    return false;
}

inline Json evaluate(const Json& expression, const Json& context);

inline bool true_value(const Json& value)
{
    return value == true;
}

inline const Json* lookup(const Json& context, const Json& path)
{
    try {
        const Json::json_pointer pointer(path.get_ref<const std::string&>());
        return context.contains(pointer) ? &context.at(pointer) : nullptr;
    } catch (const Json::exception&) {
        // A token can name an object member but also be an invalid array index.
        return nullptr;
    }
}

inline Json collection(Op operation, const Json& value, const Json& expression)
{
    using enum Op;
    if (!value.is_array()) {
        return false;
    }
    if (operation == every || operation == some) {
        for (const auto& entry : value) {
            const bool match = true_value(evaluate(expression, entry));
            if (match != (operation == every)) {
                return match;
            }
        }
        return operation == every;
    }
    if (operation == distinct) {
        std::set<Json> seen;
        for (const auto& entry : value) {
            if (!seen.insert(evaluate(expression, entry)).second) {
                return false;
            }
        }
        return true;
    }
    if (operation == increasing) {
        Json previous;
        for (const auto& entry : value) {
            auto next = evaluate(expression, entry);
            if (!next.is_number() || (!previous.is_null() && next <= previous)) {
                return false;
            }
            previous = std::move(next);
        }
        return true;
    }
    double total = 0;
    for (const auto& entry : value) {
        const auto number = evaluate(expression, entry);
        if (!number.is_number()) {
            return nullptr;
        }
        total += number.get<double>();
    }
    return total;
}

inline Json evaluate(const Json& expression, const Json& context)
{
    if (expression.is_primitive()) {
        return expression;
    }
    const auto* instruction = operation(expression);
    if (!instruction) {
        return nullptr;
    }
    const auto op = instruction->operation;
    const auto value = [&](std::size_t index) { return evaluate(expression[index], context); };
    const auto condition = [&](std::size_t index) { return true_value(value(index)); };
    using enum Op;
    switch (op) {
    case get:
    case present: {
        const auto* selected = lookup(context, expression[1]);
        if (op == present) {
            return selected != nullptr;
        }
        return selected ? *selected : expression.size() == 3 ? value(2) : Json(nullptr);
    }
    case all:
    case any:
        for (std::size_t index = 1; index < expression.size(); ++index) {
            if (condition(index) != (op == all)) {
                return op == any;
            }
        }
        return op == all;
    case neg:
        return !condition(1);
    case choose:
        return value(condition(1) ? 2 : 3);
    default:
        break;
    }

    auto first = value(1);
    switch (op) {
    case type: {
        const auto& kind = expression[2];
        if (kind == "integral") {
            if (!first.is_number()) {
                return false;
            }
            const auto number = first.get<double>();
            return std::isfinite(number) && std::trunc(number) == number;
        }
        return (kind == "string" && first.is_string()) || (kind == "integer" && first.is_number_integer()) ||
               (kind == "number" && first.is_number()) || (kind == "boolean" && first.is_boolean()) ||
               (kind == "null" && first.is_null()) || (kind == "array" && first.is_array()) ||
               (kind == "object" && first.is_object());
    }
    case eq:
        return first == value(2);
    case ne:
        return first != value(2);
    case lt:
    case le:
    case gt:
    case ge: {
        const auto second = value(2);
        if (!first.is_number() || !second.is_number()) {
            return false;
        }
        switch (op) {
        case lt:
            return first < second;
        case le:
            return first <= second;
        case gt:
            return first > second;
        default:
            return first >= second;
        }
    }
    case in:
        for (std::size_t index = 2; index < expression.size(); ++index) {
            if (first == value(index)) {
                return true;
            }
        }
        return false;
    case size:
        return first.is_string() ? first.get_ref<const std::string&>().size() : first.size();
    case lower:
        if (auto* str = first.get_ptr<std::string*>()) {
            std::ranges::transform(*str, str->begin(), [](char ch) {
                return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
            });
            return first;
        }
        return "";
    case list:
        return first.is_array() ? std::move(first) : Json::array({first});
    case item: {
        const auto index = value(2);
        return first.is_array() && index.is_number_integer() && index >= 0 && index < first.size()
                   ? first[index.get<std::size_t>()]
                   : Json(nullptr);
    }
    case mapping:
        if (const auto* key = first.get_ptr<const std::string*>()) {
            const auto found = expression[2].find(*key);
            if (found != expression[2].end()) {
                return *found;
            }
        }
        return value(3);
    case add:
    case sub:
    case mul:
    case mod:
    case decimal_places: {
        if (!first.is_number()) {
            return nullptr;
        }
        double number = first.get<double>();
        for (std::size_t index = 2; index < expression.size(); ++index) {
            const auto second = value(index);
            if (!second.is_number()) {
                return nullptr;
            }
            const auto operand = second.get<double>();
            switch (op) {
            case add:
                number += operand;
                break;
            case sub:
                number -= operand;
                break;
            case mul:
                number *= operand;
                break;
            case mod:
                number = std::fmod(number, operand);
                break;
            default: {
                const auto scale = std::pow(10, operand);
                return std::round(number * scale) / scale == number;
            }
            }
        }
        return std::isfinite(number) ? Json(number) : Json(nullptr);
    }
    case every:
    case some:
    case distinct:
    case increasing:
    case sum:
        return collection(op, first, expression[2]);
    case format:
        return format_matches(*detail::format(expression[2]), first,
                              expression.size() == 4 ? expression[3].get<std::size_t>() : 0);
    case starts_with: {
        const auto second = value(2);
        return first.is_string() && second.is_string() &&
               first.get_ref<const std::string&>().starts_with(second.get_ref<const std::string&>());
    }
    case substring: {
        const auto start = value(2);
        if (!first.is_string() || !start.is_number_integer() || start < 0) {
            return nullptr;
        }
        const auto& text = first.get_ref<const std::string&>();
        return start <= text.size() ? Json(text.substr(start.get<std::size_t>())) : Json(nullptr);
    }
    default:
        return nullptr;
    }
}

inline std::string evaluate_rules(const Json& rules, const Json& context)
{
    for (const auto& rule : rules) {
        if (rule.contains("when") && !true_value(evaluate(rule["when"], context))) {
            continue;
        }
        if (rule.contains("assert")) {
            if (!true_value(evaluate(rule["assert"], context))) {
                return rule["message"].get<std::string>();
            }
        } else if (rule.contains("each")) {
            const auto items = evaluate(rule["each"], context);
            if (items.is_array()) {
                for (const auto& item : items) {
                    if (auto issue = evaluate_rules(rule["rules"], item); !issue.empty()) {
                        return issue;
                    }
                }
            }
        } else if (auto issue = evaluate_rules(rule["rules"],
                                               rule.contains("at") ? evaluate(rule["at"], context) : context);
                   !issue.empty()) {
            return issue;
        }
    }
    return {};
}

} // namespace detail

inline bool valid(const nlohmann::json& rules)
{
    return detail::valid_rules(rules, 0);
}

inline std::string validate(const nlohmann::json& rules, const nlohmann::json& settings)
{
    return detail::evaluate_rules(rules, settings);
}

} // namespace wave::schematic_rules
