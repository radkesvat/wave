#pragma once

#include "browser_ui.h"
#include "preview_session.h"

#include <map>
#include <optional>
#include <string>

namespace wave {

struct CoreSettingsDraft {
    enum class Section {
        runtime,
        logging,
        dns
    };

    Section section = Section::runtime;
    nlohmann::json source;
    nlohmann::json settings;
    std::map<std::string, std::string> inputs;
    std::map<std::string, std::string> errors;
    std::string notice;
    std::optional<std::uint64_t> request;
    std::string validation;
    CanvasViewport scroll_bounds{};

    bool dirty() const
    {
        return settings != source || !errors.empty();
    }
};

void update_core_settings(CoreSettingsDraft& draft, PreviewSession& session);
void render_core_settings(CoreSettingsDraft& draft, PreviewSession& session);
std::string describe_core_settings(const CoreSettingsDraft& draft);

} // namespace wave
