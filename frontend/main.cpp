#include "overview.h"

#include <GLES3/gl3.h>
#include <SDL.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <cmath>
#include <cstdio>

namespace {

using namespace std::chrono_literals;

constexpr wave::OverviewData sample_data{
    .cpu_percent = 28.0f,
    .memory_used_gib = 3.2f,
    .memory_total_gib = 8.0f,
    .uptime = 103h + 23min,
    .download_mib_per_second = {2.1f, 3.0f, 2.8f, 5.5f, 4.6f, 7.9f, 6.8f,
                                10.2f, 8.3f, 8.9f, 5.7f, 6.2f, 7.4f},
};

EM_JS(void, set_overview_height, (int height), {
    Module['canvas'].style.setProperty('--overview-height', height + 'px');
});

EM_JS(void, show_overview, (const char* description), {
    Module['showOverview'](UTF8ToString(description));
});

EM_JS(void, show_startup_error, (), { Module['onAbort'](); });

struct Application {
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;
    bool imgui_sdl = false;
    bool imgui_gl = false;
    bool visible = false;
    int minimum_height = 0;

    bool initialize()
    {
        // Let the browser handle document scrolling and zoom on this read-only page.
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
        window = SDL_CreateWindow("Wave", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                  1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                            SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window) {
            return false;
        }

        gl_context = SDL_GL_CreateContext(window);
        if (!gl_context || SDL_GL_MakeCurrent(window, gl_context) != 0) {
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.FontDefault = io.Fonts->AddFontFromFileTTF("/karla.ttf", 16.0f);
        if (!io.FontDefault) {
            SDL_SetError("Could not load the embedded Overview font");
            return false;
        }

        imgui_sdl = ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
        imgui_gl = imgui_sdl && ImGui_ImplOpenGL3_Init("#version 300 es");
        if (!imgui_gl) {
            SDL_SetError("Could not initialize the Dear ImGui backends");
            return false;
        }
        return true;
    }

    void shutdown()
    {
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

    bool fit_canvas()
    {
        double css_width = 0;
        double css_height = 0;
        emscripten_get_element_css_size("#canvas", &css_width, &css_height);
        if (css_width < 1 || css_height < 1) {
            return false;
        }

        const int width = static_cast<int>(css_width);
        const int required_height = static_cast<int>(std::ceil(wave::overview_height(width)));
        if (minimum_height != required_height) {
            minimum_height = required_height;
            set_overview_height(minimum_height);
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
        if (!fit_canvas()) {
            return;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        wave::render_overview(sample_data);
        ImGui::Render();

        int drawable_width = 0;
        int drawable_height = 0;
        SDL_GL_GetDrawableSize(window, &drawable_width, &drawable_height);
        glViewport(0, 0, drawable_width, drawable_height);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);

        if (!visible) {
            show_overview(wave::describe_overview(sample_data).c_str());
            visible = true;
        }
    }
};

void render_frame(void* application)
{
    static_cast<Application*>(application)->frame();
}

} // namespace

int main()
{
    static Application application;
    if (!application.initialize()) {
        std::fprintf(stderr, "Wave startup failed: %s\n", SDL_GetError());
        show_startup_error();
        application.shutdown();
        return 1;
    }

    emscripten_set_main_loop_arg(render_frame, &application, 0, false);
    return 0;
}
