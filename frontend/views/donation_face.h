#pragma once

#include <imgui.h>

#include <array>
#include <string_view>

namespace wave {

enum class DonationFace {
    cat_1,
    cat_2,
    cat_3,
    cat_4,
    cat_5,
    cat_6,
    cat_7,
    gigachad,
    none
};

struct DonationChoice {
    const char* key;
    const char* label;
    DonationFace face;

    bool is_donation() const
    {
        return std::string_view(key) != "no";
    }
};

inline constexpr std::array donation_choices{
    DonationChoice{"no", "No donation", DonationFace::cat_1},
    DonationChoice{"1", "1 USD", DonationFace::cat_2},
    DonationChoice{"3", "3 USD", DonationFace::cat_3},
    DonationChoice{"5", "5 USD", DonationFace::cat_4},
    DonationChoice{"10", "10 USD", DonationFace::cat_5},
    DonationChoice{"15", "15 USD", DonationFace::cat_6},
    DonationChoice{"30", "30 USD", DonationFace::cat_7},
    DonationChoice{"50_plus", "50+ USD", DonationFace::gigachad},
};

const DonationChoice* find_donation_choice(std::string_view answer);
void initialize_donation_faces();
void shutdown_donation_faces();
void draw_donation_face(ImDrawList& draw, ImVec2 min, ImVec2 size, DonationFace face);
void draw_donation_response_sticker(ImDrawList& draw, ImVec2 min, ImVec2 size, DonationFace face);

} // namespace wave
