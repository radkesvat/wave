include(FetchContent)
set(wave_auth_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(wave_auth_default ON)
endif()
option(WAVE_ENABLE_AUTH "Build the HTTPS authenticated mode on Linux" ${wave_auth_default})
# Demo-only builds need neither system TLS nor compression libraries.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF)
set(HTTPLIB_REQUIRE_OPENSSL ${WAVE_ENABLE_AUTH})
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF)
set(HTTPLIB_INSTALL OFF)
FetchContent_Declare(httplib
    URL https://codeload.github.com/yhirose/cpp-httplib/tar.gz/refs/tags/v0.67.0
    URL_HASH SHA256=be711ddaf7a3655d8f23aa5655709250b24d247aedd4b2629989d18b6164e5be
)
FetchContent_MakeAvailable(httplib)

add_executable(wave-backend main.cpp panel_settings.cpp preview.cpp)
set_target_properties(wave-backend PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_include_directories(wave-backend PRIVATE "${PROJECT_SOURCE_DIR}/common")
target_link_libraries(wave-backend PRIVATE httplib::httplib nlohmann_json::nlohmann_json)

if(WAVE_ENABLE_AUTH)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "Wave authenticated mode requires Linux private state storage")
    endif()
    target_sources(wave-backend PRIVATE auth.cpp auth_store.cpp tls.cpp)
    target_compile_definitions(wave-backend PRIVATE WAVE_ENABLE_AUTH)
endif()
