cmake_minimum_required(VERSION 3.24)
project(Wave VERSION 1.6.0 LANGUAGES CXX)

configure_file(common/wave_version.h.in generated/wave_version.h @ONLY)

include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.12.0
    URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
)
FetchContent_MakeAvailable(nlohmann_json)

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
