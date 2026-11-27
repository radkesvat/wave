add_test(
    NAME backend.smoke
    COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/backend.test.cjs"
)
add_test(NAME backend.console COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/console.test.cjs")
set_tests_properties(backend.smoke backend.console PROPERTIES
    ENVIRONMENT "WAVE_BACKEND=$<TARGET_FILE:wave-backend>"
    LABELS backend
)
# Allow the full smoke suite to finish when the backend is built without optimization.
set_tests_properties(backend.smoke PROPERTIES TIMEOUT "$<IF:$<CONFIG:Debug>,90,45>")
set_tests_properties(backend.console PROPERTIES TIMEOUT 45)

if(WAVE_ENABLE_AUTH)
    add_test(NAME backend.auth COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/auth.test.cjs")
    add_test(NAME backend.tls COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/tls.test.cjs")
    set_tests_properties(backend.auth backend.tls PROPERTIES
        ENVIRONMENT "WAVE_BACKEND=$<TARGET_FILE:wave-backend>"
        LABELS backend
        TIMEOUT 60
    )
endif()
