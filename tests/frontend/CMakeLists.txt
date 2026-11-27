foreach(suite IN ITEMS panel config users)
    add_test(
        NAME frontend.workspace.${suite}
        COMMAND node --test "${CMAKE_CURRENT_SOURCE_DIR}/workspace-${suite}.test.cjs"
    )
    set_tests_properties(frontend.workspace.${suite} PROPERTIES
        ENVIRONMENT "WAVE_SITE_DIR=$<TARGET_FILE_DIR:wave>;WAVE_EXPECT_UI_DEMOS=$<BOOL:${WAVE_ENABLE_UI_DEMOS}>"
        LABELS frontend
        TIMEOUT 90
    )
endforeach()
