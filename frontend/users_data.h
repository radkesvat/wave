#pragma once

#include <string>
#include <vector>

namespace wave {

enum class UserProtocol {
    tcp,
    tcp_websocket
};

struct UserRecord {
    int id;
    std::string name;
    bool enabled;
    double upload_rate;
    double download_rate;
    double upload_total;
    double download_total;
    int remaining_minutes;
    std::string notes;
    std::string configuration;
    UserProtocol protocol;
    int upload_limit;
    int download_limit;
    int traffic_limit;
    int ip_limit;
};

struct UsersData {
    std::vector<UserRecord> rows;
    bool live = false;
};

} // namespace wave
