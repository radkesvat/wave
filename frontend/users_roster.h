#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace wave {

struct UserTraffic {
    std::uint64_t upload;
    std::uint64_t download;

    // The combined reading can require 65 bits.
    __uint128_t total() const { return static_cast<__uint128_t>(upload) + download; }
};

std::optional<UserTraffic> user_traffic(const nlohmann::json& traffic, bool detail);

struct UsersRosterRates {
    double upload;
    double download;
    double interval_ms;
};

struct UsersRosterRow {
    std::uint64_t id;
    std::string name;
    std::string folded_name;
    std::string name_state;
    std::optional<std::uint64_t> group;
    std::optional<UserTraffic> traffic;
    double traffic_changed_at = -1;
    std::optional<UsersRosterRates> rates;
    std::optional<std::uint64_t> absolute_expiry;
    std::optional<std::uint64_t> relative_expiry;
    std::optional<std::uint64_t> first_use;
    bool enabled;
    bool limited_detail;
};

std::optional<UsersRosterRow> users_roster_row(const nlohmann::json& row, bool detail = false);

struct UsersClockSamples {
    std::optional<std::uint64_t> host;
    std::optional<std::uint64_t> accounting;
    double received_at = 0;
};

UsersClockSamples users_clock_samples(const nlohmann::json& host, const nlohmann::json& accounting,
                                      double received_at);

struct UsersRoster {
    enum class State {
        loading,
        ready,
        stale,
        error,
        lost
    };
    enum class LossCause { identity, permission, config_unverified };
    using Rows = std::map<std::uint64_t, UsersRosterRow>;

    struct Transfer {
        std::uint64_t attempt;
        std::uint64_t sequence;
        std::size_t total;
        bool delta;
        std::size_t next_chunk = 0;
        std::size_t bytes = 0;
        double progress_at = 0;
        Rows rows;
        std::set<std::uint64_t> removed;
    };

    std::uint64_t subscription;
    bool live = false;
    State state = State::loading;
    LossCause loss_cause = LossCause::identity;
    bool fresh = false;
    std::string message;
    std::optional<std::uint64_t> sequence;
    std::uint64_t latest_attempt = 0;
    std::size_t generation = 0;
    std::size_t commits = 0;
    Rows baseline;
    std::optional<Transfer> transfer;
    UsersClockSamples clock;

    const Rows& visible_rows() const;
    bool receive(const nlohmann::json& frame, std::size_t bytes, double now);
    void reset_transfer(bool discard_rate_history = true);
    void reset_rates();
};

} // namespace wave
