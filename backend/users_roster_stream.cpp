#include "users_roster_stream.h"

#include <utility>

namespace wave {
namespace {

using Json = nlohmann::json;
constexpr std::size_t chunk_bytes = 128 * 1024;

std::string encode(const Json& value)
{
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

bool append_item(std::string& array, std::string_view encoded, std::size_t remaining)
{
    const bool comma = !array.empty();
    if (encoded.size() + comma > remaining) {
        return false;
    }
    if (comma) {
        array += ',';
    }
    array += encoded;
    return true;
}

template <typename Rows, typename Share>
UsersRosterDelta compare_rows(const UsersRosterVersion& previous, Rows& next, Share share)
{
    UsersRosterDelta delta{previous.sequence};
    auto old_row = previous.rows.begin();
    auto new_row = next.begin();
    while (old_row != previous.rows.end() || new_row != next.end()) {
        if (new_row == next.end() || (old_row != previous.rows.end() && old_row->first < new_row->first)) {
            delta.removed.push_back(old_row++->first);
        } else if (old_row == previous.rows.end() || new_row->first < old_row->first) {
            delta.changed.push_back(new_row++->first);
        } else {
            if (old_row->second != new_row->second) {
                if (*old_row->second == *new_row->second) {
                    share(old_row->second, new_row->second);
                } else {
                    delta.changed.push_back(new_row->first);
                }
            }
            ++old_row;
            ++new_row;
        }
    }
    return delta;
}

} // namespace

bool UsersRosterVersion::insert(const Json& row)
{
    const auto& id = row.at("id").get_ref<const std::string&>();
    auto encoded = encode(row);
    const auto existing = rows.find(id);
    const auto old_size = existing == rows.end() ? 0 : existing->second->size();
    if ((existing == rows.end() && rows.size() >= max_users_roster_rows) ||
        encoded.size() > chunk_bytes - 1024 ||
        encoded.size() > max_users_roster_bytes - (bytes - old_size)) {
        return false;
    }
    bytes = bytes - old_size + encoded.size();
    rows.insert_or_assign(id, std::make_shared<const std::string>(std::move(encoded)));
    return true;
}

void UsersRosterVersion::erase(std::string_view id)
{
    if (const auto found = rows.find(id); found != rows.end()) {
        bytes -= found->second->size();
        rows.erase(found);
    }
}

UsersRosterDelta compare_users_rosters(const UsersRosterVersion& previous, const UsersRosterVersion& next)
{
    return compare_rows(previous, next.rows, [](const auto&, const auto&) {});
}

UsersRosterDelta compare_and_share_users_rosters(const UsersRosterVersion& previous, UsersRosterVersion& next)
{
    return compare_rows(previous, next.rows, [](const auto& old_row, auto& new_row) { new_row = old_row; });
}

void add_users_roster_clock(Json& frame, const UsersRosterClock& clock)
{
    if (clock.wall_time_ms != 0) {
        frame["host_time"] = std::to_string(clock.wall_time_ms);
    }
    if (clock.accounting_time_ms) {
        frame["accounting_time"] = std::to_string(*clock.accounting_time_ms);
    }
}

UsersRosterTransfer::UsersRosterTransfer(std::shared_ptr<const UsersRosterVersion> version,
                                         std::shared_ptr<const UsersRosterDelta> delta, std::uint64_t attempt)
    : version_(std::move(version)), delta_(std::move(delta)), attempt_(attempt), row_(version_->rows.begin())
{
}

bool UsersRosterTransfer::will_end() const
{
    return begun_ && (delta_ ? changed_ == delta_->changed.size() && removed_ == delta_->removed.size()
                            : row_ == version_->rows.end());
}

std::string UsersRosterTransfer::next(std::string_view subscription, const UsersRosterClock& clock,
                                      std::optional<std::string_view> stale)
{
    Json frame{{"type", "users_roster"}, {"subscription", subscription}, {"attempt", std::to_string(attempt_)}};
    if (!begun_) {
        begun_ = true;
        frame["event"] = "begin";
        frame["total"] = version_->rows.size();
        frame["sequence"] = std::to_string(version_->sequence);
        if (delta_) {
            frame["base_sequence"] = std::to_string(delta_->base_sequence);
        }
        return encode(frame);
    }
    if (will_end()) {
        frame["event"] = "end";
        frame["chunks"] = chunks_;
        add_users_roster_clock(frame, clock);
        if (stale) {
            frame["state"] = "stale";
            frame["message"] = *stale;
        }
        return encode(frame);
    }

    frame["event"] = "chunk";
    frame["index"] = chunks_++;
    auto encoded = encode(frame);
    encoded.pop_back();
    constexpr std::string_view rows_start = ",\"rows\":[";
    constexpr std::string_view removed_start = "],\"removed\":[";
    constexpr std::string_view end = "]}";
    const auto budget = chunk_bytes - encoded.size() - rows_start.size() - removed_start.size() - end.size();
    std::string rows;
    std::string removed;
    if (delta_) {
        while (changed_ < delta_->changed.size() &&
               append_item(rows, *version_->rows.at(delta_->changed[changed_]), budget - rows.size())) {
            ++changed_;
        }
        while (removed_ < delta_->removed.size() &&
               append_item(removed, encode(delta_->removed[removed_]), budget - rows.size() - removed.size())) {
            ++removed_;
        }
    } else {
        while (row_ != version_->rows.end() && append_item(rows, *row_->second, budget - rows.size())) {
            ++row_;
        }
    }
    encoded += rows_start;
    encoded += rows;
    encoded += removed_start;
    encoded += removed;
    encoded += end;
    return encoded;
}

} // namespace wave
