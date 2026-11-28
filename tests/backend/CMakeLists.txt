add_test(
    NAME backend.smoke
    COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/backend.test.cjs"
)
add_test(NAME backend.console COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/console.test.cjs")
add_test(NAME backend.users-transport COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/users-transport.test.cjs")
add_test(NAME backend.users-roster-saved COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/users-roster-saved.test.cjs")
add_test(NAME backend.backup COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/backup.test.cjs")
set_tests_properties(backend.smoke backend.console backend.users-transport backend.users-roster-saved backend.backup PROPERTIES
    ENVIRONMENT "WAVE_BACKEND=$<TARGET_FILE:wave-backend>"
    LABELS backend
)
# The full smoke suite includes process and catalog fixtures that wait for scheduled checks.
set_tests_properties(backend.smoke PROPERTIES TIMEOUT "$<IF:$<CONFIG:Debug>,90,60>")
set_tests_properties(backend.console backend.users-transport backend.users-roster-saved backend.backup PROPERTIES TIMEOUT 45)

if(WAVE_ENABLE_AUTH)
    add_test(NAME backend.auth COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/auth.test.cjs")
    add_test(NAME backend.tls COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/tls.test.cjs")
    set_tests_properties(backend.auth backend.tls PROPERTIES
        ENVIRONMENT "WAVE_BACKEND=$<TARGET_FILE:wave-backend>"
        LABELS backend
        TIMEOUT 60
    )
endif()
