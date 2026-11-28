#include "preview_session.h"

#include "waterwall_schematic.h"

#include <emscripten.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <format>
#include <memory>
#include <utility>

namespace wave {
namespace {

using Json = nlohmann::json;
constexpr double connect_timeout_ms = 5000;
constexpr double http_poll_interval_ms = 1000;
constexpr double roster_chunk_timeout_ms = 10000;
// Match the backend WebSocket frame limit for large config registries.
constexpr std::size_t max_snapshot_bytes = 16 * 1024 * 1024;
constexpr std::string_view workspace_changed = "The workspace changed. Review its current state before retrying.";

// clang-format off
EM_JS_DEPS(preview_url, "$stringToNewUTF8");
EM_JS(char*, preview_url, (), {
    const protocol = location.protocol === 'https:' ? 'wss://' : 'ws://';
    return stringToNewUTF8(protocol + location.host + Module['basePath'] + '/ws/workspace');
});

EM_JS_DEPS(request_workspace_http, "malloc,free");
EM_JS(void, request_workspace_http,
      (PreviewSession* session, const char* sign_in, const char* workspace, const char* schematic_revision,
       const char* action, size_t max_bytes), {
    const controller = new AbortController();
    Module['workspaceHttpRequest'] = controller;
    const headers = { 'X-Wave-Session': UTF8ToString(sign_in) };
    const options = {
        mode: 'same-origin', credentials: 'same-origin', cache: 'no-store', redirect: 'error',
        signal: controller.signal, headers,
    };
    let url = Module['basePath'] + '/api/workspace';
    if (action) {
        options.method = 'POST';
        options.body = UTF8ToString(action);
        headers['Content-Type'] = 'application/json';
        headers['X-Wave-CSRF'] = '1';
        headers['X-Wave-Workspace'] = UTF8ToString(workspace);
    } else if (workspace) {
        const query = new URLSearchParams({ workspace_id: UTF8ToString(workspace),
            schematic_revision: UTF8ToString(schematic_revision) });
        url += '?' + query;
    }
    const timeout = setTimeout(() => controller.abort(), 15000);
    (async () => {
        let status = 0;
        let bytes;
        try {
            const response = await fetch(url, options);
            status = response.status;
            if (response.ok && Number(response.headers.get('Content-Length')) <= max_bytes) {
                const body = await response.arrayBuffer();
                if (body.byteLength <= max_bytes) bytes = new Uint8Array(body);
            }
        } catch {} finally {
            clearTimeout(timeout);
        }
        if (Module['workspaceHttpRequest'] !== controller) return;
        Module['workspaceHttpRequest'] = null;
        const data = bytes?.length ? _malloc(bytes.length) : 0;
        if (data) HEAPU8.set(bytes, data);
        try {
            Module['_wave_workspace_http_reply'](session, status, data, data ? bytes.length : 0);
        } finally {
            _free(data);
        }
    })();
});

EM_JS(void, cancel_workspace_http, (), {
    const request = Module['workspaceHttpRequest'];
    Module['workspaceHttpRequest'] = null;
    request?.abort();
});

EM_JS(void, reload_workspace_session, (), {
    if (!Module['panelLocationPending']()) location.reload();
});

EM_JS(void, open_panel_https, (const char* url), { Module['followPanelHttps'](UTF8ToString(url)); });
// clang-format on

bool valid_snapshot(const Json& data)
{
    return data.is_object() && data.contains("logs") && data["logs"].is_array() && data.contains("server") &&
           data["server"].is_object() && data.contains("settings") && data["settings"].is_object();
}

} // namespace

void PreviewSession::start_for_sign_in(std::string_view session_id)
{
    sign_in_id_ = session_id;
    start();
}

void PreviewSession::start(bool keep_snapshot)
{
    stop();
    status_ = PreviewStatus::loading;
    if (!keep_snapshot) {
        schematic_revision_ = 0;
        clear_waterwall_schematic();
        schematic_changed_ = false;
        data_.clear();
        update_logs();
    }

    const std::unique_ptr<char, decltype(&std::free)> url(preview_url(), std::free);
    if (!url || !emscripten_websocket_is_supported()) {
        start_http_fallback();
        return;
    }
    EmscriptenWebSocketCreateAttributes attributes{
        .url = url.get(), .protocols = nullptr, .createOnMainThread = true};
    socket_ = emscripten_websocket_new(&attributes);
    if (socket_ <= 0) {
        start_http_fallback();
        return;
    }

    const auto on_message = [](int, const EmscriptenWebSocketMessageEvent* event, void* user_data) {
        auto& session = *static_cast<PreviewSession*>(user_data);
        if (event->socket != session.socket_) {
            return true;
        }
        // Emscripten includes a terminating NUL in the length of text messages.
        if (!event->isText || event->numBytes <= 1 || event->numBytes - 1 > max_snapshot_bytes ||
            !session.receive({reinterpret_cast<const char*>(event->data), event->numBytes - 1})) {
            session.start_http_fallback();
        }
        return true;
    };
    const auto on_close = [](int, const EmscriptenWebSocketCloseEvent* event, void* user_data) {
        auto& session = *static_cast<PreviewSession*>(user_data);
        if (event->socket == session.socket_) {
            session.start_http_fallback();
        }
        return true;
    };
    if (emscripten_websocket_set_onmessage_callback(socket_, this, on_message) != EMSCRIPTEN_RESULT_SUCCESS ||
        emscripten_websocket_set_onclose_callback(socket_, this, on_close) != EMSCRIPTEN_RESULT_SUCCESS) {
        start_http_fallback();
        return;
    }
    deadline_ = emscripten_get_now() + connect_timeout_ms;
}

void PreviewSession::stop()
{
    if (socket_ > 0) {
        emscripten_websocket_close(socket_, 1000, nullptr);
        emscripten_websocket_delete(socket_);
    }
    socket_ = 0;
    if (http_request_) {
        cancel_workspace_http();
        http_request_.reset();
    }
    http_fallback_ = false;
    http_workspace_id_.clear();
    users_roster_.reset();
    supports_users_roster_ = false;
    discard_queued_http("The connection closed before the request was sent.");
    std::erase_if(requests_, [&](const auto& item) {
        const auto& request = item.second;
        // A form may consume an acknowledgement on the frame after the socket closes.
        if (request.reply) {
            return false;
        }
        log_transport_failure(request.action, "The connection closed before Wave confirmed the request.");
        return true;
    });
}

void PreviewSession::expire(Request& request, double now)
{
    if (!request.reply && request.deadline > 0 && now >= request.deadline) {
        if (!request.queued_http_body.empty()) {
            constexpr std::string_view message = "The request expired before it was sent.";
            request.queued_http_body.clear();
            request.reply = PreviewReply{PreviewReply::Status::rejected, std::string(message), {{"outcome", "not_sent"}}};
            log_transport_failure(request.action, message);
            return;
        }
        request.reply = PreviewReply{PreviewReply::Status::timed_out, {}};
        log_transport_failure(request.action,
                              "Wave did not confirm the request. Check the current state before retrying.");
    }
}

void PreviewSession::update()
{
    const double now = emscripten_get_now();
    if (socket_ > 0 && status_ == PreviewStatus::loading && now >= deadline_) {
        start_http_fallback();
    }
    for (auto& [id, request] : requests_) {
        expire(request, now);
    }
    if (http_fallback_ && !http_request_) {
        update_http(now);
    }
    if (users_roster_ && users_roster_->transfer &&
        now - users_roster_->transfer->progress_at >= roster_chunk_timeout_ms) {
        refresh_users_roster(users_roster_->subscription, true);
    }
}

std::optional<std::uint64_t> PreviewSession::submit(Json request, double timeout_ms)
{
    const auto action = request.find("action");
    if (action == request.end() || !action->is_string()) {
        return std::nullopt;
    }
    const auto action_name = action->get<std::string>();
    const auto id = next_request_id_++;
    request["request_id"] = id;
    if (schematic_revision_ != 0) {
        request["schematic_revision"] = schematic_revision_;
    }
    auto body = request.dump();
    if (!ready() || (!http_fallback_ &&
                     emscripten_websocket_send_utf8_text(socket_, body.c_str()) != EMSCRIPTEN_RESULT_SUCCESS)) {
        log_transport_failure(action_name, "The connection is unavailable.");
        return std::nullopt;
    }
    const double deadline = timeout_ms > 0 ? emscripten_get_now() + timeout_ms : 0;
    requests_.emplace(id, Request{action_name, deadline, std::nullopt, http_fallback_ ? std::move(body) : std::string{}});
    return id;
}

std::optional<PreviewReply> PreviewSession::take_reply(std::optional<std::uint64_t>& request_id)
{
    if (!request_id) {
        return std::nullopt;
    }
    const auto request = requests_.find(*request_id);
    if (request == requests_.end()) {
        request_id.reset();
        return PreviewReply{PreviewReply::Status::disconnected, {}};
    }
    if (!request->second.reply) {
        return std::nullopt;
    }
    auto reply = std::move(request->second.reply);
    abandon(request_id);
    return reply;
}

void PreviewSession::abandon(std::optional<std::uint64_t>& request_id)
{
    if (request_id) {
        requests_.erase(*request_id);
        request_id.reset();
    }
}

bool PreviewSession::send_users_roster(Json request)
{
    if (schematic_revision_ != 0) {
        request["schematic_revision"] = schematic_revision_;
    }
    return ready() && supports_users_roster_ &&
           emscripten_websocket_send_utf8_text(socket_, request.dump().c_str()) == EMSCRIPTEN_RESULT_SUCCESS;
}

const UsersRoster* PreviewSession::users_roster(std::optional<std::uint64_t> subscription) const
{
    return users_roster_ && subscription == users_roster_->subscription ? &*users_roster_ : nullptr;
}

std::optional<std::uint64_t> PreviewSession::subscribe_users_roster(Json target, bool replace)
{
    if (!document_visible_ || !ready() || !supports_users_roster_) {
        return std::nullopt;
    }
    if (users_roster_ && !replace) {
        std::optional previous(users_roster_->subscription);
        unsubscribe_users_roster(previous);
    }
    const auto subscription = next_subscription_++;
    const bool live = target.value("source", "") == "live";
    target["action"] = "users_roster_subscribe";
    target["subscription"] = std::to_string(subscription);
    if (!send_users_roster(std::move(target))) {
        return std::nullopt;
    }
    users_roster_.emplace(UsersRoster{.subscription = subscription, .live = live});
    return subscription;
}

bool PreviewSession::send_roster_unsubscribe(std::uint64_t subscription)
{
    return send_users_roster({{"action", "users_roster_unsubscribe"},
                              {"subscription", std::to_string(subscription)}});
}

void PreviewSession::unsubscribe_users_roster(std::optional<std::uint64_t>& subscription)
{
    if (const auto* roster = users_roster(subscription)) {
        if (roster->state != UsersRoster::State::lost) {
            send_roster_unsubscribe(roster->subscription);
        }
        users_roster_.reset();
    }
    subscription.reset();
}

void PreviewSession::refresh_users_roster(std::optional<std::uint64_t> subscription, bool resync)
{
    if (const auto* roster = users_roster(subscription); roster && roster->state != UsersRoster::State::lost) {
        if (resync) {
            users_roster_->reset_transfer();
        } else {
            users_roster_->state = UsersRoster::State::loading;
            users_roster_->message.clear();
        }
        if (!send_users_roster({{"action", "users_roster_refresh"},
                               {"subscription", std::to_string(*subscription)}, {"resync", resync}})) {
            users_roster_->reset_rates();
            users_roster_->state = UsersRoster::State::error;
            users_roster_->message = "The account list connection is unavailable.";
        }
    }
}

void PreviewSession::set_document_visible(bool visible)
{
    document_visible_ = visible;
    if (!visible && users_roster_) {
        std::optional subscription(users_roster_->subscription);
        unsubscribe_users_roster(subscription);
    }
}

void PreviewSession::log_transport_failure(std::string_view action, std::string_view message)
{
    if (action.starts_with("get_")) {
        return;
    }
    std::string label(action);
    std::ranges::replace(label, '_', ' ');
    add_local_log("error", std::format("Request to {}: {}", label, message));
}

void PreviewSession::add_local_log(std::string_view level, std::string_view message)
{
    const auto now = std::time(nullptr);
    std::tm date{};
    localtime_r(&now, &date);
    char timestamp[9]{};
    std::strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &date);
    const auto logs = data_.find("logs");
    if (local_logs_.size() == 64) {
        local_logs_.pop_front();
    }
    local_logs_.push_back({{{"source", "panel"}, {"ts", timestamp}, {"level", level}, {"message", message}},
                           logs != data_.end() && !logs->empty() ? logs->back() : Json{}});
    update_logs();
}

void PreviewSession::clear_local_logs()
{
    local_logs_.clear();
    update_logs();
}

bool PreviewSession::take_schematic_change()
{
    return std::exchange(schematic_changed_, false);
}

void PreviewSession::update_logs()
{
    const auto logs = data_.find("logs");
    if (local_logs_.empty()) {
        logs_ = logs == data_.end() ? Json::array() : *logs;
        return;
    }
    logs_ = Json::array();
    auto entry = local_logs_.begin();
    // Keep browser messages where they arrived, even as later snapshots add server messages.
    if (logs != data_.end()) {
        while (entry != local_logs_.end() && std::ranges::find(*logs, entry->after) == logs->end()) {
            logs_.push_back(entry++->entry);
        }
        for (const auto& record : *logs) {
            logs_.push_back(record);
            while (entry != local_logs_.end() && entry->after == record) {
                logs_.push_back(entry++->entry);
            }
        }
    }
    while (entry != local_logs_.end()) {
        logs_.push_back(entry++->entry);
    }
}

void PreviewSession::fail()
{
    status_ = PreviewStatus::unavailable;
    stop();
}

void PreviewSession::start_http_fallback()
{
    stop();
    if (sign_in_id_.empty()) {
        status_ = PreviewStatus::unavailable;
        return;
    }
    http_fallback_ = true;
    status_ = PreviewStatus::loading;
    next_http_poll_ = 0;
}

void PreviewSession::update_http(double now)
{
    // Serial exchanges ensure an older poll never overwrites an action's newer snapshot.
    const auto queued = std::ranges::find_if(requests_, [](const auto& item) {
        return !item.second.reply && !item.second.queued_http_body.empty();
    });
    if (queued != requests_.end()) {
        http_request_ = queued->first;
        auto body = std::exchange(queued->second.queued_http_body, {});
        request_workspace_http(this, sign_in_id_.c_str(), http_workspace_id_.c_str(), nullptr,
                               body.c_str(), max_snapshot_bytes);
    } else if (document_visible_ && now >= next_http_poll_) {
        http_request_ = 0;
        const auto revision = std::to_string(schematic_revision_);
        request_workspace_http(this, sign_in_id_.c_str(), http_workspace_id_.empty() ? nullptr : http_workspace_id_.c_str(),
                               revision.c_str(), nullptr, max_snapshot_bytes);
    }
}

void PreviewSession::discard_queued_http(std::string_view message)
{
    for (auto& [id, request] : requests_) {
        if (!request.reply && !request.queued_http_body.empty()) {
            request.queued_http_body.clear();
            request.reply = PreviewReply{PreviewReply::Status::rejected, std::string(message), {{"outcome", "not_sent"}}};
            log_transport_failure(request.action, message);
        }
    }
}

void PreviewSession::receive_http(int status, std::string_view payload)
{
    if (!http_request_) {
        return;
    }
    const auto request_id = *http_request_;
    http_request_.reset();
    const bool valid = status == 200 && !payload.empty() && receive(payload);
    next_http_poll_ = valid ? emscripten_get_now() + http_poll_interval_ms : 0;
    if (status == 401 || (request_id == 0 && !valid)) {
        fail();
        if (status == 401) reload_workspace_session();
        return;
    }
    if (request_id == 0) {
        return;
    }
    const auto request = requests_.find(request_id);
    if (status == 409) {
        discard_queued_http(workspace_changed);
    } else if (!valid || (request != requests_.end() && !request->second.reply)) {
        discard_queued_http("Wave did not confirm the previous request. Review the current state before retrying.");
        next_http_poll_ = 0;
    }
    if (request == requests_.end() || request->second.reply) {
        return;
    }
    auto& pending = request->second;
    if (status == 409) {
        pending.reply = PreviewReply{PreviewReply::Status::rejected, std::string(workspace_changed),
                                     {{"outcome", "not_sent"}}};
    } else {
        pending.reply = PreviewReply{PreviewReply::Status::disconnected, {}};
        log_transport_failure(pending.action, "Wave did not confirm the request. Check the current state before retrying.");
    }
}

bool PreviewSession::receive(std::string_view payload)
{
    auto frame = Json::parse(payload, nullptr, false);
    if (frame.is_object() && frame["type"] == "users_roster") {
        if (!users_roster_ || frame["subscription"] != std::to_string(users_roster_->subscription) ||
            users_roster_->state == UsersRoster::State::lost) {
            return true;
        }
        if (!users_roster_->receive(frame, payload.size(), emscripten_get_now())) {
            refresh_users_roster(users_roster_->subscription, true);
        } else if (users_roster_->state == UsersRoster::State::lost) {
            if (users_roster_->loss_cause == UsersRoster::LossCause::identity) {
                last_lost_roster_identity_ = users_roster_->subscription;
            }
            // Release server polling while the view keeps the loss reason for Retry.
            return send_roster_unsubscribe(users_roster_->subscription);
        }
        return true;
    }
    if (!frame.is_object() || frame["type"] != "snapshot" || !valid_snapshot(frame["data"])) {
        return false;
    }

    bool restarted = false;
    if (http_fallback_) {
        const auto workspace = frame.find("workspace_id");
        if (workspace == frame.end() || !workspace->is_string() || workspace->get_ref<const std::string&>().empty()) {
            return false;
        }
        const auto& id = workspace->get_ref<const std::string&>();
        restarted = !http_workspace_id_.empty() && id != http_workspace_id_;
        http_workspace_id_ = id;
        if (restarted) {
            discard_queued_http(workspace_changed);
        }
    }

    if (const auto schematic = frame.find("schematic");
        schematic != frame.end() && *schematic != waterwall_schematic()) {
        const bool had_schematic = !waterwall_version().empty();
        if (const auto error = set_waterwall_schematic(std::move(*schematic)); !error.empty()) {
            add_local_log("error", "Invalid WaterWall schematic. " + error);
            return false;
        }
        schematic_changed_ = had_schematic;
    } else if (status_ == PreviewStatus::loading && !frame.contains("schematic") &&
               !waterwall_version().empty()) {
        clear_waterwall_schematic();
        schematic_changed_ = true;
    }

    if (const auto revision = frame.find("schematic_revision");
        revision != frame.end() && revision->is_number_unsigned()) {
        const auto next = revision->get<std::uint64_t>();
        // A workspace restore can invalidate forms while keeping the same schematic.
        schematic_changed_ |= schematic_revision_ != 0 && next != schematic_revision_;
        schematic_revision_ = next;
    }
    schematic_changed_ |= restarted;
    if (const auto support = frame.find("users_roster"); support != frame.end()) {
        supports_users_roster_ = *support == true;
    }
    const auto action = frame.find("action");
    const auto feedback = frame.find("feedback");
    const auto ok = frame.find("ok");
    if (action != frame.end() && action->is_string() && feedback != frame.end() && feedback->is_string() &&
        ok != frame.end() && ok->is_boolean()) {
        const auto id = frame.find("request_id");
        if (id != frame.end() && id->is_number_unsigned()) {
            const auto request = requests_.find(id->get<std::uint64_t>());
            if (request != requests_.end() &&
                request->second.action == action->get_ref<const std::string&>()) {
                const auto received_at = emscripten_get_now();
                expire(request->second, received_at);
                if (!request->second.reply) {
                    const auto status =
                        ok->get<bool>() ? PreviewReply::Status::accepted : PreviewReply::Status::rejected;
                    auto result = frame.find("result");
                    if (status == PreviewReply::Status::accepted && request->second.action == "apply_tls_settings" &&
                        result != frame.end() && result->is_object()) {
                        if (const auto target = result->find("https_url"); target != result->end() && target->is_string()) {
                            open_panel_https(target->get_ref<const std::string&>().c_str());
                        }
                    }
                    request->second.reply = PreviewReply{status, feedback->get<std::string>(),
                                                         result == frame.end() ? Json{} : std::move(*result),
                                                         frame["feedback_in_logs_only"] == true, received_at};
                }
            }
        }
    }
    data_ = std::move(frame["data"]);
    update_logs();
    status_ = PreviewStatus::ready;
    return true;
}

} // namespace wave

extern "C" EMSCRIPTEN_KEEPALIVE void wave_workspace_http_reply(wave::PreviewSession* session, int status,
                                                             const char* payload, std::size_t length)
{
    session->receive_http(status, payload ? std::string_view(payload, length) : std::string_view{});
}
