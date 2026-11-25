#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace wave {

class PreviewSession;

enum class EntryPage { workspace, sign_in, setup };

struct EntryViewState {
    EntryPage page = EntryPage::workspace;
    int step = 0;
    bool complete = false;
    std::optional<std::uint64_t> request_id;
    std::string message;
    char username[65] = "demo";
    char password[97] = "";
    char panel_name[81] = "Wave Preview";
    char domain[161] = "wave.example";
};

void open_entry(EntryViewState& state, EntryPage page, PreviewSession& session);
void render_entry(EntryViewState& state, PreviewSession& session);

} // namespace wave
