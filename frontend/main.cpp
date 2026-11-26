#include "dialog.h"
#include "panel.h"
#include "preview_session.h"
#include "theme.h"

#include <GLES3/gl3.h>
#include <SDL.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

std::string clipboard_text;

// clang-format off
EM_JS(void, set_panel_height, (int height), {
    Module['canvas'].style.setProperty('--panel-height', height + 'px');
});

EM_JS(void, show_panel, (), { Module['showPanel'](); });

EM_JS(void, update_panel_description, (const char* description), {
    Module['updateDescription'](UTF8ToString(description));
});

EM_JS(void, register_header_controls, (int menu, int tools, int canvas), {
    Module['registerHeaderControls'](menu, tools, canvas);
});

EM_JS(void, register_panel_page, (int page, const char* label), {
    Module['registerPanelPage'](page, UTF8ToString(label));
});

EM_JS(void, update_navigation, (int page, bool open, bool menu, bool tools, bool modal, bool server_status), {
    Module['updateNavigation'](page, open, menu, tools, modal, server_status);
});

EM_JS(int, browser_panel_focus, (), { return Module['panelFocus'](); });

EM_JS(int, browser_viewport_width, (), { return window.innerWidth; });

EM_JS(void, scroll_to_top, (), { window.scrollTo(0, 0); });

EM_JS(void, show_text_input, (bool active, float x, float y), { Module['setTextInput'](active, x, y); });

EM_JS(void, copy_text, (const char* text), { Module['copyText'](UTF8ToString(text)); });

EM_JS(bool, open_browser_link, (const char* url), {
    window.open(UTF8ToString(url), '_blank', 'noopener,noreferrer');
    return true;
});

EM_JS_DEPS(install_text_input, "$stringToNewUTF8,free");
EM_JS(void, install_text_input, (), {
    Module['installTextInput']((text, paste = false) => {
        const value = stringToNewUTF8(text);
        Module['_wave_input_text'](value, paste);
        _free(value);
    });
});

EM_JS(void, show_startup_error, (), {
    Module['onAbort']();
});
// clang-format on

struct Application {
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;
    bool imgui_sdl = false;
    bool imgui_gl = false;
    bool visible = false;
    bool high_contrast = false;
    int minimum_height = 0;
    wave::PreviewSession session;
    wave::PanelState panel;
    std::string displayed_description;

    bool initialize()
    {
        // SDL handles mouse and keyboard keys. The shell routes touch, scrolling, and browser text input.
        SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            return false;
        }
        SDL_EventState(SDL_MOUSEWHEEL, SDL_IGNORE);
        SDL_EventState(SDL_FINGERDOWN, SDL_IGNORE);

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        window = SDL_CreateWindow("Wave", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1, 1,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window) {
            return false;
        }

        // Pointer events in the shell own touch gestures for the canvas.
        emscripten_set_touchstart_callback("#canvas", nullptr, false, nullptr);
        emscripten_set_touchmove_callback("#canvas", nullptr, false, nullptr);
        emscripten_set_touchend_callback("#canvas", nullptr, false, nullptr);
        emscripten_set_touchcancel_callback("#canvas", nullptr, false, nullptr);

        gl_context = SDL_GL_CreateContext(window);
        if (!gl_context || SDL_GL_MakeCurrent(window, gl_context) != 0) {
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.FontDefault =
            io.Fonts->AddFontFromFileTTF("/poppins-medium.ttf", wave::theme::workspace_font_size);
        if (!io.FontDefault) {
            SDL_SetError("Could not load the embedded Wave font");
            return false;
        }

        imgui_sdl = ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
        imgui_gl = imgui_sdl && ImGui_ImplOpenGL3_Init("#version 300 es");
        if (!imgui_gl) {
            SDL_SetError("Could not initialize the Dear ImGui backends");
            return false;
        }
        SDL_StartTextInput();
        auto& platform = ImGui::GetPlatformIO();
        platform.Platform_SetImeDataFn = [](ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData* data) {
            show_text_input(data->WantTextInput, data->InputPos.x, data->InputPos.y);
        };
        platform.Platform_GetClipboardTextFn = [](ImGuiContext*) { return clipboard_text.c_str(); };
        platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
            clipboard_text = text;
            copy_text(text);
        };
        platform.Platform_OpenInShellFn = [](ImGuiContext*, const char* path) {
            return open_browser_link(path);
        };
        wave::theme::apply(high_contrast);
        install_text_input();
        register_header_controls(wave::panel_menu_control, wave::panel_tools_control,
                                 wave::panel_canvas_control);
        for (const auto& item : wave::panel_navigation()) {
            register_panel_page(static_cast<int>(item.page), item.label);
        }
        session.start();
        return true;
    }

    void shutdown()
    {
        session.stop();
        if (imgui_gl) {
            ImGui_ImplOpenGL3_Shutdown();
        }
        if (imgui_sdl) {
            ImGui_ImplSDL2_Shutdown();
        }
        ImGui::DestroyContext();
        if (gl_context) {
            SDL_GL_DeleteContext(gl_context);
        }
        if (window) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
    }

    bool fit_canvas(int viewport_width)
    {
        double css_width = 0;
        double css_height = 0;
        emscripten_get_element_css_size("#canvas", &css_width, &css_height);
        if (css_width < 1 || css_height < 1) {
            return false;
        }

        const int required_height = static_cast<int>(
            std::ceil(wave::panel_height(viewport_width, static_cast<float>(css_width), panel)));
        if (minimum_height != required_height) {
            minimum_height = required_height;
            set_panel_height(minimum_height);
            emscripten_get_element_css_size("#canvas", &css_width, &css_height);
        }

        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window, &window_width, &window_height);
        const int height = static_cast<int>(css_height);
        const int resized_width = static_cast<int>(css_width);
        if (window_width != resized_width || window_height != height) {
            // SDL updates the backing buffer at device resolution and scales pointer events
            // to these logical window dimensions.
            SDL_SetWindowSize(window, resized_width, height);
        }
        return true;
    }

    void frame()
    {
        session.update();
        const auto* data = session.overview();
        const bool loading = session.status() == wave::PreviewStatus::loading;
        // The responsive layout uses the viewport width even when a document scrollbar is present.
        const int viewport_width = browser_viewport_width();
        if (!fit_canvas(viewport_width)) {
            return;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
        }

        if (panel.high_contrast != high_contrast) {
            high_contrast = panel.high_contrast;
            wave::theme::apply(high_contrast);
        }
        wave::theme::apply_text_scale(viewport_width);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        const auto previous_page = panel.page;
        const auto previous_entry = panel.entry.page;
        wave::render_panel(panel, session, data, loading, viewport_width, browser_panel_focus());
        if (panel.page != previous_page || panel.entry.page != previous_entry) {
            scroll_to_top();
        }
        ImGui::Render();

        int drawable_width = 0;
        int drawable_height = 0;
        SDL_GL_GetDrawableSize(window, &drawable_width, &drawable_height);
        glViewport(0, 0, drawable_width, drawable_height);
        const auto clear = wave::theme::rgba(wave::theme::Color::canvas);
        glClearColor(clear.x, clear.y, clear.z, clear.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);

        auto description = wave::describe_panel(panel, session, data, loading);
        if (description != displayed_description) {
            update_panel_description(description.c_str());
            displayed_description = std::move(description);
        }
        const bool in_workspace = panel.entry.page == wave::EntryPage::workspace;
        update_navigation(static_cast<int>(panel.page), panel.navigation_open && in_workspace,
                          panel.menu_open, panel.tools_open, wave::dialog_open(),
                          panel.page == wave::PanelPage::server && in_workspace);
        if (!visible) {
            show_panel();
            visible = true;
        }
    }
};

Application application;

void render_frame(void* context)
{
    static_cast<Application*>(context)->frame();
}

} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void wave_activate_panel_control(int control)
{
    if (!wave::dialog_open()) {
        application.panel.browser_control = control;
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void wave_tap_panel(float x, float y)
{
    auto& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
}

extern "C" EMSCRIPTEN_KEEPALIVE void wave_scroll_panel(float x, float y, float dx, float dy)
{
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    io.AddMouseWheelEvent(dx, dy);
}

extern "C" EMSCRIPTEN_KEEPALIVE bool wave_dialog_at(float x, float y)
{
    return wave::dialog_at(x, y);
}

extern "C" EMSCRIPTEN_KEEPALIVE int wave_topology_map_at(float x, float y)
{
    return static_cast<int>(application.panel.topology.map_at(x, y));
}

extern "C" EMSCRIPTEN_KEEPALIVE void wave_pan_topology(int map, float dx, float dy)
{
    application.panel.topology.pan(static_cast<wave::TopologyViewState::Tab>(map), dx, dy);
}

extern "C" EMSCRIPTEN_KEEPALIVE void wave_input_text(const char* text, bool paste)
{
    auto& io = ImGui::GetIO();
    if (paste) {
        // Use the text widget's paste path so it replaces the selection even while Ctrl is held.
        clipboard_text = text;
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_V, true);
        io.AddKeyEvent(ImGuiKey_V, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
    } else {
        io.AddInputCharactersUTF8(text);
    }
}

int main()
{
    if (!application.initialize()) {
        std::fprintf(stderr, "Wave startup failed: %s\n", SDL_GetError());
        show_startup_error();
        application.shutdown();
        return 1;
    }

    emscripten_set_main_loop_arg(render_frame, &application, 0, false);
    return 0;
}
