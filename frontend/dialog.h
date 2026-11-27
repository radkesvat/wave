#pragma once

#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace wave {

struct DialogAction {
    const char* label;
    bool disabled = false;
};

struct DialogOptions {
    bool* open = nullptr;
    float entry_top = 0;
    // An explicit idle value reserves the same footer space as a busy value.
    std::optional<bool> busy;
    std::string_view footer_error;
    bool reveal_footer_error = false;
};

inline constexpr char busy_description[] = "Operation in progress.";
inline constexpr float busy_spinner_width = 24;

// Draw the body between construction and footer(). The returned action index
// belongs to the calling view, which owns submission and dismissal.
class Dialog {
  public:
    Dialog(const char* title, float width, std::span<const DialogAction> actions, DialogOptions options = {});
    Dialog(const char* title, float width, std::initializer_list<DialogAction> actions,
           DialogOptions options = {});
    ~Dialog();

    Dialog(const Dialog&) = delete;
    Dialog& operator=(const Dialog&) = delete;

    explicit operator bool() const
    {
        return visible_;
    }
    int footer();

  private:
    struct Button {
        DialogAction action;
        float x;
        float width;
        int row_from_bottom;
    };

    bool visible_ = false;
    bool body_open_ = false;
    bool scroll_footer_ = false;
    bool busy_ = false;
    std::string_view footer_error_;
    bool reveal_footer_error_ = false;
    float action_offset_ = 0;
    float button_height_ = 0;
    int last_row_ = 0;
    std::vector<Button> buttons_;
};

void draw_busy_spinner(float row_height);
bool dialog_open();
bool dialog_at(float x, float y);

} // namespace wave
