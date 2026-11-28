#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wave {

class PreviewSession;
struct DonationChoice;

struct AboutViewState {
    const DonationChoice* submitted = nullptr;
    std::optional<std::uint64_t> donation_request;
    bool donation_saved = false;
    bool choosing_answer = false;
};

void update_about(AboutViewState& state, PreviewSession& session);
void render_about(AboutViewState& state, PreviewSession& session, std::string_view version);
std::string describe_about(const AboutViewState& state, const PreviewSession& session);

} // namespace wave
