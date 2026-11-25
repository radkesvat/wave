# Project Nayuki QR Code generator v1.8.0.
FetchContent_Declare(nayuki_qr
    URL https://codeload.github.com/nayuki/QR-Code-generator/tar.gz/refs/tags/v1.8.0
    URL_HASH SHA256=2ec0a4d33d6f521c942eeaf473d42d5fe139abcfa57d2beffe10c5cf7d34ae60
)
FetchContent_MakeAvailable(nayuki_qr)

target_sources(wave PRIVATE "${nayuki_qr_SOURCE_DIR}/cpp/qrcodegen.cpp")
target_include_directories(wave PRIVATE "${nayuki_qr_SOURCE_DIR}/cpp")
configure_file("${PROJECT_SOURCE_DIR}/doc/licenses/nayuki-qr.txt"
    "${CMAKE_BINARY_DIR}/site/licenses/nayuki-qr.txt" COPYONLY)
