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
constexpr double roster_chunk_timeout_ms = 10000;
// Match the backend WebSocket frame limit for large config registries.
constexpr std::size_t max_snapshot_bytes = 16 * 1024 * 1024;

// clang-format off
EM_JS_DEPS(preview_url, "$stringToNewUTF8");
EM_JS(char*, preview_url, (), {
    const protocol = location.protocol === 'https:' ? 'wss://' : 'ws://';
    return stringToNewUTF8(protocol + location.host + Module['basePath'] + '/ws/workspace');
});
// clang-format on

bool valid_snapshot(const Json& data)
{
    return data.is_object() && data.contains("logs") && data["logs"].is_array() && data.contains("server") &&
           data["server"].is_object() && data.contains("settings") && data["settings"].is_object();
}

} // namespace

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
        fail();
        return;
    }
    EmscriptenWebSocketCreateAttributes attributes{
        .url = url.get(), .protocols = nullptr, .createOnMainThread = true};
    socket_ = emscripten_websocket_new(&attributes);
    if (socket_ <= 0) {
        fail();
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
            session.fail();
        }
        return true;
    };
    const auto on_close = [](int, const EmscriptenWebSocketCloseEvent* event, void* user_data) {
        auto& session = *static_cast<PreviewSession*>(user_data);
        if (event->socket == session.socket_) {
            session.fail();
        }
        return true;
    };
    if (emscripten_websocket_set_onmessage_callback(socket_, this, on_message) != EMSCRIPTEN_RESULT_SUCCESS ||
        emscripten_websocket_set_onclose_callback(socket_, this, on_close) != EMSCRIPTEN_RESULT_SUCCESS) {
        fail();
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
    users_roster_.reset();
    supports_users_roster_ = false;
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
        request.reply = PreviewReply{PreviewReply::Status::timed_out, {}};
        log_transport_failure(request.action,
                              "Wave did not confirm the request. Check the current state before retrying.");
    }
}

void PreviewSession::update()
{
    const double now = emscripten_get_now();
    if (status_ == PreviewStatus::loading && now >= deadline_) {
        fail();
    }
    for (auto& [id, request] : requests_) {
        expire(request, now);
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
    if (!ready() ||
        emscripten_websocket_send_utf8_text(socket_, request.dump().c_str()) != EMSCRIPTEN_RESULT_SUCCESS) {
        log_transport_failure(action_name, "The connection is unavailable.");
        return std::nullopt;
    }
    const double deadline = timeout_ms > 0 ? emscripten_get_now() + timeout_ms : 0;
    requests_.emplace(id, Request{action_name, deadline, std::nullopt});
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
