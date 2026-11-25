cmake_minimum_required(VERSION 3.24)
project(WaveBackend LANGUAGES CXX)

include(FetchContent)
# Keep the preview build independent of system TLS and compression libraries.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF)
set(HTTPLIB_INSTALL OFF)
FetchContent_Declare(httplib
    URL https://codeload.github.com/yhirose/cpp-httplib/tar.gz/refs/tags/v0.67.0
    URL_HASH SHA256=be711ddaf7a3655d8f23aa5655709250b24d247aedd4b2629989d18b6164e5be
)
FetchContent_MakeAvailable(httplib)

add_executable(wave-backend main.cpp)
set_target_properties(wave-backend PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_link_libraries(wave-backend PRIVATE httplib::httplib)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory("../tests/backend" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
