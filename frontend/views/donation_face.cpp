#include "donation_face.h"

#include <GLES3/gl3.h>
#include <SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>

namespace wave {
namespace {

struct Sticker {
    GLuint texture = 0;
    ImVec2 size{};
};

constexpr std::array face_paths{
    "/donation/cat_01.bmp", "/donation/cat_02.bmp", "/donation/cat_03.bmp", "/donation/cat_04.bmp",
    "/donation/cat_05.bmp", "/donation/cat_06.bmp", "/donation/cat_07.bmp", "/gc1.bmp",
};
static_assert(face_paths.size() == static_cast<std::size_t>(DonationFace::none));
std::array<Sticker, face_paths.size()> faces;
Sticker response_portrait;

Sticker load_sticker(const char* path)
{
    using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
    const Surface bitmap(SDL_LoadBMP(path), SDL_FreeSurface);
    if (!bitmap) {
        return {};
    }
    const Surface image(SDL_ConvertSurfaceFormat(bitmap.get(), SDL_PIXELFORMAT_RGBA32, 0), SDL_FreeSurface);
    if (!image) {
        return {};
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) {
        return {};
    }

    GLint previous_texture = 0;
    GLint previous_row_length = 0;
    GLint previous_alignment = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &previous_row_length);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_alignment);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, image->pitch / 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image->w, image->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glPixelStorei(GL_UNPACK_ALIGNMENT, previous_alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, previous_row_length);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        return {};
    }
    return {texture, {static_cast<float>(image->w), static_cast<float>(image->h)}};
}

void draw_sticker(ImDrawList& draw, const Sticker& sticker, ImVec2 min, ImVec2 box, float rounding)
{
    if (!sticker.texture) {
        return;
    }
    const float scale = std::min(box.x / sticker.size.x, box.y / sticker.size.y);
    const ImVec2 size{sticker.size.x * scale, sticker.size.y * scale};
    min.x += (box.x - size.x) / 2;
    min.y += (box.y - size.y) / 2;
    draw.AddImageRounded(ImTextureRef{static_cast<ImTextureID>(sticker.texture)}, min,
                         {min.x + size.x, min.y + size.y}, {0, 0}, {1, 1}, ImGui::GetColorU32(IM_COL32_WHITE),
                         rounding);
}

} // namespace

const DonationChoice* find_donation_choice(std::string_view answer)
{
    const auto choice = std::ranges::find(donation_choices, answer, &DonationChoice::key);
    if (choice != donation_choices.end()) {
        return &*choice;
    }
    static constexpr DonationChoice legacy_below{"below_5", "Below 5 USD", DonationFace::none};
    static constexpr DonationChoice legacy_above{"more_than_5", "More than 5 USD", DonationFace::none};
    if (answer == legacy_below.key) {
        return &legacy_below;
    }
    return answer == legacy_above.key ? &legacy_above : nullptr;
}

void initialize_donation_faces()
{
    for (std::size_t i = 0; i < faces.size(); ++i) {
        faces[i] = load_sticker(face_paths[i]);
    }
    response_portrait = load_sticker("/gc2.bmp");
}

void shutdown_donation_faces()
{
    for (auto& face : faces) {
        glDeleteTextures(1, &face.texture);
        face = {};
    }
    glDeleteTextures(1, &response_portrait.texture);
    response_portrait = {};
}

void draw_donation_face(ImDrawList& draw, ImVec2 min, ImVec2 size, DonationFace face)
{
    if (face != DonationFace::none) {
        const float rounding = face == DonationFace::gigachad ? std::min(size.x, size.y) / 2 : 0;
        draw_sticker(draw, faces[static_cast<std::size_t>(face)], min, size, rounding);
    }
}

void draw_donation_response_sticker(ImDrawList& draw, ImVec2 min, ImVec2 size, DonationFace face)
{
    if (face == DonationFace::gigachad && response_portrait.texture) {
        draw_sticker(draw, response_portrait, min, size, 6);
    } else {
        draw_donation_face(draw, min, size, face);
    }
}

} // namespace wave
