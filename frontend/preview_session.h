#pragma once

#include "users_roster.h"

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
    bool feedback_in_logs_only = false;
    double received_at = 0;

    bool ok() const
    {
        return status == Status::accepted;
    }
};

class PreviewSession {
  public:
    void start_for_sign_in(std::string_view session_id);
    void start(bool keep_snapshot = false);
    void stop();
    void update();
    void receive_http(int status, std::string_view payload);
    std::optional<std::uint64_t> submit(nlohmann::json request, double timeout_ms = 0);
    std::optional<PreviewReply> take_reply(std::optional<std::uint64_t>& request_id);
    void abandon(std::optional<std::uint64_t>& request_id);
    void add_local_log(std::string_view level, std::string_view message);
    void clear_local_logs();
    bool take_schematic_change();
    bool supports_users_roster() const { return supports_users_roster_; }
    bool http_fallback() const { return http_fallback_; }
    const UsersRoster* users_roster(std::optional<std::uint64_t> subscription) const;
    bool lost_users_roster_identity(std::optional<std::uint64_t> subscription) const
    {
        return subscription && subscription == last_lost_roster_identity_;
    }
    std::optional<std::uint64_t> subscribe_users_roster(nlohmann::json target, bool replace = false);
    void unsubscribe_users_roster(std::optional<std::uint64_t>& subscription);
    void refresh_users_roster(std::optional<std::uint64_t> subscription, bool resync = false);
    void set_document_visible(bool visible);
    bool document_visible() const { return document_visible_; }

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
        std::string queued_http_body;
    };

    struct LocalLog {
        nlohmann::json entry;
        nlohmann::json after;
    };

    void fail();
    void start_http_fallback();
    void update_http(double now);
    void discard_queued_http(std::string_view message);
    bool receive(std::string_view payload);
    bool send_users_roster(nlohmann::json request);
    bool send_roster_unsubscribe(std::uint64_t subscription);
    void expire(Request& request, double now);
    void log_transport_failure(std::string_view action, std::string_view message);
    void update_logs();

    EMSCRIPTEN_WEBSOCKET_T socket_ = 0;
    double deadline_ = 0;
    std::string sign_in_id_;
    std::string http_workspace_id_;
    std::optional<std::uint64_t> http_request_;
    double next_http_poll_ = 0;
    bool http_fallback_ = false;
    bool schematic_changed_ = false;
    bool supports_users_roster_ = false;
    bool document_visible_ = true;
    std::uint64_t schematic_revision_ = 0;
    PreviewStatus status_ = PreviewStatus::loading;
    nlohmann::json data_ = nlohmann::json::object();
    std::deque<LocalLog> local_logs_;
    nlohmann::json logs_ = nlohmann::json::array();
    std::map<std::uint64_t, Request> requests_;
    std::uint64_t next_request_id_ = 1;
    std::uint64_t next_subscription_ = 1;
    // A close or visibility event may drop the roster before a pending create's owner sees the loss.
    std::optional<std::uint64_t> last_lost_roster_identity_;
    std::optional<UsersRoster> users_roster_;
};

} // namespace wave
