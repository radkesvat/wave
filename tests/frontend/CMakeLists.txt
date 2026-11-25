add_test(
    NAME frontend.overview
    COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/overview.test.cjs"
)
set_tests_properties(frontend.overview PROPERTIES
    ENVIRONMENT "WAVE_SITE_DIR=$<TARGET_FILE_DIR:wave>"
    LABELS frontend
    TIMEOUT 90
)
