#pragma once

#include <cstdint>
#include <string>

namespace wave {

class PreviewSession;

enum class EntryPage { workspace, sign_in, setup };

struct EntryViewState {
    EntryPage page = EntryPage::workspace;
    int step = 0;
    bool pending = false;
    bool complete = false;
    std::uint64_t requested_at = 0;
    std::string message;
    char username[65] = "demo";
    char password[97] = "";
    char panel_name[81] = "Wave Preview";
    char domain[161] = "wave.example";
};

void open_entry(EntryViewState& state, EntryPage page);
void render_entry(EntryViewState& state, PreviewSession& session);

} // namespace wave
