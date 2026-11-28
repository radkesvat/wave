#pragma once

#include "users_roster.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wave {

struct UsersRosterVersion {
    using Rows = std::map<std::string, std::shared_ptr<const std::string>, std::less<>>;
    std::uint64_t sequence = 0;
    std::size_t bytes = 0;
    Rows rows;

    bool insert(const nlohmann::json& row);
    void erase(std::string_view id);
};

struct UsersRosterDelta {
    std::uint64_t base_sequence;
    std::vector<std::string> changed;
    std::vector<std::string> removed;
};

UsersRosterDelta compare_users_rosters(const UsersRosterVersion& previous, const UsersRosterVersion& next);
UsersRosterDelta compare_and_share_users_rosters(const UsersRosterVersion& previous, UsersRosterVersion& next);
void add_users_roster_clock(nlohmann::json& frame, const UsersRosterClock& clock);

class UsersRosterTransfer {
  public:
    UsersRosterTransfer(std::shared_ptr<const UsersRosterVersion> version,
                        std::shared_ptr<const UsersRosterDelta> delta, std::uint64_t attempt);

    std::string next(std::string_view subscription, const UsersRosterClock& clock = {},
                     std::optional<std::string_view> stale = std::nullopt);
    bool will_end() const;
    const std::shared_ptr<const UsersRosterVersion>& version() const { return version_; }

  private:
    std::shared_ptr<const UsersRosterVersion> version_;
    std::shared_ptr<const UsersRosterDelta> delta_;
    std::uint64_t attempt_;
    UsersRosterVersion::Rows::const_iterator row_;
    std::size_t changed_ = 0;
    std::size_t removed_ = 0;
    std::size_t chunks_ = 0;
    bool begun_ = false;
};

} // namespace wave
