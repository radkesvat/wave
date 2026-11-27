include(FetchContent)
set(wave_auth_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(wave_auth_default ON)
endif()
option(WAVE_ENABLE_AUTH "Enable native HTTPS support" ${wave_auth_default})
# Authentication uses OpenSSL Crypto on both HTTP and HTTPS builds.
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(httplib_websocket_close.cmake)
endif()

enable_language(C)
# Argon2 reference implementation, release 20190702.
FetchContent_Declare(argon2
    URL https://codeload.github.com/P-H-C/phc-winner-argon2/tar.gz/refs/tags/20190702
    URL_HASH SHA256=daf972a89577f8772602bf2eb38b6a3dd3d922bf5724d45e7f9589b5e830442c
)
FetchContent_MakeAvailable(argon2)
find_package(Threads REQUIRED)
add_library(wave-argon2 STATIC
    ${argon2_SOURCE_DIR}/src/argon2.c
    ${argon2_SOURCE_DIR}/src/core.c
    ${argon2_SOURCE_DIR}/src/encoding.c
    ${argon2_SOURCE_DIR}/src/ref.c
    ${argon2_SOURCE_DIR}/src/thread.c
    ${argon2_SOURCE_DIR}/src/blake2/blake2b.c
)
target_include_directories(wave-argon2 PUBLIC "${argon2_SOURCE_DIR}/include")
target_link_libraries(wave-argon2 PRIVATE Threads::Threads)

# zxcvbn-c v2.7. Embed its dictionary in the backend for offline password checks.
FetchContent_Declare(zxcvbn
    URL https://codeload.github.com/tsyrogit/zxcvbn-c/tar.gz/refs/tags/v2.7
    URL_HASH SHA256=0548647d8cda5c0d52e6e00cd6896005f2813869f9ed93c245536129e3bd3f4b
    SOURCE_SUBDIR wave-embedded
)
FetchContent_MakeAvailable(zxcvbn)
add_executable(wave-zxcvbn-dict "${zxcvbn_SOURCE_DIR}/dict-generate.cpp")
target_compile_features(wave-zxcvbn-dict PRIVATE cxx_std_11)
set(zxcvbn_words
    words-eng_wiki.txt words-female.txt words-male.txt words-passwd.txt words-surname.txt words-tv_film.txt
)
list(TRANSFORM zxcvbn_words PREPEND "${zxcvbn_SOURCE_DIR}/")
set(zxcvbn_dictionary "${CMAKE_CURRENT_BINARY_DIR}/dict-src.h")
add_custom_command(
    OUTPUT "${zxcvbn_dictionary}"
    COMMAND wave-zxcvbn-dict -o "${zxcvbn_dictionary}" ${zxcvbn_words}
    DEPENDS wave-zxcvbn-dict ${zxcvbn_words}
    VERBATIM
)
add_library(wave-zxcvbn STATIC "${zxcvbn_SOURCE_DIR}/zxcvbn.c" "${zxcvbn_dictionary}")
target_include_directories(wave-zxcvbn PUBLIC "${zxcvbn_SOURCE_DIR}" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(UNIX)
    target_link_libraries(wave-zxcvbn PRIVATE m)
endif()

add_executable(wave-backend
    main.cpp panel_settings.cpp preview.cpp linux_metrics.cpp auth.cpp auth_store.cpp
    waterwall.cpp waterwall_install.cpp waterwall_logs.cpp waterwall_users.cpp
    core_config_files.cpp core_config_graph.cpp core_config_users.cpp console.cpp
)
set_target_properties(wave-backend PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_include_directories(wave-backend PRIVATE "${PROJECT_SOURCE_DIR}/common" "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(wave-backend PRIVATE
    httplib::httplib nlohmann_json::nlohmann_json OpenSSL::Crypto Threads::Threads wave-argon2 wave-zxcvbn
)
if(WIN32)
    target_link_libraries(wave-backend PRIVATE advapi32)
endif()

if(WAVE_ENABLE_AUTH)
    if(NOT UNIX)
        message(FATAL_ERROR "Wave native HTTPS support requires POSIX PEM file handling")
    endif()
    target_sources(wave-backend PRIVATE tls.cpp)
    target_compile_definitions(wave-backend PRIVATE WAVE_ENABLE_AUTH)
endif()
