cmake_minimum_required(VERSION 3.24)
project(Wave LANGUAGES CXX)

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
