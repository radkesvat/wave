cmake_minimum_required(VERSION 3.24)
project(Wave VERSION 0.0.2 LANGUAGES C CXX)

set(WAVE_VERSION_CHANNEL "beta")
set(WAVE_DISPLAY_VERSION "${PROJECT_VERSION}")
set(WAVE_RELEASE_VERSION "${PROJECT_VERSION}")
if(WAVE_VERSION_CHANNEL)
    string(APPEND WAVE_DISPLAY_VERSION " ${WAVE_VERSION_CHANNEL}")
    string(APPEND WAVE_RELEASE_VERSION "-${WAVE_VERSION_CHANNEL}")
endif()

configure_file(common/wave_version.h.in generated/wave_version.h @ONLY)

include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.12.0
    URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
)
FetchContent_MakeAvailable(nlohmann_json)

# Compile utf8proc into both targets with the same Unicode case folding data.
FetchContent_Declare(utf8proc
    URL https://codeload.github.com/JuliaStrings/utf8proc/tar.gz/refs/tags/v2.12.0
    URL_HASH SHA256=f564011d38b2888d583d510b08e69ffa15aa117155db1b9b49ef1dfe1fa25111
    SOURCE_SUBDIR wave-embedded
)
FetchContent_MakeAvailable(utf8proc)
add_library(wave-utf8proc STATIC "${utf8proc_SOURCE_DIR}/utf8proc.c" common/unicode_casefold.cpp)
target_include_directories(wave-utf8proc PRIVATE "${utf8proc_SOURCE_DIR}")
target_compile_definitions(wave-utf8proc PRIVATE UTF8PROC_STATIC)
target_compile_features(wave-utf8proc PRIVATE cxx_std_20)

if(EMSCRIPTEN)
    set(wave_component frontend)
else()
    set(wave_component backend)
endif()

add_subdirectory("${wave_component}")

include(CTest)
if(BUILD_TESTING)
    add_subdirectory("tests/${wave_component}")
endif()
