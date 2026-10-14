# Engine benchmark (scripts/benchmark.ps1). Uses the shipped DLL through the public C ABI only.
add_executable(syna_bench bench.cpp)
target_include_directories(syna_bench PRIVATE ${CMAKE_SOURCE_DIR}/src/Syna.Native/include)
target_compile_definitions(syna_bench PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(syna_bench PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
target_link_libraries(syna_bench PRIVATE Syna.Native)
