#pragma once

#include <array>
#include <string>

namespace wave {

class AuthSession;

enum class EntryPage {
    checking,
    workspace,
    sign_in,
    setup,
    base_path
};

struct EntryViewState {
    EntryPage page = EntryPage::checking;
    std::string message;
    std::array<char, 65> username{};
    std::array<char, 257> password{};
    std::string base_path = "/panel";
};

void render_entry(EntryViewState& state, AuthSession& session, float entry_top);
std::string describe_entry(const EntryViewState& state);

} // namespace wave
