#pragma once

#include <imgui.h>

namespace wave {

enum class DonationFace {
    sad,
    happy,
    happiest
};

void initialize_donation_faces();
void shutdown_donation_faces();
void draw_donation_face(ImDrawList& draw, ImVec2 center, float radius, DonationFace expression);
void draw_donation_response_sticker(ImDrawList& draw, ImVec2 min, ImVec2 size);

} // namespace wave
