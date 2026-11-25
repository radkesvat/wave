#pragma once

#include "overview_data.h"

#include <emscripten/websocket.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace wave {

enum class PreviewStatus { loading, ready, unavailable };

struct PreviewFeedback {
    std::string message;
    std::uint64_t revision = 0;
    bool ok = false;
};

class PreviewSession {
public:
    void start();
    void stop();
    void update();
    bool send(nlohmann::json request);

    PreviewStatus status() const { return status_; }
    bool ready() const { return status_ == PreviewStatus::ready; }
    const nlohmann::json& data() const { return data_; }
    const OverviewData* overview() const { return ready() && overview_ ? &*overview_ : nullptr; }
    std::string_view feedback() const { return feedback_; }
    bool feedback_ok() const { return feedback_ok_; }
    std::string_view feedback_action() const { return feedback_action_; }
    std::uint64_t feedback_revision() const { return feedback_revision_; }
    const PreviewFeedback* feedback_for(std::string_view action) const;

private:
    void fail();
    bool receive(std::string_view payload);

    EMSCRIPTEN_WEBSOCKET_T socket_ = 0;
    double deadline_ = 0;
    PreviewStatus status_ = PreviewStatus::loading;
    nlohmann::json data_ = nlohmann::json::object();
    std::optional<OverviewData> overview_;
    std::string feedback_;
    std::string feedback_action_;
    std::map<std::string, PreviewFeedback, std::less<>> action_feedback_;
    bool feedback_ok_ = false;
    std::uint64_t feedback_revision_ = 0;
};

} // namespace wave
