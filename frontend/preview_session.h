#pragma once

#include <emscripten/websocket.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace wave {

enum class PreviewStatus {
    loading,
    ready,
    unavailable
};

struct PreviewReply {
    enum class Status {
        accepted,
        rejected,
        disconnected,
        timed_out
    };

    Status status;
    std::string message;
    nlohmann::json result;

    bool ok() const
    {
        return status == Status::accepted;
    }
};

class PreviewSession {
  public:
    void start(bool keep_snapshot = false);
    void stop();
    void update();
    std::optional<std::uint64_t> submit(nlohmann::json request, double timeout_ms = 0);
    std::optional<PreviewReply> take_reply(std::optional<std::uint64_t>& request_id);
    void abandon(std::optional<std::uint64_t>& request_id);
    void add_local_log(std::string_view level, std::string_view message);
    void clear_local_logs();
    bool take_schematic_change();

    PreviewStatus status() const
    {
        return status_;
    }
    bool ready() const
    {
        return status_ == PreviewStatus::ready;
    }
    const nlohmann::json& data() const
    {
        return data_;
    }
    const nlohmann::json& logs() const
    {
        return logs_;
    }

  private:
    struct Request {
        std::string action;
        double deadline;
        std::optional<PreviewReply> reply;
    };

    struct LocalLog {
        nlohmann::json entry;
        nlohmann::json after;
    };

    void fail();
    bool receive(std::string_view payload);
    void expire(Request& request, double now);
    void log_transport_failure(std::string_view action, std::string_view message);
    void update_logs();

    EMSCRIPTEN_WEBSOCKET_T socket_ = 0;
    double deadline_ = 0;
    bool schematic_changed_ = false;
    std::uint64_t schematic_revision_ = 0;
    PreviewStatus status_ = PreviewStatus::loading;
    nlohmann::json data_ = nlohmann::json::object();
    std::deque<LocalLog> local_logs_;
    nlohmann::json logs_ = nlohmann::json::array();
    std::map<std::uint64_t, Request> requests_;
    std::uint64_t next_request_id_ = 1;
};

} // namespace wave
