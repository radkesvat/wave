#include "preview_session.h"

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
// Match the backend WebSocket frame limit for large config registries.
constexpr std::size_t max_snapshot_bytes = 16 * 1024 * 1024;

// clang-format off
EM_JS_DEPS(preview_url, "$stringToNewUTF8");
EM_JS(char*, preview_url, (), {
    const protocol = location.protocol === 'https:' ? 'wss://' : 'ws://';
    return stringToNewUTF8(protocol + location.host + '/ws/preview');
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
        // Emscripten includes a terminating NUL in the length of text messages.
        if (!event->isText || event->numBytes <= 1 || event->numBytes - 1 > max_snapshot_bytes ||
            !session.receive({reinterpret_cast<const char*>(event->data), event->numBytes - 1})) {
            session.fail();
        }
        return true;
    };
    const auto on_close = [](int, const EmscriptenWebSocketCloseEvent*, void* user_data) {
        static_cast<PreviewSession*>(user_data)->fail();
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
    if (!frame.is_object() || frame["type"] != "snapshot" || !valid_snapshot(frame["data"])) {
        return false;
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
                expire(request->second, emscripten_get_now());
                if (!request->second.reply) {
                    const auto status =
                        ok->get<bool>() ? PreviewReply::Status::accepted : PreviewReply::Status::rejected;
                    auto result = frame.find("result");
                    request->second.reply = PreviewReply{status, feedback->get<std::string>(),
                                                         result == frame.end() ? Json{} : std::move(*result)};
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
