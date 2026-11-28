foreach(suite IN ITEMS panel config users users-live)
    add_test(
        NAME frontend.workspace.${suite}
        COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/workspace-${suite}.test.cjs"
    )
    # Allow the full Chromium suite to run on hosted runners.
    set_tests_properties(frontend.workspace.${suite} PROPERTIES
        ENVIRONMENT "WAVE_SITE_DIR=$<TARGET_FILE_DIR:wave>;WAVE_EXPECT_UI_DEMOS=$<BOOL:${WAVE_ENABLE_UI_DEMOS}>"
        LABELS frontend
        TIMEOUT 300
    )
endforeach()

set_property(TEST frontend.workspace.panel APPEND PROPERTY
    ENVIRONMENT "WAVE_EXPECT_PANEL_VERSION=${WAVE_DISPLAY_VERSION}"
)
