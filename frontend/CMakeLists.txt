include(FetchContent)
# Dear ImGui v1.92.5. Pin the archive to both its commit and content hash.
FetchContent_Declare(imgui
    URL https://codeload.github.com/ocornut/imgui/tar.gz/6d910d5487d11ca567b61c7824b0c78c569d62f0
    URL_HASH SHA256=b715a3226320aaebaea79dc6af6405ae37cdcb479817fddaef12a17d8bd15a3a
)
FetchContent_MakeAvailable(imgui)

add_executable(wave
    main.cpp
    overview.cpp
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
target_include_directories(wave PRIVATE "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(wave PRIVATE IMGUI_IMPL_OPENGL_ES3)
target_compile_options(wave PRIVATE
    -sUSE_SDL=2
    "$<$<CONFIG:Release>:-flto>"
)

set(wave_shell "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
set(wave_font "${imgui_SOURCE_DIR}/misc/fonts/Karla-Regular.ttf")
set_property(TARGET wave APPEND PROPERTY LINK_DEPENDS "${wave_shell}" "${wave_font}")
target_link_options(wave PRIVATE
    -sUSE_SDL=2
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    "$<$<CONFIG:Release>:-flto>"
    "$<$<CONFIG:Release>:-sENVIRONMENT=web>"
    "SHELL:--shell-file \"${wave_shell}\""
    "SHELL:--embed-file \"${wave_font}@/karla.ttf\""
)
# Emscripten writes these assets when linking index.html.
add_custom_command(TARGET wave POST_BUILD
    BYPRODUCTS "${CMAKE_BINARY_DIR}/site/index.js" "${CMAKE_BINARY_DIR}/site/index.wasm"
    COMMAND "${CMAKE_COMMAND}" -E true
    VERBATIM
)

configure_file("${imgui_SOURCE_DIR}/LICENSE.txt" "${CMAKE_BINARY_DIR}/site/licenses/dear-imgui.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/doc/licenses/karla.txt" "${CMAKE_BINARY_DIR}/site/licenses/karla.txt" COPYONLY)
