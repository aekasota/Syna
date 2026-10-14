file(GLOB SYNA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_executable(syna_native_tests ${SYNA_TEST_SOURCES} $<TARGET_OBJECTS:syna_engine_obj>)
target_link_libraries(syna_native_tests PRIVATE syna_engine_iface)
target_compile_definitions(syna_native_tests PRIVATE SYNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_options(syna_native_tests PRIVATE ${SYNA_FFMPEG_DELAYLOAD})
foreach(dll ${SYNA_FFMPEG_DLLS})
    add_custom_command(TARGET syna_native_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SYNA_FFMPEG_ROOT}/bin/${dll}.dll $<TARGET_FILE_DIR:syna_native_tests>)
endforeach()
