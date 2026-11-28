#include "users_roster.h"

#include "unicode_casefold.h"
#include "views/view_helpers.h"

#include <charconv>
#include <utility>

namespace wave {
namespace {

using Json = nlohmann::json;

constexpr std::size_t max_roster_rows = 100'000;
constexpr std::size_t max_transfer_bytes = 40 * 1024 * 1024;

std::optional<std::uint64_t> exact_number(const Json& value)
{
    if (!value.is_string()) {
        return std::nullopt;
    }
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() > 1 && text.front() == '0') {
        return std::nullopt;
    }
    std::uint64_t number = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return number;
}

std::optional<std::uint64_t> account_number(const Json& value, bool detail)
{
    if (detail && value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    return exact_number(value);
}

void observe_traffic(UsersRosterRow& row, const UsersRosterRow& previous, double now)
{
    if (!row.traffic || !previous.traffic ||
        row.traffic->upload < previous.traffic->upload || row.traffic->download < previous.traffic->download) {
        return;
    }
    if (row.traffic->upload == previous.traffic->upload && row.traffic->download == previous.traffic->download) {
        row.traffic_changed_at = previous.traffic_changed_at;
        row.rates = previous.rates;
        return;
    }
    // The first observed change anchors the interval. The initial roster can contain older, batched usage.
    row.traffic_changed_at = now;
    if (previous.traffic_changed_at >= 0 && now > previous.traffic_changed_at) {
        const double interval_ms = now - previous.traffic_changed_at;
        const double seconds = interval_ms / 1000.0;
        row.rates = UsersRosterRates{static_cast<double>(row.traffic->upload - previous.traffic->upload) / seconds,
                                     static_cast<double>(row.traffic->download - previous.traffic->download) / seconds,
                                     interval_ms};
    }
}

std::optional<std::size_t> counter(const Json& value)
{
    if (value.is_number_unsigned() && value.get<std::uint64_t>() <= max_roster_rows * 2) {
        return value.get<std::size_t>();
    }
    return std::nullopt;
}

void sample_clocks(UsersRoster& roster, const Json& frame, double now)
{
    const auto clock = users_clock_samples(view::field(frame, "host_time"), view::field(frame, "accounting_time"), now);
    if (clock.host || clock.accounting) {
        roster.clock = clock;
    }
}

} // namespace

std::optional<UserTraffic> user_traffic(const Json& traffic, bool detail)
{
    const auto up = account_number(view::field(traffic, "up"), detail);
    const auto down = account_number(view::field(traffic, "down"), detail);
    if (!up || !down) {
        return std::nullopt;
    }
    return UserTraffic{*up, *down};
}

UsersClockSamples users_clock_samples(const Json& host, const Json& accounting, double received_at)
{
    return {exact_number(host), exact_number(accounting), received_at};
}

std::optional<UsersRosterRow> users_roster_row(const Json& row, bool detail)
{
    const auto id = account_number(view::field(row, "id"), detail);
    const auto& enabled = view::field(row, "enabled");
    if (!id || *id == 0 || !enabled.is_boolean()) {
        return std::nullopt;
    }
    const auto& stats = view::field(row, "stats");
    const auto& time = view::field(row, "time");
    const auto size = view::bool_field(row, "limited_detail") || view::bool_field(row, "oversized");
    const auto name = view::string_field(row, "name");
    return UsersRosterRow{
        .id = *id,
        .name = std::string(name),
        .folded_name = unicode_casefold(name),
        .name_state = std::string(view::string_field(row, "name_preview_state")),
        .group = account_number(view::field(row, "gid"), detail),
        .traffic = user_traffic(view::field(stats, "traffic"), detail),
        .absolute_expiry = account_number(view::field(time, "expire-at-ms"), detail),
        .relative_expiry = account_number(view::field(time, "expire-after-first-usage-ms"), detail),
        .first_use = account_number(view::field(time, "first-usage-at-ms"), detail),
        .enabled = enabled.get<bool>(),
        .limited_detail = size,
    };
}

const UsersRoster::Rows& UsersRoster::visible_rows() const
{
    return !sequence && transfer ? transfer->rows : baseline;
}

void UsersRoster::reset_rates()
{
    if (!live) {
        return;
    }
    bool changed = false;
    for (auto& [id, row] : baseline) {
        if (row.traffic_changed_at >= 0) {
            row.traffic_changed_at = -1;
            row.rates.reset();
            changed = true;
        }
    }
    if (changed) {
        ++generation;
    }
}

void UsersRoster::reset_transfer(bool discard_rate_history)
{
    if (!sequence && transfer) {
        ++generation;
    }
    transfer.reset();
    state = State::loading;
    message.clear();
    if (discard_rate_history) {
        reset_rates();
    }
}

bool UsersRoster::receive(const Json& frame, std::size_t bytes, double now)
{
    const auto tag = view::string_field(frame, "event");
    if (tag == "state") {
        auto status = view::string_field(frame, "state");
        if (status == "reset") {
            reset_transfer();
            return true;
        }
        if (status == "lost" || status == "error" || status == "stale") {
            reset_transfer();
            fresh = false;
            if (status == "lost") {
                const auto cause = view::string_field(frame, "cause");
                loss_cause = cause == "permission"          ? LossCause::permission
                             : cause == "config_unverified" ? LossCause::config_unverified
                                                              : LossCause::identity;
                baseline.clear();
                sequence.reset();
                clock = {};
                ++generation;
                state = State::lost;
            } else {
                state = status == "stale" && sequence ? State::stale : State::error;
            }
        } else if (status == "ready") {
            sample_clocks(*this, frame, now);
            if (sequence) {
                fresh = true;
                state = State::ready;
            }
        } else if (status == "loading") {
            state = State::loading;
        } else {
            return false;
        }
        message = view::string_field(frame, "message");
        return true;
    }

    const auto attempt = exact_number(view::field(frame, "attempt"));
    if (!attempt || *attempt == 0) {
        return false;
    }
    if (*attempt < latest_attempt) {
        return true;
    }

    if (tag == "begin") {
        const auto next = exact_number(view::field(frame, "sequence"));
        const auto total = counter(view::field(frame, "total"));
        const auto base = exact_number(view::field(frame, "base_sequence"));
        if (*attempt == latest_attempt || !next || *next == 0 || (sequence && *next < *sequence) || !total ||
            *total > max_roster_rows ||
            (frame.contains("base_sequence") && (!base || base != sequence || *next <= *base))) {
            return false;
        }
        reset_transfer(!base || transfer.has_value());
        latest_attempt = *attempt;
        transfer.emplace(Transfer{*attempt, *next, *total, base.has_value()});
        transfer->progress_at = now;
        return true;
    }

    if (!transfer || transfer->attempt != *attempt) {
        return *attempt <= latest_attempt;
    }
    auto& pending = *transfer;
    if (tag == "chunk") {
        const auto index = counter(view::field(frame, "index"));
        const auto& rows = view::field(frame, "rows");
        const auto& removed = view::field(frame, "removed");
        if (!index || *index != pending.next_chunk || bytes > max_transfer_bytes - pending.bytes ||
            !rows.is_array() || (!removed.is_null() && !removed.is_array()) ||
            (!pending.delta && !removed.empty()) || (rows.empty() && removed.empty())) {
            return false;
        }
        for (const auto& value : rows) {
            auto row = users_roster_row(value);
            if (!row || pending.removed.contains(row->id) ||
                !pending.rows.emplace(row->id, std::move(*row)).second) {
                return false;
            }
        }
        for (const auto& value : removed) {
            const auto id = exact_number(value);
            if (!id || !baseline.contains(*id) || pending.rows.contains(*id) ||
                !pending.removed.emplace(*id).second) {
                return false;
            }
        }
        if (pending.rows.size() > pending.total) {
            return false;
        }
        pending.bytes += bytes;
        ++pending.next_chunk;
        pending.progress_at = now;
        if (!sequence) {
            ++generation;
        }
        return true;
    }

    if (tag != "end" || counter(view::field(frame, "chunks")) != pending.next_chunk) {
        return false;
    }
    const bool current = view::string_field(frame, "state") != "stale";
    if (pending.delta) {
        std::size_t size = baseline.size() - pending.removed.size();
        for (const auto& [id, row] : pending.rows) {
            size += !baseline.contains(id);
        }
        if (size != pending.total) {
            return false;
        }
        for (const auto id : pending.removed) {
            baseline.erase(id);
        }
        for (auto& [id, row] : pending.rows) {
            if (live && current) {
                if (const auto previous = baseline.find(id); previous != baseline.end()) {
                    observe_traffic(row, previous->second, now);
                }
            }
            baseline.insert_or_assign(id, std::move(row));
        }
    } else {
        if (pending.rows.size() != pending.total) {
            return false;
        }
        baseline = std::move(pending.rows);
    }
    sequence = pending.sequence;
    transfer.reset();
    ++generation;
    ++commits;
    if (!current) {
        reset_rates();
    }
    fresh = current;
    state = fresh ? State::ready : State::stale;
    message = view::string_field(frame, "message");
    sample_clocks(*this, frame, now);
    return true;
}

} // namespace wave
