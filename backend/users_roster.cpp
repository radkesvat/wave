#include "users_roster.h"

#include <charconv>
#include <chrono>
#include <set>
#include <thread>
#include <utility>

namespace wave {
namespace {

using Json = nlohmann::json;
using Op = WaterWallUsersOperation;
using Outcome = WaterWallUsersOutcome;
using Request = std::function<WaterWallUsersReply(const WaterWallUsersRequest&)>;
using Current = std::function<bool()>;
using namespace std::chrono_literals;

const Json& field(const Json& object, const char* name)
{
    static const Json missing;
    const auto value = object.find(name);
    return value == object.end() ? missing : *value;
}

std::string_view text(const Json& value)
{
    const auto* string = value.get_ptr<const std::string*>();
    return string ? std::string_view(*string) : std::string_view{};
}

Json exact_number(const Json& value)
{
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<std::uint64_t>());
    }
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
        return std::to_string(value.get<std::int64_t>());
    }
    if (const auto digits = text(value); !digits.empty()) {
        std::uint64_t number;
        const auto end = digits.data() + digits.size();
        const auto parsed = std::from_chars(digits.data(), end, number);
        if (parsed.ec == std::errc{} && parsed.ptr == end) {
            return std::to_string(number);
        }
    }
    return nullptr;
}

Json name_preview(const Json& normalized, std::string_view& state)
{
    const auto& value = field(normalized, "name");
    if (state == "invalid_encoding") {
        return nullptr;
    }
    if (!value.is_string()) {
        state = "absent";
        return nullptr;
    }
    const auto name = text(value);
    constexpr std::size_t max_name_bytes = 256;
    if (name.size() <= max_name_bytes) {
        state = state == "truncated" ? "truncated" : "complete";
        return value;
    }
    auto end = max_name_bytes;
    // The source is valid UTF-8. Trim any codepoint that straddles the byte limit.
    while ((static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) {
        --end;
    }
    state = "truncated";
    return name.substr(0, end);
}

UsersRosterClock clock_sample(const WaterWallUsersRevisions& revisions)
{
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return {revisions.accounting_time_ms, wall.count() > 0 ? static_cast<std::uint64_t>(wall.count()) : 0};
}

bool succeeded(const WaterWallUsersReply& reply)
{
    return reply.outcome == Outcome::applied && reply.revisions.has_value();
}

bool valid_page_count(const WaterWallUsersReply& reply, std::size_t seen)
{
    const auto count = reply.revisions->count;
    const auto& rows = field(reply.result, "users");
    const bool terminal = field(reply.result, "next_cursor").is_null();
    return rows.is_array() && seen <= count && rows.size() <= count - seen &&
           (terminal ? rows.size() == count - seen : !rows.empty() && rows.size() < count - seen);
}

LiveUsersRosterRead unavailable(std::string error, std::optional<UsersRosterLoss> loss = std::nullopt)
{
    return {.error = std::move(error), .loss = loss};
}

LiveUsersRosterRead changed_scope()
{
    return unavailable("The live user database changed. Reopen the Users tab.", UsersRosterLoss::identity);
}

LiveUsersRosterRead invalid_roster()
{
    return unavailable("WaterWall returned an incomplete or inconsistent user list. Retry the list.");
}

LiveUsersRosterRead failed_read(const WaterWallUsersReply& reply)
{
    const auto code = text(field(reply.result, "code"));
    if (reply.error.empty()) {
        return invalid_roster();
    }
    if (code == "forbidden") {
        return unavailable(reply.error, UsersRosterLoss::permission);
    }
    const bool identity_lost = code == "unauthorized" || code == "target_not_found" || code == "ambiguous_target" ||
                               code == "unsupported_api" || code == "unsupported_operation" ||
                               code == "unverified_peer" || code == "run_changed";
    return identity_lost ? unavailable(reply.error, UsersRosterLoss::identity) : unavailable(reply.error);
}

bool replaced_database(const WaterWallUsersRevisions& next, const WaterWallUsersRevisions& previous)
{
    // Ordinary configuration changes advance the revision and replace the token together.
    return next.edit_token != previous.edit_token &&
           (next.config <= previous.config || next.stats < previous.stats);
}

bool inconsistent_revision(const WaterWallUsersRevisions& next, const WaterWallUsersRevisions& previous)
{
    if (next.edit_token != previous.edit_token) {
        return false;
    }
    return next.config != previous.config || next.count != previous.count || next.stats < previous.stats;
}

struct Walk {
    LiveUsersRosterRead result;
    std::optional<WaterWallUsersRevisions> covered;
    bool follow_up = false;
    bool restart = false;
};

Walk scan(const Request& request, const Current& current, const std::optional<WaterWallUsersRevisions>& previous)
{
    UsersRosterRows rows;
    std::set<std::string, std::less<>> cursors;
    std::string cursor;
    std::size_t bytes = 0;
    std::optional<WaterWallUsersRevisions> first;
    std::uint64_t last_stats = 0;
    UsersRosterClock clock;
    bool follow_up = false;
    while (current()) {
        auto reply = request({.operation = Op::list, .cursor = cursor});
        if (!current()) {
            break;
        }
        if (!succeeded(reply)) {
            if (!cursor.empty() && field(reply.result, "code") == "stale_cursor") {
                return {.restart = true};
            }
            return {.result = failed_read(reply)};
        }
        const auto& revisions = *reply.revisions;
        if ((previous && replaced_database(revisions, *previous)) ||
            (first && replaced_database(revisions, *first))) {
            return {.result = changed_scope()};
        }
        if ((previous && inconsistent_revision(revisions, *previous)) ||
            (first && inconsistent_revision(revisions, *first))) {
            return {.result = invalid_roster()};
        }
        if (!first) {
            first = revisions;
        } else if (revisions.config != first->config) {
            return {.restart = true};
        }
        if (revisions.stats < last_stats) {
            return {.result = invalid_roster()};
        }
        last_stats = revisions.stats;
        follow_up |= revisions.stats != first->stats;
        clock = clock_sample(revisions);

        if (!valid_page_count(reply, rows.size())) {
            return {.result = invalid_roster()};
        }
        for (const auto& entry : reply.result.at("users")) {
            auto row = make_users_roster_row(entry);
            const auto id = text(field(row, "id"));
            if (id.empty() || rows.contains(id)) {
                return {.result = invalid_roster()};
            }
            const auto size = row.dump().size();
            if (rows.size() == max_users_roster_rows || size > max_users_roster_bytes - bytes) {
                return {.result = unavailable("The WaterWall user list exceeds Wave's roster size limit.")};
            }
            bytes += size;
            rows.emplace(std::string(id), std::move(row));
        }
        const auto& next = field(reply.result, "next_cursor");
        if (next.is_null()) {
            // At least the first page's statistics revision is represented throughout this walk.
            // A newer revision might have changed an account in a page already visited.
            return {.result = {.rows = std::move(rows), .clock = clock},
                    .covered = std::move(first), .follow_up = follow_up};
        }
        cursor = text(next);
        if (cursor.empty() || !cursors.insert(cursor).second) {
            return {.result = invalid_roster()};
        }
    }
    return {.result = changed_scope()};
}

} // namespace

Json make_users_roster_row(const Json& normalized)
{
    auto id = exact_number(field(normalized, "id"));
    const auto& enabled = field(normalized, "enabled");
    if (id.is_null() || id == "0" || !enabled.is_boolean()) {
        return nullptr;
    }
    auto state = text(field(normalized, "name_preview_state"));
    auto name = name_preview(normalized, state);
    const auto& usage = field(field(normalized, "stats"), "traffic");
    const auto& limits = field(field(normalized, "limit"), "traffic");
    const auto& times = field(normalized, "time");
    Json row{{"id", std::move(id)},
             {"gid", exact_number(field(normalized, "gid"))},
             {"enabled", enabled},
             {"name", std::move(name)},
             {"name_preview_state", state},
             {"stats", {{"traffic", {{"up", exact_number(field(usage, "up"))},
                                     {"down", exact_number(field(usage, "down"))}}}}},
             {"limit", {{"traffic", {{"up", exact_number(field(limits, "up"))},
                                     {"down", exact_number(field(limits, "down"))},
                                     {"total", exact_number(field(limits, "total"))}}}}},
             {"time", {{"expire-at-ms", exact_number(field(times, "expire-at-ms"))},
                       {"expire-after-first-usage-ms", exact_number(field(times, "expire-after-first-usage-ms"))},
                       {"first-usage-at-ms", exact_number(field(times, "first-usage-at-ms"))}}}};
    if (field(normalized, "oversized") == true) {
        row["oversized"] = true;
    }
    return row;
}

LiveUsersRosterRead LiveUsersRosterReader::read(const Request& request, bool has_status, bool force,
                                               const Current& current)
{
    if (!current()) {
        return changed_scope();
    }
    if (covered_ && !force && !follow_up_) {
        WaterWallUsersReply probe;
        if (has_status) {
            probe = request({.operation = Op::status});
        }
        if (!current()) {
            return changed_scope();
        }
        if (!succeeded(probe)) {
            probe = request({.operation = Op::list, .list_limit = 1});
            if (!current()) {
                return changed_scope();
            }
            if (!succeeded(probe)) {
                return failed_read(probe);
            }
            if (!valid_page_count(probe, 0)) {
                return invalid_roster();
            }
        }
        const auto& revisions = *probe.revisions;
        if (replaced_database(revisions, *covered_)) {
            return changed_scope();
        }
        if (inconsistent_revision(revisions, *covered_)) {
            return invalid_roster();
        }
        if (revisions.config == covered_->config && revisions.stats == covered_->stats) {
            return {.clock = clock_sample(revisions)};
        }
    }

    constexpr std::chrono::milliseconds delays[] = {0ms, 25ms, 50ms};
    for (const auto delay : delays) {
        if (delay != 0ms && current()) {
            std::this_thread::sleep_for(delay);
        }
        auto walk = scan(request, current, covered_);
        if (walk.restart) {
            continue;
        }
        if (walk.covered) {
            covered_ = std::move(walk.covered);
            follow_up_ = walk.follow_up;
        }
        return std::move(walk.result);
    }
    return unavailable("The WaterWall user list is changing too quickly to finish loading. Retry the list.");
}

} // namespace wave
