#include "donation_face.h"

#include "theme.h"

#include <GLES3/gl3.h>
#include <SDL.h>

#include <algorithm>
#include <memory>

namespace wave {
namespace {

GLuint badge_texture = 0;
GLuint response_texture = 0;

GLuint load_sticker(const char* path)
{
    using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
    const Surface bitmap(SDL_LoadBMP(path), SDL_FreeSurface);
    if (!bitmap) {
        return 0;
    }
    const Surface image(SDL_ConvertSurfaceFormat(bitmap.get(), SDL_PIXELFORMAT_RGBA32, 0), SDL_FreeSurface);
    if (!image) {
        return 0;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) {
        return 0;
    }

    GLint previous_texture = 0;
    GLint previous_row_length = 0;
    GLint previous_alignment = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &previous_row_length);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_alignment);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, image->pitch / 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image->w, image->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, previous_alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, previous_row_length);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        return 0;
    }
    return texture;
}

void draw_sticker(ImDrawList& draw, GLuint texture, ImVec2 min, ImVec2 max, float rounding)
{
    draw.AddImageRounded(ImTextureRef{static_cast<ImTextureID>(texture)}, min, max, {0, 0}, {1, 1},
                         ImGui::GetColorU32(IM_COL32_WHITE), rounding);
}

} // namespace

void initialize_donation_faces()
{
    badge_texture = load_sticker("/gc1.bmp");
    response_texture = load_sticker("/gc2.bmp");
}

void shutdown_donation_faces()
{
    glDeleteTextures(1, &badge_texture);
    glDeleteTextures(1, &response_texture);
    badge_texture = 0;
    response_texture = 0;
}

void draw_donation_face(ImDrawList& draw, ImVec2 center, float radius, DonationFace expression)
{
    if (expression == DonationFace::happiest && badge_texture) {
        draw_sticker(draw, badge_texture, {center.x - radius, center.y - radius},
                     {center.x + radius, center.y + radius}, radius);
        return;
    }

    draw.AddCircleFilled(center, radius, theme::packed(theme::Color::warning));
    const auto ink = theme::packed(theme::Color::inset);
    const float stroke = std::max(1.2f, radius * 0.13f);
    const float eye_y = center.y - radius * 0.28f;
    for (const float side : {-1.0f, 1.0f}) {
        const float eye_x = center.x + side * radius * 0.34f;
        if (expression == DonationFace::happiest) {
            const float eye_width = radius * 0.17f;
            draw.AddBezierQuadratic({eye_x - eye_width, eye_y}, {eye_x, eye_y - radius * 0.32f},
                                    {eye_x + eye_width, eye_y}, ink, stroke);
        } else {
            draw.AddCircleFilled({eye_x, eye_y}, std::max(1.0f, radius / 9), ink);
        }
    }

    const float mouth_width = radius * 0.4f;
    switch (expression) {
    case DonationFace::sad: {
        const float y = center.y + radius * 0.45f;
        draw.AddBezierQuadratic({center.x - mouth_width, y}, {center.x, center.y - radius * 0.12f},
                                {center.x + mouth_width, y}, ink, stroke);
        break;
    }
    case DonationFace::happy: {
        const float y = center.y + radius * 0.12f;
        draw.AddBezierQuadratic({center.x - mouth_width, y}, {center.x, center.y + radius * 0.74f},
                                {center.x + mouth_width, y}, ink, stroke);
        break;
    }
    case DonationFace::happiest: {
        const ImVec2 left{center.x - radius * 0.49f, center.y + radius * 0.08f};
        draw.PathLineTo(left);
        draw.PathLineTo({center.x + radius * 0.49f, left.y});
        draw.PathBezierQuadraticCurveTo({center.x, center.y + radius * 0.95f}, left, 6);
        draw.PathFillConvex(ink);
        break;
    }
    }
}

void draw_donation_response_sticker(ImDrawList& draw, ImVec2 min, ImVec2 size)
{
    if (response_texture) {
        draw_sticker(draw, response_texture, min, {min.x + size.x, min.y + size.y}, 6);
        return;
    }
    draw_donation_face(draw, {min.x + size.x / 2, min.y + size.y / 2}, std::min(size.x, size.y) / 2,
                       DonationFace::happiest);
}

} // namespace wave
