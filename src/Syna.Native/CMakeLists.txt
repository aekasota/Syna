# The FFmpeg build ships GNU-style import libraries, which MSVC cannot delay-load. Generate
# native import libraries from the .def files instead.
add_library(syna_ffmpeg INTERFACE)
target_include_directories(syna_ffmpeg SYSTEM INTERFACE ${SYNA_FFMPEG_ROOT}/include)
set(SYNA_FFMPEG_IMPLIB_DIR ${CMAKE_BINARY_DIR}/ffmpeg-implib)
file(MAKE_DIRECTORY ${SYNA_FFMPEG_IMPLIB_DIR})
foreach(dll ${SYNA_FFMPEG_DLLS})
    string(REGEX REPLACE "-[0-9]+$" "" lib ${dll})
    set(implib ${SYNA_FFMPEG_IMPLIB_DIR}/${lib}.lib)
    if(NOT EXISTS ${implib})
        execute_process(
            COMMAND ${CMAKE_AR} /nologo /machine:x64 /def:${SYNA_FFMPEG_ROOT}/lib/${dll}.def /out:${implib}
            RESULT_VARIABLE implib_result OUTPUT_QUIET)
        if(NOT implib_result EQUAL 0)
            message(FATAL_ERROR "Failed to create import library for ${dll}")
        endif()
    endif()
    target_link_libraries(syna_ffmpeg INTERFACE ${implib})
endforeach()

# Shaders are compiled at build time into headers (no runtime d3dcompiler dependency).
find_program(SYNA_FXC fxc REQUIRED)
set(SYNA_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(SYNA_SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/video.hlsl)
set(SYNA_SHADER_HEADERS)
foreach(spec "video_vs;vs_5_0;vs_main" "video_ps;ps_5_0;ps_main")
    list(GET spec 0 name)
    list(GET spec 1 profile)
    list(GET spec 2 entry)
    add_custom_command(
        OUTPUT ${SYNA_SHADER_DIR}/${name}.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SYNA_SHADER_DIR}
        COMMAND ${SYNA_FXC} /nologo /O3 /Ges /T ${profile} /E ${entry} /Vn g_${name} /Fh ${SYNA_SHADER_DIR}/${name}.h ${SYNA_SHADER_SRC}
        DEPENDS ${SYNA_SHADER_SRC}
        COMMENT "fxc ${name}")
    list(APPEND SYNA_SHADER_HEADERS ${SYNA_SHADER_DIR}/${name}.h)
endforeach()

add_library(syna_engine_iface INTERFACE)
target_include_directories(syna_engine_iface INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SYNA_SHADER_DIR})
target_compile_definitions(syna_engine_iface INTERFACE SYNA_BUILDING_DLL UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(syna_engine_iface INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /external:W0 /wd4324 $<$<CONFIG:Release>:/Zi>)
target_link_libraries(syna_engine_iface INTERFACE
    syna_ffmpeg d3d11 dxgi dxguid dwmapi ole32 oleaut32 uuid avrt windowscodecs pdh delayimp)

file(GLOB_RECURSE SYNA_ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(syna_engine_obj OBJECT ${SYNA_ENGINE_SOURCES} ${SYNA_SHADER_HEADERS})
target_link_libraries(syna_engine_obj PUBLIC syna_engine_iface)

add_library(Syna.Native SHARED $<TARGET_OBJECTS:syna_engine_obj>)
target_link_libraries(Syna.Native PRIVATE syna_engine_iface)
target_link_options(Syna.Native PRIVATE ${SYNA_FFMPEG_DELAYLOAD} /DEBUG:FULL /OPT:REF /OPT:ICF)

# Ship the FFmpeg runtime next to the engine.
foreach(dll ${SYNA_FFMPEG_DLLS})
    add_custom_command(TARGET Syna.Native POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SYNA_FFMPEG_ROOT}/bin/${dll}.dll $<TARGET_FILE_DIR:Syna.Native>)
endforeach()
