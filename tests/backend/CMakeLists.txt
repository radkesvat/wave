add_test(
    NAME backend.smoke
    COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/backend.test.cjs"
)
set_tests_properties(backend.smoke PROPERTIES
    ENVIRONMENT "WAVE_BACKEND=$<TARGET_FILE:wave-backend>"
    LABELS backend
    TIMEOUT 45
)
