#pragma once

#include <initializer_list>
#include <span>
#include <vector>

namespace wave {

struct DialogAction {
    const char* label;
    bool disabled = false;
};

// Draw the body between construction and footer(). The returned action index
// belongs to the calling view, which owns submission and dismissal.
class Dialog {
  public:
    Dialog(const char* title, float width, std::span<const DialogAction> actions, bool* open = nullptr,
           float entry_top = 0);
    Dialog(const char* title, float width, std::initializer_list<DialogAction> actions, bool* open = nullptr,
           float entry_top = 0);
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
    float button_height_ = 0;
    int last_row_ = 0;
    std::vector<Button> buttons_;
};

bool dialog_open();
bool dialog_at(float x, float y);

} // namespace wave
