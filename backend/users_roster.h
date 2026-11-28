#pragma once

#include "waterwall_users.h"

namespace wave {

using UsersRosterRows = std::map<std::string, nlohmann::json, std::less<>>;

inline constexpr std::size_t max_users_roster_rows = 100'000;
inline constexpr std::size_t max_users_roster_bytes = 32 * 1024 * 1024;

// Project a validated, normalized account onto the common live/saved row shape.
nlohmann::json make_users_roster_row(const nlohmann::json& normalized);

struct UsersRosterClock {
    std::optional<std::uint64_t> accounting_time_ms;
    std::uint64_t wall_time_ms = 0;
};

enum class UsersRosterLoss { identity, permission };

struct LiveUsersRosterRead {
    std::optional<UsersRosterRows> rows;
    UsersRosterClock clock;
    std::string error;
    std::optional<UsersRosterLoss> loss;
};

class LiveUsersRosterReader {
  public:
    // A successful revision probe can update the clock without replacing rows.
    // The caller serializes reads and discards results from a retired scope.
    LiveUsersRosterRead read(const std::function<WaterWallUsersReply(const WaterWallUsersRequest&)>& request,
                            bool has_status, bool force, const std::function<bool()>& current);

  private:
    std::optional<WaterWallUsersRevisions> covered_;
    bool follow_up_ = false;
};

} // namespace wave
