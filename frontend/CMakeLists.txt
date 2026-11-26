include(FetchContent)
# Dear ImGui v1.93.0. Pin the archive to both its commit and content hash.
FetchContent_Declare(imgui
    URL https://codeload.github.com/ocornut/imgui/tar.gz/c1aaac65e896a45f81f47a4d68f9959e0f86c859
    URL_HASH SHA256=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114
)
FetchContent_MakeAvailable(imgui)

set(WAVE_STARTUP_CANVAS 080C10)
set(WAVE_STARTUP_TEXT EAEBFF)
set(wave_shell "${CMAKE_CURRENT_BINARY_DIR}/shell.html")
configure_file(shell.html "${wave_shell}" @ONLY)
configure_file(theme_swatches.h.in "${CMAKE_CURRENT_BINARY_DIR}/theme_swatches.h" @ONLY)

add_executable(wave
    browser_ui.cpp
    main.cpp
    panel.cpp
    preview_session.cpp
    theme.cpp
    views/about_view.cpp
    views/bot_view.cpp
    views/console_view.cpp
    views/entry_view.cpp
    views/logs_view.cpp
    views/overview.cpp
    views/server_status_view.cpp
    views/settings_view.cpp
    views/topology_view.cpp
    views/users_view.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
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
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}/common"
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)
target_link_libraries(wave PRIVATE nlohmann_json::nlohmann_json)
include(qr_code.cmake)
target_compile_definitions(wave PRIVATE IMGUI_IMPL_OPENGL_ES3)
target_compile_options(wave PRIVATE
    -sUSE_SDL=2
    "$<$<CONFIG:Release>:-flto>"
)

# Poppins Medium 4.004 from google/fonts at 7dc16b7de42db624b902f2292b68ed9e489e5053.
set(wave_font "${CMAKE_CURRENT_SOURCE_DIR}/fonts/Poppins-Medium.ttf")
set_property(TARGET wave APPEND PROPERTY LINK_DEPENDS "${wave_shell}" "${wave_font}")
target_link_options(wave PRIVATE
    -sUSE_SDL=2
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    -lwebsocket.js
    "$<$<CONFIG:Release>:-flto>"
    "$<$<CONFIG:Release>:-sENVIRONMENT=web>"
    "SHELL:--shell-file \"${wave_shell}\""
    "SHELL:--embed-file \"${wave_font}@/poppins-medium.ttf\""
)
# Emscripten writes these assets when linking index.html.
add_custom_command(TARGET wave POST_BUILD
    BYPRODUCTS "${CMAKE_BINARY_DIR}/site/index.js" "${CMAKE_BINARY_DIR}/site/index.wasm"
    COMMAND "${CMAKE_COMMAND}" -E true
    VERBATIM
)

configure_file("${imgui_SOURCE_DIR}/LICENSE.txt" "${CMAKE_BINARY_DIR}/site/licenses/dear-imgui.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/site/licenses/wave.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/doc/licenses/poppins.txt" "${CMAKE_BINARY_DIR}/site/licenses/poppins.txt" COPYONLY)
configure_file("${nlohmann_json_SOURCE_DIR}/LICENSE.MIT" "${CMAKE_BINARY_DIR}/site/licenses/nlohmann-json.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/doc/licenses/nlohmann-json-third-party.txt"
    "${CMAKE_BINARY_DIR}/site/licenses/nlohmann-json-third-party.txt" COPYONLY)
