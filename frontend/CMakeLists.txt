include(FetchContent)
option(WAVE_ENABLE_UI_DEMOS "Include the Dear ImGui and ImPlot demos in the Tools menu" OFF)

# Dear ImGui v1.93.0. Pin the archive to both its commit and content hash.
FetchContent_Declare(imgui
    URL https://codeload.github.com/ocornut/imgui/tar.gz/c1aaac65e896a45f81f47a4d68f9959e0f86c859
    URL_HASH SHA256=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114
)
FetchContent_MakeAvailable(imgui)

# ImPlot v1.1. Pin the archive to both its commit and content hash.
FetchContent_Declare(implot
    URL https://codeload.github.com/epezent/implot/tar.gz/819f200639455a58b6b3d4156b70e4271aeb2d00
    URL_HASH SHA256=75491b25e1027ed0d1c9b1cd1fdcfb94efabfc45328d4356461e96419833960d
)
FetchContent_MakeAvailable(implot)

# xterm.js 6.0.0 and its Fit addon 0.11.0, distributed as browser modules.
FetchContent_Declare(xterm
    URL https://registry.npmjs.org/@xterm/xterm/-/xterm-6.0.0.tgz
    URL_HASH SHA256=908e66e04af6c8dc6b00dd3b54de088e2e81e5ed866284fd6c2fb3c2d1c7a3f6
)
FetchContent_Declare(xterm_fit
    URL https://registry.npmjs.org/@xterm/addon-fit/-/addon-fit-0.11.0.tgz
    URL_HASH SHA256=26003b4517a132b64e4ff228fd88a5fda3fff5e606c76093f6dcff772e9ecec0
)
FetchContent_MakeAvailable(xterm xterm_fit)
configure_file(console.js "${CMAKE_BINARY_DIR}/site/console.js" COPYONLY)
configure_file("${xterm_SOURCE_DIR}/lib/xterm.mjs" "${CMAKE_BINARY_DIR}/site/terminal/xterm.js" COPYONLY)
configure_file("${xterm_SOURCE_DIR}/css/xterm.css" "${CMAKE_BINARY_DIR}/site/terminal/xterm.css" COPYONLY)
configure_file("${xterm_fit_SOURCE_DIR}/lib/addon-fit.mjs" "${CMAKE_BINARY_DIR}/site/terminal/addon-fit.js" COPYONLY)
configure_file("${xterm_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/site/licenses/xterm.txt" COPYONLY)
configure_file("${xterm_fit_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/site/licenses/xterm-addon-fit.txt" COPYONLY)

# Local browser bundles for the config JSON editor and its precision-preserving parser.
FetchContent_Declare(vanilla_jsoneditor
    URL https://registry.npmjs.org/vanilla-jsoneditor/-/vanilla-jsoneditor-3.13.1.tgz
    URL_HASH SHA256=342ae9cdf66ca505559ee0ce01451f0aa9851abe9c621bfd37b0a45e6ca84f84
)
FetchContent_Declare(lossless_json
    URL https://registry.npmjs.org/lossless-json/-/lossless-json-4.3.4.tgz
    URL_HASH SHA256=fbd483fdbb243aae96fd8e3a47ca6efa060ca67ec9aa5595d99a4ce25203805c
)
FetchContent_MakeAvailable(vanilla_jsoneditor lossless_json)
configure_file(json-editor.js "${CMAKE_BINARY_DIR}/site/json-editor.js" COPYONLY)
# These aliases belong to the pinned 3.13.1 standalone bundle. Text folding needs
# the CodeMirror effects from the same instance as the editor.
file(READ "${vanilla_jsoneditor_SOURCE_DIR}/standalone.js" wave_vanilla_jsoneditor_bundle)
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/site/json-editor/vanilla-jsoneditor.js" CONTENT [=[
@wave_vanilla_jsoneditor_bundle@
export { DG as EditorView, mX as ensureSyntaxTree, WX as foldNodeProp, HX as foldEffect,
         JX as unfoldEffect, GX as foldState };
]=] @ONLY)
configure_file("${vanilla_jsoneditor_SOURCE_DIR}/themes/jse-theme-dark.css"
    "${CMAKE_BINARY_DIR}/site/json-editor/jse-theme-dark.css" COPYONLY)
configure_file("${lossless_json_SOURCE_DIR}/lib/umd/lossless-json.js"
    "${CMAKE_BINARY_DIR}/site/json-editor/lossless-json.js" COPYONLY)
configure_file("${vanilla_jsoneditor_SOURCE_DIR}/LICENSE.md"
    "${CMAKE_BINARY_DIR}/site/licenses/vanilla-jsoneditor.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/doc/licenses/vanilla-jsoneditor-third-party.txt"
    "${CMAKE_BINARY_DIR}/site/licenses/vanilla-jsoneditor-third-party.txt" COPYONLY)
configure_file("${lossless_json_SOURCE_DIR}/LICENSE.md"
    "${CMAKE_BINARY_DIR}/site/licenses/lossless-json.txt" COPYONLY)

set(WAVE_STARTUP_CANVAS 080C10)
set(WAVE_STARTUP_TEXT EAEBFF)
set(wave_shell "${CMAKE_CURRENT_BINARY_DIR}/shell.html")
configure_file(shell.html "${wave_shell}" @ONLY)
configure_file(theme_swatches.h.in "${CMAKE_CURRENT_BINARY_DIR}/theme_swatches.h" @ONLY)

add_executable(wave
    "${PROJECT_SOURCE_DIR}/common/waterwall_schematic.cpp"
    auth_session.cpp
    browser_ui.cpp
    dialog.cpp
    main.cpp
    panel.cpp
    preview_session.cpp
    users_roster.cpp
    theme.cpp
    views/about_view.cpp
    views/config_json_import.cpp
    views/config_nodes_view.cpp
    views/config_users_view.cpp
    views/config_workspace_view.cpp
    views/console_view.cpp
    views/core_settings_view.cpp
    views/donation_face.cpp
    views/panel_controls_view.cpp
    views/entry_view.cpp
    views/logs_view.cpp
    views/server_status_plot.cpp
    views/server_status_view.cpp
    views/settings_view.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)
if(WAVE_ENABLE_UI_DEMOS)
    target_sources(wave PRIVATE
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${implot_SOURCE_DIR}/implot_demo.cpp
    )
    target_compile_definitions(wave PRIVATE WAVE_ENABLE_UI_DEMOS)
endif()

set_target_properties(wave PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/site"
    OUTPUT_NAME index
    SUFFIX .html
)
target_include_directories(wave PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/common"
    "${PROJECT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
    "${implot_SOURCE_DIR}"
)
target_link_libraries(wave PRIVATE nlohmann_json::nlohmann_json wave-utf8proc)
# The WebGL renderer needs 32-bit indices for large meshes in the ImPlot demo.
target_compile_definitions(wave PRIVATE IMGUI_IMPL_OPENGL_ES3 IMGUI_USE_WCHAR32 "ImDrawIdx=unsigned int")
target_compile_options(wave PRIVATE
    -fexceptions
    -sUSE_SDL=2
    "$<$<CONFIG:Release>:-flto>"
)

# Poppins Medium 4.004 from google/fonts at 7dc16b7de42db624b902f2292b68ed9e489e5053.
set(wave_font "${CMAKE_CURRENT_SOURCE_DIR}/fonts/Poppins-Medium.ttf")
set(wave_donation_face "${CMAKE_CURRENT_SOURCE_DIR}/assets/gc1.bmp")
set(wave_donation_portrait "${CMAKE_CURRENT_SOURCE_DIR}/assets/gc2.bmp")
set_property(TARGET wave APPEND PROPERTY LINK_DEPENDS
    "${wave_shell}" "${wave_font}" "${wave_donation_face}" "${wave_donation_portrait}")
target_link_options(wave PRIVATE
    -fexceptions
    -sUSE_SDL=2
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    # Keep the initial heap small and allow concurrently staged and committed rosters.
    -sALLOW_MEMORY_GROWTH=1
    -sMAXIMUM_MEMORY=268435456
    -lwebsocket.js
    "$<$<CONFIG:Release>:-flto>"
    "$<$<CONFIG:Release>:-sENVIRONMENT=web>"
    "SHELL:--shell-file \"${wave_shell}\""
    "SHELL:--embed-file \"${wave_font}@/poppins-medium.ttf\""
    "SHELL:--embed-file \"${wave_donation_face}@/gc1.bmp\""
    "SHELL:--embed-file \"${wave_donation_portrait}@/gc2.bmp\""
)
# Emscripten writes these assets when linking index.html.
add_custom_command(TARGET wave POST_BUILD
    BYPRODUCTS "${CMAKE_BINARY_DIR}/site/index.js" "${CMAKE_BINARY_DIR}/site/index.wasm"
    COMMAND "${CMAKE_COMMAND}" -E true
    VERBATIM
)

configure_file("${imgui_SOURCE_DIR}/LICENSE.txt" "${CMAKE_BINARY_DIR}/site/licenses/dear-imgui.txt" COPYONLY)
configure_file("${implot_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/site/licenses/implot.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/site/licenses/wave.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/doc/licenses/poppins.txt" "${CMAKE_BINARY_DIR}/site/licenses/poppins.txt" COPYONLY)
configure_file("${nlohmann_json_SOURCE_DIR}/LICENSE.MIT" "${CMAKE_BINARY_DIR}/site/licenses/nlohmann-json.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/doc/licenses/nlohmann-json-third-party.txt"
    "${CMAKE_BINARY_DIR}/site/licenses/nlohmann-json-third-party.txt" COPYONLY)
configure_file("${utf8proc_SOURCE_DIR}/LICENSE.md" "${CMAKE_BINARY_DIR}/site/licenses/utf8proc.txt" COPYONLY)
