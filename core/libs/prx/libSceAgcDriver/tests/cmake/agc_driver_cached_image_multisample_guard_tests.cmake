if(BUILD_TESTING)
    add_test(NAME agc_driver_cached_image_multisample_guard_no_fast COMMAND agc_driver_cached_image_multisample_guard_tests)
    set_tests_properties(agc_driver_cached_image_multisample_guard_no_fast PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_FAST_REVALIDATE=1"
            SKIP_RETURN_CODE 77
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_cached_image_multisample_guard_no_fast PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
