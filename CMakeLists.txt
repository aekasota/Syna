cmake_minimum_required(VERSION 3.28)
project(Syna LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the engine DLL must not depend on a separately installed VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(SYNA_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg shared build (include/, lib/, bin/)")
if(NOT EXISTS "${SYNA_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg not found in ${SYNA_FFMPEG_ROOT}. Run scripts/fetch-deps.ps1.")
endif()

set(SYNA_FFMPEG_DLLS avcodec-62 avformat-62 avutil-60 swresample-6 swscale-9 avfilter-11)
# Only needed for speed changes (atempo) and thumbnails / exotic pixel formats: load on first use.
set(SYNA_FFMPEG_DELAYLOAD /DELAYLOAD:swscale-9.dll /DELAYLOAD:avfilter-11.dll)

add_subdirectory(src/Syna.Native)
add_subdirectory(tests/Syna.Native.Tests)
add_subdirectory(tests/Syna.Native.Bench)
