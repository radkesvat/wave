#include "preview_session.h"

#include <emscripten.h>

#include <cstdlib>
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
    feedback_.clear();
    feedback_action_.clear();
    action_feedback_.clear();
    if (!keep_snapshot) {
        data_.clear();
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
    // A form may consume an acknowledgement on the frame after the socket closes.
    std::erase_if(requests_, [](const auto& entry) { return !entry.second.reply; });
}

void PreviewSession::Request::expire(double now)
{
    if (!reply && deadline > 0 && now >= deadline) {
        reply = PreviewReply{PreviewReply::Status::timed_out, {}};
    }
}

void PreviewSession::update()
{
    const double now = emscripten_get_now();
    if (status_ == PreviewStatus::loading && now >= deadline_) {
        fail();
    }
    for (auto& [id, request] : requests_) {
        request.expire(now);
    }
}

bool PreviewSession::send(Json request)
{
    if (!ready()) {
        return false;
    }
    const auto action = request.find("action");
    if (action == request.end() || !action->is_string() ||
        emscripten_websocket_send_utf8_text(socket_, request.dump().c_str()) != EMSCRIPTEN_RESULT_SUCCESS) {
        return false;
    }
    action_feedback_.try_emplace(action->get_ref<const std::string&>());
    return true;
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
    if (!send(std::move(request))) {
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

const PreviewFeedback* PreviewSession::feedback_for(std::string_view action) const
{
    const auto found = action_feedback_.find(action);
    return found != action_feedback_.end() && found->second.revision != 0 ? &found->second : nullptr;
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
        feedback_action_ = action->get<std::string>();
        feedback_ok_ = ok->get<bool>();
        const auto logs_only = frame.find("feedback_in_logs_only");
        feedback_ = feedback_ok_ && logs_only != frame.end() && *logs_only == true
                        ? std::string{}
                        : feedback->get<std::string>();
        ++feedback_revision_;
        const auto known_action = action_feedback_.find(feedback_action_);
        if (known_action != action_feedback_.end()) {
            known_action->second = {feedback_, feedback_revision_, feedback_ok_};
        }
        const auto id = frame.find("request_id");
        if (id != frame.end() && id->is_number_unsigned()) {
            const auto request = requests_.find(id->get<std::uint64_t>());
            if (request != requests_.end() && request->second.action == feedback_action_) {
                request->second.expire(emscripten_get_now());
                if (!request->second.reply) {
                    const auto status =
                        feedback_ok_ ? PreviewReply::Status::accepted : PreviewReply::Status::rejected;
                    auto result = frame.find("result");
                    request->second.reply =
                        PreviewReply{status, feedback_, result == frame.end() ? Json{} : std::move(*result)};
                }
            }
        }
    }
    data_ = std::move(frame["data"]);
    status_ = PreviewStatus::ready;
    return true;
}

} // namespace wave
