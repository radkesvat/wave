#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace wave {

class PreviewSession;

struct ConsoleViewState {
    std::array<char, 241> command{};
    std::array<char, 97> search{};
    int history_position = -1;
    std::size_t output_size = 0;
    std::string last_line;
    bool autoscroll = true;
    bool copied = false;
};

void render_console(ConsoleViewState& state, PreviewSession& session, bool narrow);

} // namespace wave
