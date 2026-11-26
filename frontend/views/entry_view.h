#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace wave {

class AuthSession;
class PreviewSession;

enum class EntryPage {
    workspace,
    sign_in,
    setup
};

struct EntryViewState {
    EntryPage page = EntryPage::workspace;
    int step = 0;
    bool complete = false;
    bool reveal_feedback = false;
    std::optional<std::uint64_t> request_id;
    std::string message;
    char username[65] = "demo";
    char password[257] = "";
    char panel_name[81] = "Wave Preview";
    char bootstrap[65] = "";
    bool acknowledged = false;
};

void open_entry(EntryViewState& state, EntryPage page, PreviewSession& session);
void render_entry(EntryViewState& state, PreviewSession& session, float entry_top);
void render_authenticated_entry(EntryViewState& state, AuthSession& session, float entry_top);
std::string describe_entry(const EntryViewState& state, bool authenticated);

} // namespace wave
