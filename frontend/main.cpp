#include "browser_ui.h"
#include "dialog.h"
#include "panel.h"
#include "preview_session.h"
#include "theme.h"
#include "views/donation_face.h"
#include "views/view_helpers.h"

#include <GLES3/gl3.h>
#include <SDL.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace {

std::string clipboard_text;
std::string pending_paste;
ImGuiID paste_target = 0;

void clear_pending_paste()
{
    std::ranges::fill(pending_paste, '\0');
    pending_paste.clear();
    paste_target = 0;
}

// clang-format off
EM_JS(void, set_panel_height, (int height), {
    Module['canvas'].style.setProperty('--panel-height', height + 'px');
});

EM_JS(void, show_panel, (), { Module['showPanel'](); });

EM_JS(void, update_panel_description, (const char* description), {
    Module['updateDescription'](UTF8ToString(description));
});

EM_JS(void, set_scrollbar_colors, (ImU32 track, ImU32 thumb, ImU32 hover, ImU32 active), {
    const root = document.documentElement;
    const style = root.style;
    const palette_changed = style.getPropertyValue("--scrollbar-thumb") !== "";
    const rgb = color => "rgb(" + [color & 255, (color >> 8) & 255, (color >> 16) & 255].join(", ") + ")";
    style.setProperty("--scrollbar-track", rgb(track));
    style.setProperty("--scrollbar-thumb", rgb(thumb));
    style.setProperty("--scrollbar-hover", rgb(hover));
    style.setProperty("--scrollbar-active", rgb(active));
    if (palette_changed && root.classList.contains("classic-scrollbars") && root.clientWidth < innerWidth &&
        !matchMedia("(forced-colors: active)").matches && getComputedStyle(root).scrollbarColor === "auto") {
        // Blink can keep the old thumb pixels until hover. Switching to native colors
        // and back refreshes the scrollbar without changing its size or scroll position.
        const previous = style.scrollbarColor;
        style.scrollbarColor = rgb(thumb) + " " + rgb(track);
        root.getBoundingClientRect();
        style.scrollbarColor = previous;
    }
});

EM_JS(void, apply_browser_appearance,
      (bool light, bool high_contrast, ImU32 canvas, ImU32 primary_text, ImU32 secondary_text, ImU32 terminal,
       ImU32 toolbar, ImU32 divider, ImU32 control_border, ImU32 focus, ImU32 accent, ImU32 button_face,
       ImU32 button_hover, ImU32 text_selection), {
    const hex = color => "#" + [color & 255, (color >> 8) & 255, (color >> 16) & 255]
        .map(byte => byte.toString(16).padStart(2, "0")).join("");
    Module['applyAppearance']({
        light: !!light, highContrast: !!high_contrast, canvas: hex(canvas), primaryText: hex(primary_text),
        secondaryText: hex(secondary_text), terminal: hex(terminal), toolbar: hex(toolbar), divider: hex(divider),
        controlBorder: hex(control_border), focus: hex(focus), accent: hex(accent), buttonFace: hex(button_face),
        buttonHover: hex(button_hover), textSelection: hex(text_selection)
    });
});

EM_JS(void, register_header_controls, (int menu, int tools), {
    Module['registerHeaderControls'](menu, tools);
});

EM_JS(void, register_panel_page, (int page, const char* label), {
    Module['registerPanelPage'](page, UTF8ToString(label));
});

EM_JS(void, update_config_links, (const char* encoded, int footer), {
    Module['updateConfigLinks'](JSON.parse(UTF8ToString(encoded)), footer);
});

EM_JS(void, update_config_tabs, (const char* encoded, int tab), {
    Module['updateConfigTabs'](UTF8ToString(encoded), tab);
});

EM_JS(void, update_navigation, (int page, bool open, bool menu, bool tools, bool modal, const char* donation), {
    Module['updateNavigation'](page, open, menu, tools, modal, UTF8ToString(donation));
});

EM_JS(void, update_console, (bool active, float x, float y, float width, float height), {
    Module['updateConsole'](active, x, y, width, height);
});

EM_JS(int, browser_panel_focus, (), { return Module['panelFocus'](); });

EM_JS(void, read_browser_viewport, (float* width, float* height), {
    HEAPF32[width >> 2] = window.innerWidth;
    HEAPF32[height >> 2] = window.innerHeight;
});

EM_JS(void, scroll_to_top, (), { window.scrollTo(0, 0); });

EM_JS(void, show_text_input, (bool active, float x, float y, float line_height), {
    Module['setTextInput'](active, x, y, line_height);
});

EM_JS(void, update_keyboard_state, (bool manual, bool pressed), {
    Module['updateKeyboardState'](manual, pressed);
});

EM_JS(void, copy_text, (const char* text), { Module['copyText'](UTF8ToString(text)); });

EM_JS(bool, open_browser_link, (const char* url), {
    const path = UTF8ToString(url);
    const destination = path.startsWith('/') && !path.startsWith('//') ? Module['basePath'] + path : path;
    window.open(destination, '_blank', 'noopener,noreferrer');
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

EM_JS(void, install_document_visibility, (), {
    const update = () => Module['_wave_document_visibility'](document.visibilityState !== 'hidden');
    document.addEventListener('visibilitychange', update);
    update();
});

EM_JS(void, show_startup_error, (), {
    Module['onAbort']();
});
// clang-format on

void apply_theme(wave::theme::Appearance appearance)
{
    using wave::theme::Color;
    using wave::theme::packed;
    wave::theme::apply(appearance);
    apply_browser_appearance(appearance.preset == wave::theme::Preset::light, appearance.high_contrast,
                             packed(Color::canvas), packed(Color::primary_text),
                             packed(Color::secondary_text), packed(Color::terminal), packed(Color::toolbar),
                             packed(Color::divider), packed(Color::control_border), packed(Color::focus),
                             packed(Color::accent), packed(Color::button_face), packed(Color::button_hover),
                             packed(Color::text_selection));
    set_scrollbar_colors(ImGui::GetColorU32(ImGuiCol_ScrollbarBg), ImGui::GetColorU32(ImGuiCol_ScrollbarGrab),
                         ImGui::GetColorU32(ImGuiCol_ScrollbarGrabHovered),
                         ImGui::GetColorU32(ImGuiCol_ScrollbarGrabActive));
}

struct Application {
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;
    ImPlotContext* implot = nullptr;
    bool imgui_sdl = false;
    bool imgui_gl = false;
    bool visible = false;
    wave::theme::Appearance appearance;
    std::optional<wave::theme::Preset> theme_choice;
    std::optional<std::uint64_t> theme_request;
    wave::theme::Preset submitted_theme = wave::theme::Preset::standard;
    int minimum_height = 0;
    wave::PreviewSession session;
    wave::PanelState panel;
    std::string workspace_session_id;
    std::string displayed_description;
    std::vector<wave::PanelConfigItem> displayed_config_links;
    std::vector<wave::ConfigWorkspaceTab> displayed_config_tabs;
    int displayed_config_tab = -1;

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
        implot = ImPlot::CreateContext();
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
        wave::initialize_donation_faces();
        SDL_StartTextInput();
        auto& platform = ImGui::GetPlatformIO();
        platform.Platform_SetImeDataFn = [](ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData* data) {
            show_text_input(data->WantTextInput, data->InputPos.x, data->InputPos.y, data->InputLineHeight);
        };
        platform.Platform_GetClipboardTextFn = [](ImGuiContext* context) {
            return paste_target && paste_target == context->ActiveId ? pending_paste.c_str()
                                                                     : clipboard_text.c_str();
        };
        platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
            clipboard_text = text;
            copy_text(text);
        };
        platform.Platform_OpenInShellFn = [](ImGuiContext*, const char* path) {
            return open_browser_link(path);
        };
        apply_theme(appearance);
        install_text_input();
        install_document_visibility();
        register_header_controls(wave::panel_menu_control, wave::panel_tools_control);
        for (const auto& item : wave::panel_navigation()) {
            register_panel_page(static_cast<int>(item.page), item.label);
        }
        panel.auth.start();
        if (panel.auth.page() == wave::AuthSession::Page::setup) {
            panel.entry.page = wave::EntryPage::setup;
        } else if (panel.auth.page() == wave::AuthSession::Page::sign_in) {
            panel.entry.page = wave::EntryPage::sign_in;
        } else if (panel.auth.page() == wave::AuthSession::Page::base_path) {
            panel.entry.page = wave::EntryPage::base_path;
        }
        return true;
    }

    void shutdown()
    {
        session.stop();
        if (imgui_gl) {
            wave::shutdown_donation_faces();
            ImGui_ImplOpenGL3_Shutdown();
        }
        if (imgui_sdl) {
            ImGui_ImplSDL2_Shutdown();
        }
        if (implot) {
            ImPlot::DestroyContext(implot);
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

        const int required_height = static_cast<int>(std::ceil(wave::panel_height(viewport_width, panel)));
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

    void update_session()
    {
        if (panel.auth.page() == wave::AuthSession::Page::panel) {
            const bool authenticated = !panel.auth.username().empty();
            const bool in_workspace = panel.entry.page == wave::EntryPage::workspace;
            if (in_workspace && (!authenticated || workspace_session_id != panel.auth.session_id())) {
                panel.entry.page = wave::EntryPage::checking;
                panel.page = wave::PanelPage::server;
                panel.config_id = 0;
                panel.config_items.clear();
                decltype(panel.workspaces) receipts;
                for (auto& [config_id, workspace] : panel.workspaces) {
                    for (auto& [node, users] : workspace.users) {
                        if (auto receipt = wave::unconfirmed_create(users, session)) {
                            wave::restore_create_receipt(receipts[config_id].users[node], *receipt);
                        }
                    }
                    wave::abandon_config_workspace(workspace, session);
                }
                panel.workspaces = std::move(receipts);
                session.abandon(panel.about.donation_request);
                panel.about = {};
                session.abandon(panel.panel_controls.updates.request_id);
                session.abandon(panel.panel_controls.request_id);
                panel.panel_controls = {};
                wave::clear_workspace_backup_request();
                panel.settings = {};
                theme_choice.reset();
                session.abandon(theme_request);
                session.stop();
            }
            if (authenticated && panel.entry.page != wave::EntryPage::workspace) {
                if (workspace_session_id.empty() || workspace_session_id != panel.auth.session_id()) {
                    panel.workspaces.clear();
                    session.clear_local_logs();
                }
                workspace_session_id = panel.auth.session_id();
                panel.entry.page = wave::EntryPage::workspace;
                session.start();
            }
        }
        session.update();
        if (session.take_schematic_change()) {
            panel.panel_controls.remove_id = 0;
            for (auto& [_, workspace] : panel.workspaces) {
                wave::abandon_config_workspace(workspace, session);
            }
            wave::close_config_json_editor();
            panel.workspaces.clear();
            session.abandon(panel.settings.core.request);
            panel.settings.core = {};
            panel.logs.hidden_sources.clear();
            session.add_local_log("info", "Reloaded saved configuration settings.");
        }
    }

    void update_theme_preference()
    {
        if (panel.entry.page != wave::EntryPage::workspace) {
            return;
        }
        // A reply settles the submitted choice. A newer selection must survive its snapshot.
        if (session.take_reply(theme_request) && theme_choice == submitted_theme) {
            theme_choice.reset();
        }
        if (!session.ready() || theme_request) {
            return;
        }

        if (!theme_choice) {
            const auto& settings = wave::view::field(session.data(), "settings");
            panel.appearance.preset = wave::theme::parse_preset(wave::view::string_field(settings, "theme"))
                                          .value_or(wave::theme::Preset::standard);
            return;
        }

        submitted_theme = *theme_choice;
        theme_request = session.submit(
            {{"action", "apply_theme"}, {"theme", wave::theme::preset_key(submitted_theme)}}, 5000);
        if (!theme_request) {
            theme_choice.reset();
        }
    }

    void update_config_controls()
    {
        if (displayed_config_links != panel.config_items) {
            auto links = nlohmann::json::array();
            for (const auto& item : panel.config_items) {
                links.push_back({{"id", -item.id}, {"name", item.name}});
            }
            update_config_links(links.dump().c_str(), static_cast<int>(wave::PanelPage::console));
            displayed_config_links = panel.config_items;
        }
        if (const auto* config = panel.config_workspace()) {
            const auto selected = std::ranges::find(config->tabs, config->selected);
            const int tab =
                selected == config->tabs.end() ? 0 : static_cast<int>(selected - config->tabs.begin());
            if (displayed_config_tab != tab || displayed_config_tabs != config->tabs) {
                auto labels = nlohmann::json::array();
                for (const auto& item : config->tabs) {
                    labels.push_back(item.label());
                }
                update_config_tabs(labels.dump().c_str(), tab);
                displayed_config_tabs = config->tabs;
                displayed_config_tab = tab;
            }
        } else if (displayed_config_tab >= 0) {
            update_config_tabs("[]", -1);
            displayed_config_tabs.clear();
            displayed_config_tab = -1;
        }
    }

    void frame()
    {
        const auto previous_entry = panel.entry.page;
        const auto previous_page = panel.page_control();
        update_session();
        update_theme_preference();
        // Use the browser viewport even when the canvas extends beyond it.
        ImVec2 browser_viewport;
        read_browser_viewport(&browser_viewport.x, &browser_viewport.y);
        const int viewport_width = static_cast<int>(browser_viewport.x);
        if (!fit_canvas(viewport_width)) {
            wave::finish_config_json_editor_frame();
            return;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
        }

        if (panel.appearance != appearance) {
            appearance = panel.appearance;
            apply_theme(appearance);
        }
        wave::theme::apply_text_scale(viewport_width);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        const auto previous_preset = panel.appearance.preset;
        wave::render_panel(panel, session, browser_viewport, browser_panel_focus());
        update_keyboard_state(panel.mini_keyboard_open, ImGui::IsMouseDown(ImGuiMouseButton_Left));
        wave::finish_config_json_editor_frame();
        if (panel.appearance.preset != previous_preset) {
            theme_choice = panel.appearance.preset;
        }
        // ImGui may spread the paste shortcut across frames to preserve input order.
        if (paste_target &&
            (paste_target != ImGui::GetActiveID() || ImGui::GetCurrentContext()->InputEventsQueue.empty())) {
            clear_pending_paste();
        }
        if (panel.page_control() != previous_page || panel.entry.page != previous_entry) {
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

        auto description = wave::describe_panel(panel, session);
        if (const auto help = wave::view::visible_help_description(); !help.empty()) {
            description += ' ';
            description += help;
        }
        if (description != displayed_description) {
            update_panel_description(description.c_str());
            displayed_description = std::move(description);
        }
        const bool in_workspace = panel.entry.page == wave::EntryPage::workspace;
        update_config_controls();
        update_navigation(in_workspace ? panel.page_control() : 0, panel.navigation_open && in_workspace,
                          in_workspace && panel.menu_open, in_workspace && panel.tools_open,
                          !in_workspace || wave::dialog_open(), wave::panel_donation_label(session));
        const auto& console = panel.console;
        update_console(in_workspace && panel.page == wave::PanelPage::console, console.left, console.top,
                       console.width, console.height);
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
    if (application.panel.entry.page == wave::EntryPage::workspace && !wave::dialog_open()) {
        application.panel.browser_control = control;
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void wave_document_visibility(bool visible)
{
    application.session.set_document_visible(visible);
}

extern "C" EMSCRIPTEN_KEEPALIVE void wave_activate_config_tab(int index)
{
    if (application.panel.entry.page != wave::EntryPage::workspace || wave::dialog_open() || index < 0) {
        return;
    }
    if (auto* config = application.panel.config_workspace();
        config && static_cast<std::size_t>(index) < config->tabs.size()) {
        config->requested = config->tabs[index];
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

extern "C" EMSCRIPTEN_KEEPALIVE bool wave_zoom_node_graph(float x, float y, float steps)
{
    auto* config = application.panel.config_workspace();
    return config && config->selected.kind == wave::ConfigWorkspaceTab::Kind::nodes &&
           !wave::panel_tool_window_at(application.panel, x, y) && config->nodes.zoom_at(x, y, steps);
}

extern "C" EMSCRIPTEN_KEEPALIVE bool wave_scroll_window_at(float x, float y)
{
    const auto within = [x, y](const wave::CanvasViewport& bounds) {
        return x >= bounds.left && x < bounds.left + bounds.width && y >= bounds.top &&
               y < bounds.top + bounds.height;
    };
    if (wave::dialog_at(x, y) || wave::panel_tool_window_at(application.panel, x, y) ||
        within(application.panel.settings.scroll_bounds) ||
        within(application.panel.settings.core.scroll_bounds) ||
        within(application.panel.config_nav_scroll_bounds)) {
        return true;
    }
    if (auto* config = application.panel.config_workspace()) {
        return within(wave::config_workspace_scroll_bounds(*config));
    }
    return false;
}

extern "C" EMSCRIPTEN_KEEPALIVE bool wave_topology_at(float x, float y)
{
    auto* config = application.panel.config_workspace();
    return config && config->selected.kind == wave::ConfigWorkspaceTab::Kind::nodes &&
           !wave::panel_tool_window_at(application.panel, x, y) && config->nodes.begin_touch(x, y);
}

extern "C" EMSCRIPTEN_KEEPALIVE void wave_pan_topology(float dx, float dy)
{
    if (auto* config = application.panel.config_workspace();
        config && config->selected.kind == wave::ConfigWorkspaceTab::Kind::nodes) {
        config->nodes.drag_touch(dx, dy);
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void wave_input_text(const char* text, bool paste)
{
    auto& io = ImGui::GetIO();
    if (paste) {
        const auto id = ImGui::GetActiveID();
        if (!ImGui::GetInputTextState(id)) {
            return;
        }
        // Use the text widget's paste path so it replaces the selection even while Ctrl is held.
        clear_pending_paste();
        pending_paste = text;
        paste_target = id;
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_V, true);
        io.AddKeyEvent(ImGuiKey_V, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
    } else {
        io.AddInputCharactersUTF8(text);
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void wave_text_input_key(int key_code)
{
    const auto* input = ImGui::GetInputTextState(ImGui::GetActiveID());
    if (!input || (input->Flags & ImGuiInputTextFlags_ReadOnly)) {
        return;
    }

    ImGuiKey key;
    switch (key_code) {
    case 0:
        ImGui::ClearActiveID();
        return;
    case 8:
        key = ImGuiKey_Backspace;
        break;
    case 13:
        key = ImGuiKey_Enter;
        break;
    default:
        return;
    }
    auto& io = ImGui::GetIO();
    io.AddKeyEvent(key, true);
    io.AddKeyEvent(key, false);
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
